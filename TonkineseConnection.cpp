/** \file
    \brief Tonk Implementation: Connection class
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonk nor the names of its contributors may be
      used to endorse or promote products derived from this software without
      specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
*/

#include "TonkineseConnection.h"

namespace tonk {

static logger::Channel ModuleLogger("Connection", MinimumLogLevel);


//------------------------------------------------------------------------------
// Tools

static std::string GetConnectionLogPrefix(const std::string& hostname, uint16_t port)
{
    std::ostringstream oss;
    oss << "[" << hostname << ":" << port << "] ";
    return oss.str();
}


//------------------------------------------------------------------------------
// DNS Hostname Resolution

void HostnameResolverState::InitializeResolvedAddressList(
    const asio::ip::tcp::resolver::results_type& results)
{
    // Copy the resolved addresses
    ResolvedIPAddresses.clear();
    ResolvedIPAddresses.reserve(results.size());
    for (const auto& result : results) {
        ResolvedIPAddresses.emplace_back(result.endpoint().address());
    }
    ConnectRequestsRemaining = (unsigned)ResolvedIPAddresses.size();

    // Select the first address to connect to at random
    siamese::PCGRandom prng;
    prng.Seed(siamese::GetTimeUsec());
    ConnectAddrIndex = static_cast<int>(prng.Next() % ConnectRequestsRemaining);
}

Result HostnameResolverState::GetNextServerAddress(asio::ip::address& addr)
{
    // If there are no TCP addresses resolved:
    if (ResolvedIPAddresses.empty()) {
        return Result("Connection failed", "Hostname did not resolve", ErrorType::Tonk, Tonk_ConnectionTimeout);
    }

    // Try the next connection address
    if (++ConnectAddrIndex >= static_cast<unsigned>(ResolvedIPAddresses.size())) {
        ConnectAddrIndex = 0;
    }

    // Try once per server in the list
    if (ConnectRequestsRemaining <= 0) {
        return Result("Connection failed", "No servers responded", ErrorType::Tonk, Tonk_ConnectionTimeout);
    }
    --ConnectRequestsRemaining;

    addr = ResolvedIPAddresses[ConnectAddrIndex];
    if (addr.is_v4()) {
        return Result::Success();
    }

    try
    {
        // Workaround fix: IPv6 loopback is not handled properly by asio to_v4()
        if (addr.is_loopback())
        {
            addr = asio::ip::address_v4::loopback();
            return Result::Success();
        }

        addr = addr.to_v4();
    }
    catch (...)
    {
    }

    // Unable to convert, keep as IPv6
    return Result::Success();
}


//------------------------------------------------------------------------------
// Connection

Connection::Connection()
    : Logger("Connection", MinimumLogLevel)
{
    ActiveConnectionCount++;
}

Connection::~Connection()
{
    ActiveConnectionCount--;
}

Result Connection::InitializeIncomingConnection(
    const Dependencies& deps,
    const TonkSocketConfig* socketConfig,
    uint64_t encryptionKey,
    const UDPAddress& peerAddress,
    uint64_t receiveUsec,
    DatagramBuffer datagram)
{
    /*
        It is one of the goals of this function to stay as simple as possible
        because this code runs in the datagram processing thread and blocks
        processing the datagrams from other peers.
    */

    Result result;

    try
    {
        Deps = deps;
        SocketConfig = socketConfig;
        EncryptionKey = encryptionKey;

        // Add reference to socket
        Deps.SocketRefCount->IncrementReferences();

        AsioEventStrand = MakeUniqueNoThrow<asio::io_context::strand>(*Deps.Context);
        if (!AsioEventStrand)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        SelfRefCount.IncrementReferences();
        AsioEventStrand->post([this, receiveUsec, peerAddress, datagram]()
        {
            Result initResult = completeIncomingConnection(peerAddress);

            if (initResult.IsGood())
            {
                // Process first datagram
                TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
                Result result = Incoming.ProcessDatagram(
                    receiveUsec,
                    peerAddress,
                    datagram.Data,
                    datagram.Bytes);
                if (result.IsFail()) {
                    onDecodeFailure(result);
                }
            }

            datagram.Free();

            if (initResult.IsFail())
            {
                // Send connection rejection:
                DatagramBuffer datagramBuffer;
                datagramBuffer.AllocatorForFree = Outgoing.GetWriteAllocator();
                datagramBuffer.Bytes = protocol::kDisconnectBytes;
                datagramBuffer.Data = datagramBuffer.AllocatorForFree->Allocate(protocol::kDisconnectBytes);
                datagramBuffer.FreePtr = datagramBuffer.Data;
                if (datagramBuffer.Data)
                {
                    protocol::GenerateDisconnect(EncryptionKey, datagramBuffer.Data);
                    Deps.UDPSender->Send(
                        peerAddress,
                        datagramBuffer,
                        &SelfRefCount);

                    // Let Send() decrement SelfRefCount
                    return;
                }
            }

            SelfRefCount.DecrementReferences();
        });
    }
    catch (...)
    {
        TONK_DEBUG_BREAK(); // Should never happen except out of memory error
        result = Result("Connection::InitializeIncomingConnection", "Exception!", ErrorType::Tonk);
        goto OnError;
    }

    return result;

OnError:
    Shutdown();

    return result;
}

Result Connection::completeIncomingConnection(const UDPAddress& peerUDPAddress)
{
    Result result;

    try
    {
        Logger.SetPrefix(GetConnectionLogPrefix(
            peerUDPAddress.address().to_string(),
            peerUDPAddress.port()));

        result = initSubsystems();
        if (result.IsFail()) {
            goto OnError;
        }

        // Update the peer address for outgoing data
        Outgoing.SetPeerUDPAddress(peerUDPAddress);

        // Initialize timers for incoming connection
        resetTimers(SocketConfig->ConnectionTimeoutUsec, true);

        /*
            This is the right time to assign a Connection Id, before any
            messages are queued for delivery to the peer.

            The Connection Id is used to reconnect if the source IP address or
            port changes during a connection.  Both client and server in a
            connection are assigned a Connection Id by the other side.
            And both peers are able to change their source address by sending
            a valid datagram with the ID flag set.
        */
        if (!Deps.IdMap->InsertAndAssignId(LocallyAssignedIdForRemoteHost, this))
        {
            // Id assignment can only fail if too many clients were allowed to
            // connect, which should not be possible.
            TONK_DEBUG_BREAK();
            Logger.Warning("completeConnectionInitialization: Failed to insert new connection into id map");
        }

        // Write control channel data before the OnConnect() callback so that
        // any tonk_flush() will include the control channel data in the first
        // UDP response datagram
        queueAddressAndIdChangeMessage(peerUDPAddress);

        TonkAddress appAddress;
        SetTonkAddressStr(appAddress, peerUDPAddress);

        // FIXME: If this asserts we currently seem to shut down uncleanly
        const uint32_t appDecision = SocketConfig->OnIncomingConnection(
            SocketConfig->AppContextPtr,
            GetTonkConnection(),
            &appAddress,
            &ConnectionConfig);
        if (TONK_DENY_CONNECTION == appDecision)
        {
            result = Result("Connection::completeConnectionInitialization", "Application OnIncomingConnection() callback denied connection", ErrorType::Tonk);
            goto OnError;
        }

        MakeCallbacksOptional(ConnectionConfig);

        // TBD: We could call OnConnect() here and some apps may have lower
        // latency to establish connections.  It is already possible to react
        // with the same speed to the OnIncomingConnection() callback though.
        // Right now OnConnect() is called when the first ID update control
        // message is received from the client.
        //ConnectionConfig.OnConnect(ConnectionConfig.AppContextPtr, GetTonkConnection());

        /*
            For the server, we do not want to delay delivery of the client's ID
            until the next timed or application flush, so perform a flush here.
            This still allows the application to submit data in the first round.
        */
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());

        // Buffer enough flush space for the first connection request message
        Result flushResult = Outgoing.Flush();
        if (flushResult.IsFail()) {
            SelfRefCount.StartShutdown(Tonk_Error, flushResult);
        }

        postNextTimer();
    }
    catch (...)
    {
        TONK_DEBUG_BREAK(); // Should never happen except out of memory error
        result = Result("Connection::completeConnectionInitialization", "Exception!", ErrorType::Tonk);
        goto OnError;
    }

OnError:
    if (result.IsFail())
    {
        Logger.Warning("Connection::completeIncomingConnection failed: ", result.ToJson());
        Shutdown();
    }

    return result;
}

Result Connection::InitializeOutgoingConnection(
    const Dependencies& deps,
    const TonkSocketConfig* socketConfig,
    const TonkConnectionConfig& connectionConfig,
    const char* serverHostname,
    uint16_t serverPort)
{
    Result result;

    try
    {
        Deps = deps;
        SocketConfig = socketConfig;
        ConnectionConfig = connectionConfig;

        // Add reference to socket
        Deps.SocketRefCount->IncrementReferences();

        Logger.SetPrefix(GetConnectionLogPrefix(serverHostname, serverPort));

        AsioEventStrand = MakeUniqueNoThrow<asio::io_context::strand>(*Deps.Context);
        if (!AsioEventStrand)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        ResolverState = MakeUniqueNoThrow<HostnameResolverState>();
        ResolverState->ServerUDPPort = serverPort;
        ResolverState->Resolver = MakeUniqueNoThrow<asio::ip::tcp::resolver>(*Deps.Context);

#ifdef TONK_DISABLE_OUTGOING_ENCRYPTION
        EncryptionKey = 0;
#else // TONK_DISABLE_OUTGOING_ENCRYPTION
        uint8_t keyBytes[8];
        result = SecureRandom_Next(keyBytes, 8);
        if (result.IsFail()) {
            goto OnError;
        }
        EncryptionKey = siamese::ReadU64_LE(keyBytes);
#endif // TONK_DISABLE_OUTGOING_ENCRYPTION

        result = initSubsystems();
        if (result.IsFail()) {
            goto OnError;
        }

        // Start attaching handshake to each outgoing datagram
        Outgoing.StartHandshakeC2SConnect(EncryptionKey);

        // Initialize timers for outgoing connection
        resetTimers(SocketConfig->ConnectionTimeoutUsec, false);

        //----------------------------------------------------------------------
        // Connection creation cannot fail past this point
        //----------------------------------------------------------------------

        // Allow any protocol
        static const std::string kAnyProtocolStr = "";

        // Kick off async resolve after initialization
        SelfRefCount.IncrementReferences();
        ResolverState->Resolver->async_resolve(serverHostname, kAnyProtocolStr,
            [this](const asio::error_code& error, const asio::ip::tcp::resolver::results_type& results)
        {
            onResolve(error, results);
            SelfRefCount.DecrementReferences();
        });

        /*
            This is the right time to assign a Connection Id, before any
            messages are queued for delivery to the peer.

            The Connection Id is used to reconnect if the source IP address or
            port changes during a connection.  Both client and server in a
            connection are assigned a Connection Id by the other side.
            And both peers are able to change their source address by sending
            a valid datagram with the ID flag set.
        */
        if (!Deps.IdMap->InsertAndAssignId(LocallyAssignedIdForRemoteHost, this))
        {
            // Id assignment can only fail if too many clients were allowed to
            // connect, which should not be possible.
            TONK_DEBUG_BREAK();
            Logger.Warning("Failed to insert new connection into map");
        }

        postNextTimer();

        /*
            Do not immediately flush queued messages - The application decides
            when to flush the initial data so that it can be included in the
            first connection request datagram to the server.
        */
    }
    catch (...)
    {
        TONK_DEBUG_BREAK(); // Should never happen except out of memory error
        result = Result("Connection::InitializeOutgoingConnection", "Exception!", ErrorType::Tonk);
    }

OnError:
    if (result.IsFail())
    {
        Logger.Warning("Connection::InitializeOutgoingConnection failed: ", result.ToJson());
        Shutdown();
    }

    return result;
}

Result Connection::InitializeP2PConnection(
    const Dependencies& deps,
    const TonkSocketConfig* socketConfig,
    uint32_t rendezvousServerConnectionId,
    const protocol::P2PConnectParams& startParams)
{
    /*
        It is one of the goals of this function to stay as simple as possible
        because this code runs in the datagram processing thread and blocks
        processing the datagrams from other peers.
    */

    Result result;

    try
    {
        Deps = deps;
        SocketConfig = socketConfig;
        EncryptionKey = startParams.EncryptionKey;

        // Add reference to socket
        Deps.SocketRefCount->IncrementReferences();

        AsioEventStrand = MakeUniqueNoThrow<asio::io_context::strand>(*Deps.Context);
        if (!AsioEventStrand)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        Logger.SetPrefix(GetConnectionLogPrefix(
            startParams.PeerExternalAddress.address().to_string(),
            startParams.PeerExternalAddress.port()));

        result = initSubsystems();
        if (result.IsFail())
            goto OnError;

        // Initialize timers for P2P connection (ignores ConnectionTimeoutUsec setting)
#ifdef TONK_DISABLE_RANDOM_ROUND
        resetTimers(SocketConfig->ConnectionTimeoutUsec, false);
#else
        resetTimers(protocol::kP2PConnectionTimeout, false);
#endif

        HolePuncher = MakeUniqueNoThrow<NATHolePuncher>(startParams);
        if (!HolePuncher)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        HolePuncher->RendezvousServerConnectionId = rendezvousServerConnectionId;

        // Write 5 byte probe data array
        static_assert(protocol::kNATProbeBytes == 5, "Update this");
        siamese::WriteU16_LE(HolePuncher->ProbeData, (uint16_t)(startParams.GetOutgoingProbeKey() >> 16));
        HolePuncher->ProbeData[2] = static_cast<uint8_t>(protocol::Unconnected_P2PNATProbe);
        siamese::WriteU16_LE(HolePuncher->ProbeData + 3, (uint16_t)startParams.GetOutgoingProbeKey());

        HolePuncher->RoundTicker = MakeUniqueNoThrow<asio::steady_timer>(*Deps.Context);
        if (!HolePuncher->RoundTicker)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        /*
            This is the right time to assign a Connection Id, before any
            messages are queued for delivery to the peer.

            The Connection Id is used to reconnect if the source IP address or
            port changes during a connection.  Both client and server in a
            connection are assigned a Connection Id by the other side.
            And both peers are able to change their source address by sending
            a valid datagram with the ID flag set.
        */
        if (!Deps.IdMap->InsertAndAssignId(LocallyAssignedIdForRemoteHost, this))
        {
            // Id assignment can only fail if too many clients were allowed to
            // connect, which should not be possible.
            TONK_DEBUG_BREAK();
            Logger.Warning("InitializeP2PConnection: Failed to insert new connection into id map");
        }

        TonkAddress appAddress;
        SetTonkAddressStr(appAddress, startParams.PeerExternalAddress);

        // FIXME: If this asserts we currently seem to shut down uncleanly
        const uint32_t appDecision = SocketConfig->OnP2PConnectionStart(
            SocketConfig->AppContextPtr,
            GetTonkConnection(),
            &appAddress,
            &ConnectionConfig);
        if (TONK_DENY_CONNECTION == appDecision)
        {
            result = Result("Connection::InitializeP2PConnection", "Application OnP2PConnectionStart() callback denied connection", ErrorType::Tonk);
            goto OnError;
        }

        MakeCallbacksOptional(ConnectionConfig);

        Logger.Debug("App accepted P2P connection request. Waiting for NAT probe key = ", HexString(startParams.GetExpectedIncomingProbeKey()));

        // Start listening to probes with the correct key
        const bool insertSuccess = Deps.P2PKeyMap->InsertByKey(
            startParams.GetExpectedIncomingProbeKey(),
            this);

        if (!insertSuccess)
        {
            TONK_DEBUG_BREAK(); // Should never happen
            Logger.Warning("InitializeP2PConnection: Unable to insert into P2P key map");
        }

        startNextNATProbeRoundTimer();

        postNextTimer();
    }
    catch (...)
    {
        TONK_DEBUG_BREAK(); // Should never happen except out of memory error
        result = Result("Connection::InitializeP2PConnection", "Exception!", ErrorType::Tonk);
        goto OnError;
    }

OnError:
    if (result.IsFail())
    {
        Logger.Warning("Connection::InitializeP2PConnection failed: ", result.ToJson());
        Shutdown();
    }

    return result;
}

Result Connection::initSubsystems()
{
#ifdef TONK_DETAILED_STATS
    // Cycle the detail statistics
    Deets.NextWrite();
    Deets.GetWritePtr()->TimestampUsec = siamese::GetTimeUsec();
#endif // TONK_DETAILED_STATS

    memset(&CachedLocalAddress, 0, sizeof(CachedLocalAddress));
    memset(&LastIdAndAddressSent, 0, sizeof(LastIdAndAddressSent));

    const bool enableCompression = \
        (SocketConfig->Flags & TONK_FLAGS_DISABLE_COMPRESSION) == 0;

    const bool enablePadding = \
        (SocketConfig->Flags & TONK_FLAGS_ENABLE_PADDING) != 0;

    Result result = Outgoing.Initialize({
        enableCompression,
        enablePadding,
        &Logger,
        Deps.UDPSender,
        &SelfRefCount,
#ifdef TONK_DETAILED_STATS
        &Deets,
#endif // TONK_DETAILED_STATS
        &SenderControl
    },
        EncryptionKey);
    if (result.IsFail()) {
        return result;
    }

    // Initialize Incoming object given the app context
    result = Incoming.Initialize({
        &SelfRefCount,
        this,
        &TimeSync,
        &Outgoing,
        &ReceiverControl,
#ifdef TONK_DETAILED_STATS
        &Deets,
#endif // TONK_DETAILED_STATS
        &Logger
    },
        EncryptionKey,
        &ConnectionConfig,
        GetTonkConnection());
    if (result.IsFail()) {
        return result;
    }

    // Configure the bandwidth limit
    SenderBandwidthControl::Parameters sparams;
    sparams.MaximumBPS = (unsigned)SocketConfig->BandwidthLimitBPS;
    if (sparams.MaximumBPS > TONK_MAX_BPS) {
        sparams.MaximumBPS = TONK_MAX_BPS;
    }

    // Initialize bandwidth control
    SenderControl.Initialize({
        &TimeSync,
        &Logger
    },
        sparams);
    ReceiverControl.Initialize({
        &TimeSync,
        &Logger
    });

    // Create the timer
    Ticker = MakeUniqueNoThrow<asio::steady_timer>(*Deps.Context);
    if (!Ticker) {
        return Result::OutOfMemory();
    }

    return Result::Success();
}

void Connection::resetTimers(uint32_t noDataTimeoutUsec, bool connectionEstablished)
{
    const uint64_t nowUsec = GetTicksUsec();

    // Tick the no-data timeout once
    NoDataTimer.SetTimeoutUsec(noDataTimeoutUsec, Timer::Behavior::Stop);
    NoDataTimer.Start(nowUsec);

    // Tick the flow control timer repeatedly
    TimeSyncTimer.SetTimeoutUsec(
        connectionEstablished ? protocol::kTimeSyncIntervalUsec : protocol::kConnectionIntervalUsec,
        Timer::Behavior::Restart);
    TimeSyncTimer.Start(nowUsec);

    // Tick the acknowledgement timer repeatedly
    AcknowledgementTimer.SetTimeoutUsec(protocol::kMinAckIntervalUsec, Timer::Behavior::Restart);
    AcknowledgementTimer.Start(nowUsec);

    // Tick the app's OnTick() timer repeatedly
    AppTimer.SetTimeoutUsec(SocketConfig->TimerIntervalUsec, Timer::Behavior::Restart);
    AppTimer.Start(nowUsec);

    Logger.Debug("Reset no data timeout to ", noDataTimeoutUsec, " usec");
}

void Connection::onResolve(const asio::error_code& error, const asio::ip::tcp::resolver::results_type& results)
{
    if (SelfRefCount.IsShutdown()) {
        Logger.Warning("Ignoring onResolve() result during disco");
        return;
    }

    Result lookupResult;
    if (error) {
        lookupResult = Result("Connection::onResolve", error.message(), ErrorType::Asio, error.value());
    }
    else if (results.empty()) {
        lookupResult = Result("Connection::onResolve", "No results", ErrorType::Tonk, Tonk_HostnameLookup);
    }

    if (lookupResult.IsFail()) {
        SelfRefCount.StartShutdown(Tonk_HostnameLookup, lookupResult);
        return;
    }

    Logger.Debug("Resolve success: ", results.size(), " results");

    ResolverState->InitializeResolvedAddressList(results);

    // Post a connect to this address
    startConnectingToNextAddress();
}

void Connection::startConnectingToNextAddress()
{
    if (SelfRefCount.IsShutdown()) {
        return;
    }

    Result result;

    try
    {
        asio::ip::address ip_addr;
        result = ResolverState->GetNextServerAddress(ip_addr);
        if (result.IsFail()) {
            goto OnError;
        }

        Logger.Info("Attempting UDP-based connection with ", ip_addr.to_string(), " : ", ResolverState->ServerUDPPort);

        // Update peer address
        setPeerAddress(UDPAddress(ip_addr, ResolverState->ServerUDPPort));
    }
    catch (...)
    {
        result = Result("Connection::startConnectingToNextAddress", "Exception!");
        goto OnError;
    }

    return;

OnError:
    SelfRefCount.StartShutdown(Tonk_Error, result);
}

void Connection::OnICMPError(const asio::error_code& error)
{
    AsioEventStrand->post([this, error]()
    {
        const bool isOutgoingConnection = (ResolverState != nullptr);

        /*
            IF Connection Was Established
            OR This Was NOT The Connection Initiator
            THEN
                Disconnect asychronously with network failure error
            ELSE
                This is a client trying to connect given hostname,
                so try the next hostname.
            END IF
        */
        if (ConnectionEstablished || !isOutgoingConnection)
        {
            SelfRefCount.StartShutdown(Tonk_NetworkFailed,
                Result("Connection::OnICMPError",
                    error.message(),
                    ErrorType::Asio,
                    error.value()));
        }
        else
        {
            Logger.Debug("Selecting next server to connect to");

            // This is a client trying to connect given hostname,
            // so try the next hostname.
            startConnectingToNextAddress();
        }

        SelfRefCount.DecrementReferences();
    });
}

void Connection::OnUDPDatagram(
    uint64_t receiveUsec,
    const UDPAddress& addr,
    DatagramBuffer datagram)
{
    /*
        TBD: This might be more efficient if we add incoming datagrams to a
        linked list and only post() a task if the list is empty.  This needs
        benchmarking with a real application in order to evaluate the value
        of the extra complexity.  The intent would be to reduce the average
        latency of processing the second (and later) datagrams.
    */
    AsioEventStrand->post([this, receiveUsec, addr, datagram]()
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
        Result result = this->Incoming.ProcessDatagram(receiveUsec, addr, datagram.Data, datagram.Bytes);
        if (result.IsFail()) {
            onDecodeFailure(result);
        }

        datagram.Free();

        SelfRefCount.DecrementReferences();
    });
}

void Connection::onDecodeFailure(const Result& error)
{
    TONK_DEBUG_ASSERT(error.IsFail());

    const bool isUnauthenticatedData = \
        error.Error->Type == ErrorType::Tonk &&
        error.Error->Code == Tonk_BogonData;

    if (isUnauthenticatedData)
    {
        // Disabled this log
        //Logger.Debug("Bogon data ignored: ", error.ToJson());
        return;
    }

    Logger.Error("Authenticated datagram decode failure: ", error.ToJson());
    SelfRefCount.StartShutdown(Tonk_InvalidData, error);
}

void Connection::OnNATProbe(
    const UDPAddress& addr,
    uint32_t key,
    ConnectionAddrMap* addrMap,
    IUDPSender* sender)
{
    AsioEventStrand->post([this, addr, key, addrMap, sender]()
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());

        if (!HolePuncher) {
            Logger.Warning("Ignoring NAT probe from unrelated port");
        }
        else if (!HolePuncher->SeenProbe)
        {
            HolePuncher->SeenProbe = true;

            Logger.Warning("NAT probe from ", addr.address().to_string(), " : ", addr.port(), " - key = ", HexString(key));

            // Start attaching handshake to each outgoing datagram
            Outgoing.StartHandshakePeer2PeerConnect(HolePuncher->StartParams.GetOutgoingHandshakeKey());

            // Update source and destination addresses
            setPeerAddressAndSource(addrMap, sender, addr);
        }
        else {
            Logger.Warning("Ignoring extra probe from ", addr.address().to_string(), " : ", addr.port(), " - key = ", HexString(key));
        }

        SelfRefCount.DecrementReferences();
    });
}

void Connection::OnP2PConnection(
    uint64_t receiveUsec,
    const UDPAddress& addr,
    DatagramBuffer datagram,
    uint64_t handshakeKey,
    ConnectionAddrMap* addrMap,
    IUDPSender* sender)
{
    AsioEventStrand->post([this, receiveUsec, addr, datagram, handshakeKey, addrMap, sender]()
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());

        if (!HolePuncher)
        {
            TONK_DEBUG_BREAK(); // Should never happen
            Logger.Warning("Ignoring P2P connect: P2P connection is not in progress");
        }
        else if (handshakeKey != HolePuncher->StartParams.GetExpectedIncomingHandshakeKey())
        {
            Logger.Warning("Ignoring P2P connect: HandshakeKey=", HexString(handshakeKey),
                " IncomingKey=", HexString(HolePuncher->StartParams.GetExpectedIncomingHandshakeKey()),
                " does not match from ", addr.address().to_string(), " : ", addr.port());
        }
        else
        {
            // If we have seen a probe already, and this peer is the tie-breaker:
            if (HolePuncher->SeenProbe && HolePuncher->StartParams.WinTies)
            {
                // Ignore connection requests as tie-breaker if we have already seen a probe.
                // It is the job of the tie-breaker to make the connection request.
                ModuleLogger.Warning("Ignoring connection request - We are tie breaker and received a probe already");
            }
            else
            {
                /*
                    (1) Have seen probe (2) I am not tie-breaker

                    Switch to the new sender/source pair.

                    (1) Have *not* seen probe yet (2) I am/not tie-breaker

                    No connection requests have been sent yet.
                    Peer got a probe and we got a connection request, so go with it.
                */

                // In any case this counts as a probe received
                HolePuncher->SeenProbe = true;

                // If the peer address or source is changing:
                if (Outgoing.GetPeerUDPAddress() != addr ||
                    addrMap != Deps.AddressMap)
                {
                    ModuleLogger.Warning("Switching NAT port pairs for tie-breaker. New dest: ",
                        addr.address().to_string(), ":", addr.port());

                    // It is safe to update these from this thread because
                    // datagrams are only sent from this strand.

                    setPeerAddressAndSource(addrMap, sender, addr);
                }
                else
                {
                    ModuleLogger.Debug("Repeated connection request from the same source address - No change needed");
                }
            }

            // Process data attached to the handshake regardless of outcome above
            Result result = Incoming.ProcessDatagram(receiveUsec, addr, datagram.Data, datagram.Bytes);
            if (result.IsFail()) {
                onDecodeFailure(result);
            }
            datagram.Free();
        }

        SelfRefCount.DecrementReferences();
    });
}

void Connection::postNextTimer()
{
    Ticker->expires_after(std::chrono::microseconds(protocol::kSendTimerTickIntervalUsec));

    Ticker->async_wait(([this](const asio::error_code& error)
    {
        // Note: Dispatch invokes on the same thread if possible.
        // It often executes immediately.
        AsioEventStrand->dispatch([this, error]() {
            if (error) {
                onTimerError(error);
            }
            else if (TimerTickResult::TickAgain == onTimerTick()) {
                postNextTimer();
            }
        });
    }));
}

void Connection::onTimerError(const asio::error_code& error)
{
    /*
        Our Timer failed for some reason unexpectedly.  Maybe this happens during shutdown.
        This error is not recoverable so we should begin the disconnect process.
    */
    SelfRefCount.StartShutdown(Tonk_NetworkFailed,
        Result("Connection::onTimerError",
            error.message(),
            ErrorType::Asio,
            error.value()));

    // Report close to application since timer will no longer fire
    reportClose();

    removeFromMaps();
}

Connection::TimerTickResult Connection::onTimerTick()
{
    const uint64_t nowUsec = GetTicksUsec();

#ifdef TONK_DETAILED_STATS
    // Cycle the detail statistics
    Deets.NextWrite();
    Deets.GetWritePtr()->TimestampUsec = nowUsec;
#endif // TONK_DETAILED_STATS

    // If the TonkConnection's parent TonkSocket is shut down:
    if (Deps.SocketRefCount->IsShutdown())
    {
        SelfRefCount.StartShutdown(
            Tonk_AppRequest,
            Result("Connection::onTimerTick", "App shutdown in progress", ErrorType::Tonk, Tonk_AppRequest));
    }

    // If this TonkConnection is disconnected:
    if (SelfRefCount.IsShutdown()) {
        return onDiscoTimerTick();
    }

    // If no data has been received for too long:
    if (NoDataTimer.IsExpired(nowUsec))
    {
        SelfRefCount.StartShutdown(
            Tonk_RemoteTimeout,
            Result("Connection::onTimerTick", "No data from peer (Connection timeout)"));
        return TimerTickResult::TickAgain;
    }

    // If peer should be sending acks but none are received:
    if (!Outgoing.PeerCaughtUp() &&
        Outgoing.GetLastAckReceiveUsec() != 0 &&
        (int64_t)(nowUsec - Outgoing.GetLastAckReceiveUsec() - protocol::kNoAckSquelchTimeoutUsec) > 0)
    {
        // Squelch sender
        SenderControl.Squelch();

        if (RemoteAssignedIdForLocalHost != TONK_INVALID_ID)
        {
            // In case this is a mobile hand-off, start sending source address update tags
            Outgoing.StartHandshakeC2SUpdateSourceAddress(
                EncryptionKey,
                RemoteAssignedIdForLocalHost);
        }
    }

    // Update bandwidth control
    SenderControl.RecalculateAvailableBytes(nowUsec);
    ReceiverControl.UpdateSendShape(nowUsec, CachedReceiverStats.TripUsec);

    // Update NAT external mapped port
    if (Deps.MappedPort)
    {
        const uint16_t externalMappedPort = Deps.MappedPort->ExternalMappedPort;
        if (LocalNATMapExternalPort != externalMappedPort)
        {
            LocalNATMapExternalPort = externalMappedPort;
            sendNATMapPortToPeer(externalMappedPort);
        }
    }

    // If connection is not established yet:
    if (!ConnectionEstablished)
    {
        // If there is no peer address yet, wait longer
        if (!Outgoing.IsPeerAddressValid()) {
            return TimerTickResult::TickAgain;
        }

        // If it is time to run time sync but no session is established:
        if (TimeSyncTimer.IsExpired(nowUsec))
        {
            /*
                Each outgoing datagram will have a handshake footer attached, so
                this code path just needs to make sure there is at least one
                message to send, and to flush datagrams
            */

            // Send a 0-byte padding message frame
            Outgoing.QueueUnreliable(protocol::MessageType_Padding, nullptr, 0);
        }

        // Flush all queued data
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
        // Send at least one packet
        Result flushResult = Outgoing.Flush();
        if (flushResult.IsFail()) {
            SelfRefCount.StartShutdown(Tonk_Error, flushResult);
        }

        return TimerTickResult::TickAgain;
    }

    // Post retransmits up to the protocol limit before adding more data
    for (unsigned i = 0; i < protocol::kRetransmitLimit; ++i)
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
        bool messageSent = false;
        Result retransmitResult = Outgoing.PostRetransmit(messageSent);

        if (retransmitResult.IsFail())
        {
            SelfRefCount.StartShutdown(Tonk_Error, retransmitResult);
            return TimerTickResult::TickAgain;
        }

        if (!messageSent) {
            break;
        }
    }

    /*
        TBD: Should application OnTick() and other things be running from an
        Asio green thread instead of blocking the rest of the code here?
        If the application significantly stalls the timer tick, for example,
        then it may cause inefficient use of the network.  And if the OnData()
        callback is called from a background thread then it would allow us
        to process FEC data in parallel.  Benchmarking needed here.
    */

    // If it is time to send a time sync message:
    if (TimeSyncTimer.IsExpired(nowUsec)) {
        onTimeSyncSendTimer();
    }

    /*
        This will emit a new acknowledgement either on a regular timer,
        or when the receiver bandwidth controller wants to emit a new
        rate control message for the sender to use.

        TBD: Should we gate this also on whether or not an acknowledgement
        might be needed?  Currently not because I do not want to decrease
        the rate at which we send bandwidth shape updates.
    */
    if (AcknowledgementTimer.IsExpired(nowUsec) ||
        (ReceiverControl.GetShouldAckFast()
         /* && Incoming.IsAckRequired() */))
    {
        onAckSendTimer(nowUsec);
    }

    // If it is time to invoke the application OnTick():
    if (AppTimer.IsExpired(nowUsec))
    {
        // Invoke application OnTick
        ConnectionConfig.OnTick(
            ConnectionConfig.AppContextPtr,
            reinterpret_cast<TonkConnection>(this),
            nowUsec);
    }

    // Flush all queued data
    TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
    Result flushResult = Outgoing.Flush();
    if (flushResult.IsFail()) {
        SelfRefCount.StartShutdown(Tonk_Error, flushResult);
    }

    //Logger.Info("this=", this, " AvailBytes = ", SenderControl.GetAvailableBytes(), " ProbeBytes = ", SenderControl.GetAvailableProbeBytes());

    // Calculate amount of recovery data we must send to protect data we've sent
    const unsigned recoveryCount = SenderControl.CalculateRecoverySendCount(nowUsec);
    int recoveryBytesSent = 0;
    for (unsigned i = 0; i < recoveryCount; ++i)
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
        tonk::Result result = Outgoing.PostRecovery(recoveryBytesSent);

        // If recovery packet was not sent:
        if (recoveryBytesSent == 0)
        {
            if (result.IsFail()) {
                SelfRefCount.StartShutdown(Tonk_FECFailed, result);
            }
            break; // Stop here
        }
    }

    // If FEC is enabled for probing bandwidth:
    if ((SocketConfig->Flags & TONK_FLAGS_ENABLE_FEC) != 0)
    {
        while (SenderControl.ShouldSendProbe(recoveryBytesSent))
        {
            // Note that recovery packets do not subtract from the bandwidth for
            // app data, but app data subtracts from bandwidth used for probing.

            TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
            tonk::Result result = Outgoing.PostRecovery(recoveryBytesSent);

            // If recovery packet was not sent:
            if (recoveryBytesSent == 0)
            {
                if (result.IsFail()) {
                    SelfRefCount.StartShutdown(Tonk_FECFailed, result);
                }
                break; // Stop here
            }

            SenderControl.SpendProbeBytes(recoveryBytesSent);
        }
    }

    // While there is available bandwidth for more probing:
    while (SenderControl.ShouldSendProbe(protocol::kDummyBytes))
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
        if (!Outgoing.PostDummyDatagram()) {
            break;
        }

        SenderControl.SpendProbeBytes(protocol::kDummyBytes);
    }

    return TimerTickResult::TickAgain;
}

void Connection::onAckSendTimer(uint64_t nowUsec)
{
    Result result = Incoming.PostAcknowledgements();
    if (result.IsFail()) {
        SelfRefCount.StartShutdown(Tonk_FECFailed, result);
    }

    // Find latest flow control interval
    unsigned intervalUsec = protocol::kMinAckIntervalUsec;
    if (TimeSync.IsSynchronized())
    {
        // Otherwise send up to two acknowledgements per OWD, in case one gets lost.
        // If the data rate is very low, Ack-Acks will stop us from sending a ton of Acks.
        const uint64_t owdUsec = TimeSync.GetMinimumOneWayDelayUsec();
        intervalUsec = static_cast<unsigned>(owdUsec) / 2;
    }
    if (intervalUsec < protocol::kMinAckIntervalUsec) {
        intervalUsec = protocol::kMinAckIntervalUsec;
    }

    AcknowledgementTimer.SetTimeoutUsec(intervalUsec);
    AcknowledgementTimer.Start(nowUsec);
}

/**
    TimeSync message format:

    <Next Expected Nonce (3 bytes)>
    <MinDeltaTS24 (3 bytes)>
    <Receiver Statistics (5 bytes)>
*/
static const unsigned kTimeSyncMessageBytes = 3 + 3 + ReceiverStatistics::kCompressBytes;

void Connection::onTimeSyncSendTimer()
{
    uint8_t buffer[kTimeSyncMessageBytes];

    // Write next expected nonce
    siamese::WriteU24_LE_Min4Bytes(buffer, Incoming.GetNextExpectedNonce24());

    // Write MinDeltaTS24
    siamese::WriteU24_LE_Min4Bytes(buffer + 3, TimeSync.GetMinDeltaTS24().ToUnsigned());

    // Write ReceiverStatistics
    const ReceiverStatistics stats = ReceiverControl.GetStatistics();
    stats.Compress(buffer + 3 + 3);

    Result result = Outgoing.QueueUnreliable(
        protocol::MessageType_TimeSync,
        buffer,
        kTimeSyncMessageBytes);

    if (result.IsFail()) {
        SelfRefCount.StartShutdown((unsigned)result.GetErrorCode(), result);
    }
}

Result Connection::OnTimeSync(const uint8_t* data, unsigned bytes)
{
    // If the message is truncated:
    if (bytes != kTimeSyncMessageBytes) {
        return Result("Connection::OnTimeSync", "Truncated time sync message", ErrorType::Tonk, Tonk_InvalidData);
    }

    // Read the 3-byte next expected nonce field
    const Counter24 nextExpectedNonce24 = siamese::ReadU24_LE_Min4Bytes(data);
    Outgoing.UpdateNextExpectedNonceFromTimeSync(nextExpectedNonce24);

    // Read the 3-byte MinDeltaTS24 field
    const Counter24 minDeltaTS24 = siamese::ReadU24_LE_Min4Bytes(data + 3);
    TimeSync.OnPeerMinDeltaTS24(minDeltaTS24);

    // Read ReceiverStatistics
    ReceiverStatistics stats;
    stats.Decompress(data + 3 + 3);

#ifdef TONK_ENABLE_VERBOSE_BANDWIDTH
    Logger.Debug("Connection::OnTimeSync(ReceivedBPS=", stats.GoodputBPS, ", LossRate=", stats.LossRate, ")");
#endif

    // Update the cached receiver state for statistics for the app
    {
        Locker locker(PublicAPILock);
        CachedReceiverStats = stats;
    }

    return Result::Success();
}

HandshakeResult Connection::OnUnauthenticatedHandshake(const uint8_t* data, const unsigned bytes)
{
    /*
        Note that all of this processing is not from authenticated data so
        it could have come from anywhere.  The impact of processing this
        data should be minimal and restricted mainly to cases where the
        peer is not reachable or connection handshake is ongoing.
    */

    static const unsigned kHandshakeRightOffset = protocol::kUntaggedDatagramBytes + protocol::kHandshakeBytes;
    if (bytes < kHandshakeRightOffset)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return HandshakeResult::DropDatagram;
    }
    const uint8_t* handshakeData = data + bytes - kHandshakeRightOffset;
    const protocol::HandshakeType type = protocol::handshake::GetType(handshakeData);

    if (type == protocol::HandshakeType_S2CUnknownSourceAddress)
    {
        Logger.Debug("Server did not recognize our source address");

        /*
            Server did not recognize our source address.  This happens if we
            changed IP source address or source port without realizing it.
            This can happen in practice if a mobile device is rehoming under
            a new NAT address.  We will need to start attaching our Connection
            Id to outgoing datagrams.
        */
        if (RemoteAssignedIdForLocalHost != TONK_INVALID_ID)
        {
            Outgoing.StartHandshakeC2SUpdateSourceAddress(
                EncryptionKey,
                RemoteAssignedIdForLocalHost);
        }
        return HandshakeResult::DropDatagram;
    }

    if (type == protocol::HandshakeType_Disconnect)
    {
        const uint64_t key = protocol::handshake::GetKey(handshakeData);
        if (key != EncryptionKey)
        {
            Logger.Warning("Ignoring disconnect with wrong key = ", HexString(key));
            return HandshakeResult::DropDatagram;
        }

        if (ConnectionEstablished)
        {
            Logger.Warning("Ignoring handshake disconnect after connection established");
            return HandshakeResult::DropDatagram;
        }

        SelfRefCount.StartShutdown(
            Tonk_ConnectionRejected,
            Result("Disconnected",
                "Connection rejected by peer (server full?)",
                ErrorType::Tonk,
                Tonk_ConnectionRejected));
        return HandshakeResult::DropDatagram;
    }

    if (type == protocol::HandshakeType_S2CCookieResponse)
    {
        if (ConnectionEstablished)
        {
            Logger.Warning("Ignoring S2C cookie response after connection established");
            return HandshakeResult::DropDatagram;
        }

        const uint64_t key = protocol::handshake::GetKey(handshakeData);

        // Update our encryption key to match
        EncryptionKey = key;
        Outgoing.ChangeEncryptionKey(key);
        Incoming.ChangeEncryptionKey(key);

        // Start attaching new handshake to each outgoing datagram
        Outgoing.StartHandshakeC2SConnect(key);

        // Cookie responses contain no other data
        return HandshakeResult::DropDatagram;
    }

    // Ignore other types
    return HandshakeResult::ContinueProcessing;
}

void Connection::OnAuthenticatedDatagram(const UDPAddress& addr, uint64_t receiveUsec)
{
    // If peer address has changed:
    if (PeerAddressChangesAllowed && addr != Outgoing.GetPeerUDPAddress())
    {
        setPeerAddress(addr);
        PeerAddressChangesAllowed = false;
    }

    // Reset the data timer
    NoDataTimer.Start(receiveUsec);

    // Stop attaching connection request if connection is established
    Outgoing.StopHandshake();

    // Handle connection:
    if (ConnectionEstablished) {
        return;
    }

    // If P2P connection data is coming from the wrong source address:
    if (HolePuncher &&
        HolePuncher->SeenProbe &&
        HolePuncher->StartParams.WinTies &&
        addr != Outgoing.GetPeerUDPAddress())
    {
        Logger.Warning("Refusing to mark connection established until we receive data from the expected peer address. Data was from ", addr.address().to_string(), " : ", addr.port());
        return;
    }

    ConnectionEstablished = true;

    Logger.Debug("Connection established with ", addr.address().to_string(), " : ", addr.port());

    // Reset timers when connection is established
    resetTimers(SocketConfig->NoDataTimeoutUsec, true);

    // Force time sync timer to expire right away to send a time sync update on the next tick
    TimeSyncTimer.ForceExpire();

    // Invoke application OnConnect() callback
    ConnectionConfig.OnConnect(ConnectionConfig.AppContextPtr, GetTonkConnection());
}

void Connection::setPeerAddress(const UDPAddress& peerUDPAddress)
{
    const bool insertSuccess = Deps.AddressMap->ChangeAddress(
        Outgoing.GetPeerUDPAddress(),
        peerUDPAddress,
        this);

    // If unable to insert the address because it is already listed:
    if (!insertSuccess)
    {
        Logger.Warning("Unable to change address (already taken in map): ",
            peerUDPAddress.address().to_string(), ":", peerUDPAddress.port());
    }

    Outgoing.SetPeerUDPAddress(peerUDPAddress);

    queueAddressAndIdChangeMessage(peerUDPAddress);

    Logger.SetPrefix(GetConnectionLogPrefix(
        peerUDPAddress.address().to_string(),
        peerUDPAddress.port()));
}

void Connection::setPeerAddressAndSource(
    ConnectionAddrMap* addrMap,
    IUDPSender* sender,
    const UDPAddress& peerUDPAddress)
{
    TONK_DEBUG_ASSERT(addrMap && sender && !peerUDPAddress.address().is_unspecified());

    const UDPAddress oldAddress = Outgoing.GetPeerUDPAddress();

    const bool insertSuccess = addrMap->InsertAddress(peerUDPAddress, this);

    if (!insertSuccess)
    {
        Logger.Error("Unexpectedly failed to insert new peer address ", peerUDPAddress.address().to_string(), " : ", peerUDPAddress.port());
    }
    else if (!oldAddress.address().is_unspecified() && Deps.AddressMap)
    {
        const bool removeSuccess = Deps.AddressMap->RemoveAddress(oldAddress, this);
        if (!removeSuccess)
            Logger.Error("Unexpectedly failed to remove old peer address ", oldAddress.address().to_string(), " : ", oldAddress.port());
    }

    // Update sender to use
    Outgoing.SetUDPSender(sender);

    // Update address map
    Deps.AddressMap = addrMap;

    // Update outgoing peer address
    Outgoing.SetPeerUDPAddress(peerUDPAddress);

    // Add new address/id change message to queue
    queueAddressAndIdChangeMessage(peerUDPAddress);

    // Update logger prefix
    Logger.SetPrefix(GetConnectionLogPrefix(
        peerUDPAddress.address().to_string(),
        peerUDPAddress.port()));
}

void Connection::queueAddressAndIdChangeMessage(const UDPAddress& peerUDPAddress)
{
    try
    {
        uint8_t data[protocol::kMaxClientIdAndAddressBytes];

        data[0] = protocol::ControlChannel_S2C_ClientIdAndAddress;
        siamese::WriteU24_LE(data + 1, LocallyAssignedIdForRemoteHost);
        siamese::WriteU16_LE(data + 1 + 3, peerUDPAddress.port());
        size_t written = 1 + 3 + 2;

        const asio::ip::address ipaddr = peerUDPAddress.address();
        if (ipaddr.is_v4())
        {
            const asio::ip::address_v4 v4addr = ipaddr.to_v4();
            const auto addrData = v4addr.to_bytes();
            for (size_t i = 0; i < addrData.size(); ++i) {
                data[written + i] = addrData[i];
            }
            written += addrData.size(); // Length implies IPv4
        }
        else if (ipaddr.is_v6())
        {
            const asio::ip::address_v6 v6addr = ipaddr.to_v6();
            const auto addrData = v6addr.to_bytes();
            for (size_t i = 0; i < addrData.size(); ++i) {
                data[written + i] = addrData[i];
            }
            written += addrData.size(); // Length implies IPv6
        }
        else
        {
            TONK_DEBUG_BREAK();
            return;
        }

        TONK_DEBUG_ASSERT(written <= protocol::kMaxClientIdAndAddressBytes);

        // Note: Sometimes the same IP:port can be contacted twice in a row
        // from e.g. hostname resolution.  Avoid queuing the same data twice.
        if (0 != memcmp(LastIdAndAddressSent, data, written))
        {
            Outgoing.QueueControl(data, written);
            memcpy(LastIdAndAddressSent, data, written);
        }
    }
    catch (...)
    {
        Logger.Error("queueAddressAndIdChangeMessage: Exception while converting IP address");
    }
}

Result Connection::OnControlChannel(const uint8_t* data, unsigned bytes)
{
    const uint8_t controlType = data[0];

    if (controlType == protocol::ControlChannel_S2C_ClientIdAndAddress &&
        bytes >= 1 + 3 + 2)
    {
        Logger.Debug("Remote peer set our client id and address via control channel");

        TONK_DEBUG_ASSERT(!ClientIdAndAddressIsSet);

        /*
            Note that it is currently safe for this event to be received
            multiple times.  If in the future we add more data then it may
            need a flag to check this being set twice.
        */
        const uint32_t id = siamese::ReadU24_LE_Min4Bytes(data + 1);
        const uint16_t port = siamese::ReadU16_LE(data + 1 + 3);

        data += 1 + 3 + 2;
        bytes -= 1 + 3 + 2;

        if (bytes == 4) // IPv4:
        {
            auto addrPtr = reinterpret_cast<const std::array<uint8_t, 4>*>(data);
            setSelfAddress(id, UDPAddress(asio::ip::address_v4(*addrPtr), port));
            return Result::Success();
        }

        if (bytes == 16) // IPv6:
        {
            auto addrPtr = reinterpret_cast<const std::array<uint8_t, 16>*>(data);
            setSelfAddress(id, UDPAddress(asio::ip::address_v6(*addrPtr), port));
            return Result::Success();
        }
    }
    else if (controlType == protocol::ControlChannel_C2S_NATExternalPort &&
        bytes == protocol::kControlNATExternalBytes)
    {
        const uint16_t port = siamese::ReadU16_LE(data + 1);

        // Peer has announced its NAT-mapped external port
        RemoteNATMapExternalPort = port;

        Logger.Debug("Peer informed its NAT router mapped port ", port);
        return Result::Success();
    }
    else if (controlType == protocol::ControlChannel_S2C_Peer2PeerConnect &&
        bytes >= 1 + protocol::P2PConnectParams::kMinBytes)
    {
        protocol::P2PConnectParams params;
        siamese::ReadByteStream controlStream(data + 1, bytes - 1);
        if (params.Read(controlStream))
        {
            onP2PStartConnect(params);
            return Result::Success();
        }
    }
    else if (controlType == protocol::ControlChannel_Disconnect &&
        bytes == protocol::kControlDisconnectBytes &&
        data[1] == protocol::ControlChannel_Disconnect &&
        data[2] == protocol::ControlChannel_Disconnect &&
        data[3] == protocol::ControlChannel_Disconnect)
    {
        SelfRefCount.StartShutdown(
            Tonk_AppRequest,
            Result("Disconnected",
                "Connection closed by peer (gracefully)",
                ErrorType::Tonk,
                Tonk_AppRequest));
        return Result::Success();
    }

    // Fall through into invalid data handler:

    TONK_DEBUG_BREAK(); // Invalid data

    std::stringstream ss;
    ss << "Server sent invalid control channel message " << (int)data[0] << " with length " << bytes;
    return Result("Connection::onControlStreamMessage", ss.str(), ErrorType::Tonk, Tonk_InvalidData);
}

void Connection::setSelfAddress(uint32_t id, const UDPAddress& addr)
{
    Logger.Debug("Informed by remote peer that our network address is ", addr.address().to_string(), " : ", addr.port(), " and id = ", id);

    Locker locker(PublicAPILock);

    RemoteAssignedIdForLocalHost = id;
    SelfAddress = addr;

    // Cache the string form of the UDP address
    SetTonkAddressStr(CachedLocalAddress, addr);
}

Connection::TimerTickResult Connection::onDiscoTimerTick()
{
    // Cancel any ongoing hostname resolution to quit faster
    if (ResolverState && ResolverState->Resolver) {
        ResolverState->Resolver->cancel();
    }

    // It is safe to report OnClose() to the application here because all of
    // the strand work will check if disconnection has started before invoking
    // any further callbacks.
    reportClose();

    /*
        The Connection object will stay alive until the application calls
        tonk_free().  After that point this code will run periodically to
        check if it is time to free the Connection object.  Note that since
        this code is running in the Connection's strand, it is not possible
        for datagram processing (or other operations) to be going on in
        parallel.  And the application has called tonk_free() to promise
        that no more API calls will interact with the object.

        Summary- In the Connection strand:
        (1) Datagram may be pending to process
        (2) Flush may be queued
        (3) async_resolve() may be in progress

        Summary- In the Send() completion callback:
        (4) Connection::WriteAllocator.Free() from Send() completion


        Drilling into each one:

        (1) Datagram may be pending to process

        This will continue to happen until the Connection is removed from all
        of the ConnectionMaps.  After that point incoming datagrams will stop
        being assigned to this Connection.  Some may still be pending for
        processing directly after this timer tick.

        (2) Flush may be queued.

        Flush gets queued when the app calls tonk_flush(), which may happen
        immediately after this timer tick.

        (3) async_resolve() may be in progress

        Initiated connections will resolve the server hostname before sending
        the first datagram.  If the application calls tonk_close() followed by
        tonk_free() during this operation, then we should wait for it to
        complete before shutting down.

        (4) Connection::WriteAllocator.Free() from Send() completion

        Since the application is no longer sending datagrams and this timer tick
        does not cause any new datagrams to be sent, there may be some pending
        Send() callbacks that will not complete until after this timer tick.


        Design:

        A reference count in the Connection class is incremented before queuing
        all operations.  When the operation completes, the counter is decremented
        before the completion callback returns.  It is important to pay close
        attention to the lifetime of scoped objects like Locker to make sure that
        the counter is decremented after these objects go out of scope.

        This timer tick will be able to delete the object after the reference
        counter goes to zero.

        In practice this means that any asio async operation e.g.
            ->post(
        Or
            ->async_resolve(
        Or
            ->async_send_to(
        Must have all code paths covered with reference counting.
    */

    if (SelfRefCount.ObjectInUse()) {
        //Logger.Debug("Delaying Connection shutdown (object in use)");
        return TimerTickResult::TickAgain;
    }

    Shutdown();

    // Object goes out of scope here----------------------------------------
    delete this;

    return TimerTickResult::StopTicking;
}

void Connection::sendNATMapPortToPeer(uint16_t port)
{
    uint8_t data[protocol::kControlNATExternalBytes];
    data[0] = protocol::ControlChannel_C2S_NATExternalPort;
    siamese::WriteU16_LE(data + 1, port);

    Result result = Outgoing.QueueControl(
        data,
        protocol::kControlNATExternalBytes);
    if (result.IsFail()) {
        Logger.Warning("Unable to send NAT map message: ", result.ToJson());
    }
}

static const uint8_t kControlDiscoData[protocol::kControlDisconnectBytes] = {
    protocol::ControlChannel_Disconnect,
    protocol::ControlChannel_Disconnect,
    protocol::ControlChannel_Disconnect,
    protocol::ControlChannel_Disconnect
};

void Connection::sendDisconnectMessageToPeer()
{
    // If a connection was not established:
    if (!ConnectionEstablished) {
        return;
    }

    // Post disco message
    Result result = Outgoing.QueueControl(
        kControlDiscoData,
        protocol::kControlDisconnectBytes);
    if (result.IsFail()) {
        Logger.Warning("Unable to queue disconnect message: ", result.ToJson());
    }

    result = Outgoing.Flush();
    if (result.IsFail()) {
        Logger.Error("Flush for disco failed: ", result.ToJson());
    }

    Logger.Debug("Flushed a disconnection message");
}

void Connection::reportClose()
{
    if (SelfRefCount.NotShutdown() || HasReportedClose) {
        return;
    }
    HasReportedClose = true;

    // Invoke application OnClose() callback
    ConnectionConfig.OnClose(
        ConnectionConfig.AppContextPtr,
        GetTonkConnection(),
        static_cast<TonkResult>(SelfRefCount.ShutdownReason),
        SelfRefCount.ShutdownJson.c_str());

    sendDisconnectMessageToPeer();

    removeFromMaps();
}

void Connection::removeFromMaps()
{
    if (RemovedFromMaps) {
        return;
    }
    RemovedFromMaps = true;

    // Make sure that the maps do not contain the Connection
    bool success = Deps.IdMap->RemoveAndUnassignId(LocallyAssignedIdForRemoteHost, this);
    if (!success) {
        Logger.Warning("Unable to remove/unassign id=", LocallyAssignedIdForRemoteHost);
    }

    const UDPAddress addr = Outgoing.GetPeerUDPAddress();
    success = Deps.AddressMap->RemoveAddress(addr, this);
    if (!success) {
        Logger.Warning("Unable to remove/unassign address ", addr.address().to_string(), ":", addr.port());
    }

    if (HolePuncher) {
        const uint32_t probeKey = HolePuncher->StartParams.GetExpectedIncomingProbeKey();
        success = Deps.P2PKeyMap->RemoveByKey(probeKey, this);
        if (!success) {
            Logger.Warning("Unable to remove/unassign P2PKey=", probeKey);
        }
    }
}

void Connection::onP2PStartConnect(const protocol::P2PConnectParams& params)
{
    protocol::P2PConnectParams startParams = params;
    startParams.ShotTimeUsec = TimeSync.FromLocalTime16(GetTicksUsec(), params.ShotTS16);

    Deps.NATPool->OnP2PStartConnect(
        getSelfAddress().port(),
        LocallyAssignedIdForRemoteHost,
        startParams);
}

void Connection::onNATProbeRound(const asio::error_code& error)
{
    // Note this is not called on the AsioEventStrand

    if (error)
    {
        SelfRefCount.StartShutdown(Tonk_NetworkFailed,
            Result("Connection::onNATProbeRound",
                error.message(), ErrorType::Asio, error.value()));
        return;
    }

    if (SelfRefCount.IsShutdown())
    {
        Logger.Debug("Connection is shutdown before round ", HolePuncher->ProtocolRound, " - Aborting remaining probes");
        return;
    }

    if (HolePuncher->SeenProbe)
    {
        Logger.Debug("Saw NAT probe before round ", HolePuncher->ProtocolRound, " - Aborting remaining probes");
        return;
    }

    // Check if rendezvous server is still around
    IConnection* icon = Deps.IdMap->FindById(HolePuncher->RendezvousServerConnectionId);
    if (icon)
        icon->SelfRefCount.DecrementReferences();
    else
    {
        SelfRefCount.StartShutdown(Tonk_ConnectionRejected,
            Result("Connection::onNATProbeRound", "Rendezvous server connection closed - Aborting P2P connection attempt", ErrorType::Tonk, Tonk_ConnectionRejected));
        return;
    }

#ifdef TONK_DISABLE_RANDOM_ROUND
    if (HolePuncher->ProtocolRound >= protocol::NATProbeRound4_Random)
        return;
#endif

#ifdef TONK_DEBUG
    const uint64_t t0 = siamese::GetTimeUsec();
    Logger.Debug("NAT probe round ", HolePuncher->ProtocolRound, " starting @ t = ", t0);
    TONK_DEBUG_ASSERT(HolePuncher->ProtocolRound >= protocol::NATProbeRound1_Exact);
#endif // TONK_DEBUG

    // On the first and second rounds:
    if (HolePuncher->ProtocolRound <= protocol::NATProbeRound2_Fuzzy)
    {
        // Send an extra probe from the main socket to the same port that
        // the rendezvous server sees for the peer
        Deps.NATPool->SendNATProbe(
            kNATPortMainSocketIndex,
            HolePuncher->ProbeData,
            HolePuncher->StartParams.PeerExternalAddress,
            &SelfRefCount);

        const uint16_t mappedPort = HolePuncher->StartParams.PeerNATMappedPort;
        const uint16_t mainPort = HolePuncher->StartParams.PeerExternalAddress.port();

        // If the peer mapped an external port on its NAT router:
        if (mappedPort != 0 && mappedPort != mainPort)
        {
            const UDPAddress probeTargetAddress(
                HolePuncher->StartParams.PeerExternalAddress.address(),
                mappedPort);

            // Send an extra probe from the main socket
            Deps.NATPool->SendNATProbe(
                kNATPortMainSocketIndex,
                HolePuncher->ProbeData,
                probeTargetAddress,
                &SelfRefCount);
        }
    }

    // Set up round parameters
    unsigned socketIndexOffset = 0;
    unsigned probesPerPort = 1;
    unsigned portCount = protocol::kNATRound1Exact_PortCount;
    if (HolePuncher->ProtocolRound >= protocol::NATProbeRound2_Fuzzy &&
        HolePuncher->ProtocolRound <= protocol::NATProbeRound3_Fuzzy2)
    {
        portCount = protocol::kNATRound23Fuzzy_PortCount;
    }
#ifndef TONK_DISABLE_RANDOM_ROUND
    if (HolePuncher->ProtocolRound >= protocol::NATProbeRound4_Random)
    {
        static_assert(protocol::kNATMaxPorts % protocol::kNATRound4Random_PortCount == 0, "Must evenly divide");
        socketIndexOffset = ((HolePuncher->ProtocolRound - protocol::NATProbeRound4_Random) * protocol::kNATRound4Random_PortCount + protocol::kNATRound23Fuzzy_PortCount) % protocol::kNATMaxPorts;
        probesPerPort = protocol::kProbesPerPortForRandom;
        portCount = 1;
    }
#endif

    const uint16_t remoteExternalPort = HolePuncher->StartParams.PeerExternalAddress.port();

    // Seed a PRNG to generate good port numbers to try on peer
    siamese::PCGRandom portPrng;
    const uint64_t seed = HolePuncher->StartParams.EncryptionKey + HolePuncher->ProtocolRound;
    portPrng.Seed(seed, HolePuncher->StartParams.GetPcgDomain());

    for (unsigned portIndex = 0; portIndex < portCount; ++portIndex)
    {
        if (HolePuncher->SeenProbe)
        {
            Logger.Debug("Saw NAT probe during round ", HolePuncher->ProtocolRound, " while sending probe ", portIndex, " - Aborting remaining probes");
            return;
        }

        for (unsigned probeIndex = 0; probeIndex < probesPerPort; ++probeIndex)
        {
            // Pick a port to target
            uint16_t destPort = 0;
            if (HolePuncher->ProtocolRound <= protocol::NATProbeRound1_Exact)
            {
                destPort = protocol::CalculateExactNATPort(remoteExternalPort, portIndex);
                if (destPort == 0) {
                    destPort = protocol::CalculateFuzzyNATPort(remoteExternalPort, portPrng);
                }
            }
            else if (HolePuncher->ProtocolRound <= protocol::NATProbeRound3_Fuzzy2) {
                destPort = protocol::CalculateFuzzyNATPort(remoteExternalPort, portPrng);
            }
            else { // Round 3+
                destPort = protocol::CalculateRandomNATPort(portPrng);
            }
            TONK_DEBUG_ASSERT(destPort != 0);

            const UDPAddress probeTargetAddress(
                HolePuncher->StartParams.PeerExternalAddress.address(),
                destPort);

            TONK_DEBUG_ASSERT(socketIndexOffset + portIndex < protocol::kNATMaxPorts);

            Deps.NATPool->SendNATProbe(
                socketIndexOffset + portIndex,
                HolePuncher->ProbeData,
                probeTargetAddress,
                &SelfRefCount);
        }
    }

#ifdef TONK_DEBUG
    const uint64_t t1 = siamese::GetTimeUsec();
    Logger.Debug("NAT probe blast took ", (t1 - t0) / 1000., " msec");
#endif // TONK_DEBUG

    startNextNATProbeRoundTimer();
}

void Connection::startNextNATProbeRoundTimer()
{
    // Next round
    HolePuncher->ProtocolRound++;

    const uint64_t nowUsec = GetTicksUsec();

    uint64_t delayUntilShotUsec = HolePuncher->StartParams.ShotTimeUsec - nowUsec;

    if (HolePuncher->ProtocolRound >= protocol::NATProbeRound4_Random)
    {
        // Add on prior round intervals
        delayUntilShotUsec += HolePuncher->StartParams.ProtocolRoundIntervalMsec * 1000 * (protocol::NATProbeRound4_Random - 1);

        // Add multiple of kP2PRound4Interval
        delayUntilShotUsec += (HolePuncher->ProtocolRound - protocol::NATProbeRound4_Random) * protocol::kP2PRound4Interval;
    }
    else
    {
        // Add on round intervals
        delayUntilShotUsec += HolePuncher->StartParams.ProtocolRoundIntervalMsec * 1000 * (HolePuncher->ProtocolRound - 1);
    }

    Logger.Info("Round ", HolePuncher->ProtocolRound, " P2PConnect shot time in ", (int)delayUntilShotUsec, " usec");

    // If delay is negative or too high:
    if (delayUntilShotUsec > protocol::kMaxP2PRoundFutureDelayUsec) {
        delayUntilShotUsec = 2000; // 2 msec
    }

    HolePuncher->RoundTicker->expires_after(std::chrono::microseconds(delayUntilShotUsec));

    SelfRefCount.IncrementReferences();
    HolePuncher->RoundTicker->async_wait(
        [this](const asio::error_code& error)
    {
        onNATProbeRound(error);
        SelfRefCount.DecrementReferences();
    });
}

void Connection::Shutdown()
{
    TONK_DEBUG_ASSERT(!SelfRefCount.ObjectInUse());

    // Make sure that at least one Disconnect has occurred
    SelfRefCount.StartShutdown(
        Tonk_AppRequest,
        Result("Connection::Shutdown", "Tonk shutdown", ErrorType::Tonk, Tonk_AppRequest));

    reportClose();

    removeFromMaps();

    // If the flood detection subsystem needs to be notified of a disconnect:
    if (Deps.DisconnectHandler) {
        Deps.DisconnectHandler->OnDisconnect(Deps.DisconnectAddrKey);
    }

    if (ResolverState && ResolverState->Resolver)
    {
        ResolverState->Resolver->cancel();
        ResolverState->Resolver = nullptr;
        ResolverState = nullptr;
    }

    // Kill the strand
    AsioEventStrand = nullptr;
    Ticker = nullptr;
    HolePuncher = nullptr;

    // Shut down most connected objects first:

    Incoming.Shutdown();
    Outgoing.Shutdown();

#ifdef TONK_DETAILED_STATS

    Deets.WriteJsonToFile();
    Logger.Info("Detailed statistics consumed ", Deets.GetMemoryUsed(), " bytes");

#endif

    // Release reference to socket
    Deps.SocketRefCount->DecrementReferences();
}


//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void Connection::tonk_status(TonkStatus& statusOut)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    statusOut.Flags = 0;

    if (TimeSync.IsSynchronized()) {
        statusOut.Flags |= TonkFlag_TimeSync;
    }

    if (LocalNATMapExternalPort != 0) {
        statusOut.Flags |= TonkFlag_NATMap_Local;
    }
    if (RemoteNATMapExternalPort != 0) {
        statusOut.Flags |= TonkFlag_NATMap_Remote;
    }

    if (ResolverState) { // Set during Initialize() - safe here
        statusOut.Flags |= TonkFlag_Initiated;
    }

    if (SelfRefCount.IsShutdown()) {
        statusOut.Flags |= TonkFlag_Disconnected;
    }
    else if (ConnectionEstablished) {
        statusOut.Flags |= TonkFlag_Connected;
    }
    else {
        statusOut.Flags |= TonkFlag_Connecting;
    }

    Outgoing.tonk_status(statusOut);

    // This Id is assigned on Connection initialization and is safe to read
    statusOut.LocallyAssignedIdForRemoteHost = LocallyAssignedIdForRemoteHost;

    statusOut.TimerIntervalUsec = SocketConfig->TimerIntervalUsec;
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void Connection::tonk_status_ex(TonkStatusEx& statusEx)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    statusEx.ConnectionKey = EncryptionKey; // Set during Initialize() - safe

    //--------------------------------------------------------------------------
    // API Lock
    {
        Locker locker(PublicAPILock);

        // Fill in local address
        statusEx.Local = CachedLocalAddress;
        statusEx.RemoteAssignedIdForLocalHost = RemoteAssignedIdForLocalHost;

        // Fill in peer receiver stats from the last TimeSync message
        statusEx.PeerSeenLossRate = CachedReceiverStats.LossRate;
        statusEx.PeerSeenBPS = CachedReceiverStats.GoodputBPS;
        statusEx.PeerTripUsec = CachedReceiverStats.TripUsec;

        // Fill in NAT map state
        statusEx.RemoteNATMapExternalPort = RemoteNATMapExternalPort;
        statusEx.LocalNATMapExternalPort = LocalNATMapExternalPort;
    }
    //--------------------------------------------------------------------------

    Outgoing.tonk_status_ex(statusEx);

    // Fill in local receiver stats
    const ReceiverStatistics receiverStats = ReceiverControl.GetStatistics();
    statusEx.IncomingBPS = receiverStats.GoodputBPS;
    statusEx.IncomingLossRate = receiverStats.LossRate;
    statusEx.TripUsec = receiverStats.TripUsec;
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

Result Connection::tonk_send(unsigned channel, const uint8_t* data, uint64_t bytes64)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    const size_t bytes = static_cast<size_t>(bytes64);
    Result result;

    if (channel == TonkChannel_Unreliable) {
        result = Outgoing.QueueUnreliable(protocol::MessageType_Unreliable, data, bytes);
    }
    else if (channel >= TonkChannel_Reliable0 && channel <= TonkChannel_Reliable0 + TonkChannel_Last) {
        const unsigned messageType = protocol::MessageType_Reliable + channel - TonkChannel_Reliable0;

        result = Outgoing.QueueReliable(messageType, data, bytes);
    }
    else if (channel >= TonkChannel_LowPri0 && channel <= TonkChannel_LowPri0 + TonkChannel_Last) {
        const unsigned messageType = protocol::MessageType_LowPri + channel - TonkChannel_LowPri0;

        result = Outgoing.QueueReliable(messageType, data, bytes);
    }
    else if (channel == TonkChannel_Unordered) {
        result = Outgoing.QueueUnordered(data, bytes);
    }
    else {
        result = Result("Connection::tonk_send", "Invalid channel input parameter. See documentation", ErrorType::Tonk, Tonk_InvalidChannel);
    }

    if (result.IsFail())
    {
        Logger.Error("tonk_send failed: ", result.ToJson());
        SelfRefCount.StartShutdown((unsigned)result.GetErrorCode(), result);
    }

    return result;
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void Connection::tonk_flush()
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    SelfRefCount.IncrementReferences();
    AsioEventStrand->post([this]()
    {
        TONK_DEBUG_ASSERT(AsioEventStrand->running_in_this_thread());
        Result flushResult = Outgoing.Flush();
        if (flushResult.IsFail()) {
            Logger.Error("tonk_flush failed: ", flushResult.ToJson());
            SelfRefCount.StartShutdown(Tonk_Error, flushResult);
        }

        SelfRefCount.DecrementReferences();
    });
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void Connection::tonk_close()
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    // Begin disconnect if it has not happened yet
    SelfRefCount.StartShutdown(Tonk_AppRequest, Result("Connection::tonk_close", "Application requested session end", ErrorType::Tonk, Tonk_AppRequest));

    // The disconnection will complete on the timer tick green thread
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void Connection::tonk_free()
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    SelfRefCount.AppRelease();

    // The object destruction will complete on the timer tick green thread
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

uint16_t Connection::tonk_to_remote_time_16(uint64_t localUsec)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());
    return TimeSync.ToRemoteTime16(localUsec);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

uint64_t Connection::tonk_from_local_time_16(uint16_t networkTimestamp16)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    // Grab a recent timestamp as a basis for truncated timestamp decompression
    const uint64_t localUsec = LastTickUsec;

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());
    return TimeSync.FromLocalTime16(localUsec, networkTimestamp16);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

uint32_t Connection::tonk_to_remote_time_23(uint64_t localUsec)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());
    return TimeSync.ToRemoteTime23(localUsec);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

uint64_t Connection::tonk_from_local_time_23(uint32_t networkTimestamp23)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    // Grab a recent timestamp as a basis for truncated timestamp decompression
    const uint64_t localUsec = LastTickUsec;

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());
    return TimeSync.FromLocalTime23(localUsec, networkTimestamp23);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

Result Connection::tonk_p2p_connect(Connection* bob)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    TONK_DEBUG_ASSERT(SelfRefCount.DoesAppHoldReference());

    if (!bob->TimeSync.IsSynchronized())
        return Result("Connection::tonk_p2p_connect", "Bob is not time-synchronized yet. Wait a bit longer", ErrorType::Tonk, Tonk_InvalidInput);
    if (!TimeSync.IsSynchronized())
        return Result("Connection::tonk_p2p_connect", "Alice is not time-synchronized yet. Wait a bit longer", ErrorType::Tonk, Tonk_InvalidInput);

    // Generate an encryption key for the two peers
    uint8_t keyBytes[8];
    Result result = SecureRandom_Next(keyBytes, 8);
    if (result.IsFail())
    {
        Logger.Error("SecureRandom_Next failed: ", result.ToJson());
        TONK_DEBUG_BREAK();
        return result;
    }
    const uint64_t encryptionKey = siamese::ReadU64_LE(keyBytes);

    // Collect OWD measurements
    const uint64_t bobMinOwdUsec = bob->TimeSync.GetMinimumOneWayDelayUsec();
    const uint64_t aliceMinOwdUsec = TimeSync.GetMinimumOneWayDelayUsec();

    // Calculate protocol round interval
    uint64_t roundIntervalUsec = (bobMinOwdUsec + aliceMinOwdUsec) * 2;
    if (roundIntervalUsec > protocol::kMaxP2PRoundIntervalUsec)
        roundIntervalUsec = protocol::kMaxP2PRoundIntervalUsec;
    if (roundIntervalUsec < protocol::kMinP2PRoundIntervalUsec)
        roundIntervalUsec = protocol::kMinP2PRoundIntervalUsec;
    const uint16_t roundIntervalMsec = static_cast<uint16_t>(roundIntervalUsec / 1000);

    // Calculate first shot time
    uint64_t shotDelayUsec = bobMinOwdUsec;
    if (shotDelayUsec < aliceMinOwdUsec)
        shotDelayUsec = aliceMinOwdUsec;
    shotDelayUsec *= 2;
    if (shotDelayUsec > protocol::kMaxP2PRoundIntervalUsec)
        shotDelayUsec = protocol::kMaxP2PRoundIntervalUsec;
    if (shotDelayUsec < protocol::kMinP2PRoundIntervalUsec)
        shotDelayUsec = protocol::kMinP2PRoundIntervalUsec;
    const uint64_t shotTimeLocalUsec = GetTicksUsec() + shotDelayUsec;

    // Write Alice's control message
    protocol::P2PConnectParams aliceParams;
    aliceParams.EncryptionKey = encryptionKey;
    aliceParams.PeerExternalAddress = bob->Outgoing.GetPeerUDPAddress();
    aliceParams.PeerNATMappedPort = bob->RemoteNATMapExternalPort;
    aliceParams.SelfExternalPort = Outgoing.GetPeerUDPAddress().port();
    aliceParams.WinTies = true;
    aliceParams.ProtocolRoundIntervalMsec = roundIntervalMsec;
    aliceParams.ShotTS16 = TimeSync.ToRemoteTime16(shotTimeLocalUsec);
    uint8_t aliceControlData[1 + protocol::P2PConnectParams::kMaxBytes];
    aliceControlData[0] = protocol::ControlChannel_S2C_Peer2PeerConnect;
    siamese::WriteByteStream aliceWriter(aliceControlData + 1, protocol::P2PConnectParams::kMaxBytes);
    aliceParams.Write(aliceWriter);

    // Write Bob's control message
    protocol::P2PConnectParams bobParams;
    bobParams.EncryptionKey = encryptionKey;
    bobParams.PeerExternalAddress = Outgoing.GetPeerUDPAddress();
    bobParams.PeerNATMappedPort = RemoteNATMapExternalPort;
    bobParams.SelfExternalPort = bob->Outgoing.GetPeerUDPAddress().port();
    bobParams.WinTies = false;
    bobParams.ProtocolRoundIntervalMsec = roundIntervalMsec;
    bobParams.ShotTS16 = bob->TimeSync.ToRemoteTime16(shotTimeLocalUsec);
    uint8_t bobControlData[1 + protocol::P2PConnectParams::kMaxBytes];
    bobControlData[0] = protocol::ControlChannel_S2C_Peer2PeerConnect;
    siamese::WriteByteStream bobWriter(bobControlData + 1, protocol::P2PConnectParams::kMaxBytes);
    bobParams.Write(bobWriter);

    // Write Alice's message
    result = Outgoing.QueueControl(
        aliceControlData,
        1 + aliceWriter.WrittenBytes);
    if (result.IsFail())
    {
        Logger.Error("Alice's QueueMessage failed: ", result.ToJson());
        TONK_DEBUG_BREAK();
        return result;
    }

    // Write Bob's message
    result = bob->Outgoing.QueueControl(
        bobControlData,
        1 + bobWriter.WrittenBytes);
    if (result.IsFail())
    {
        Logger.Error("Bob's QueueMessage failed: ", result.ToJson());
        TONK_DEBUG_BREAK();
        return result;
    }

    // Flush both connections
    tonk_flush();
    bob->tonk_flush();

    return Result::Success();
}


} // namespace tonk
