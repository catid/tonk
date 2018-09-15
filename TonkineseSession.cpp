/** \file
    \brief Tonk Implementation: Application Session
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonkinese nor the names of its contributors may be
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

#include "TonkineseSession.h"

namespace tonk {

static logger::Channel ModuleLogger("Socket", MinimumLogLevel);


//------------------------------------------------------------------------------
// ApplicationSession

ApplicationSession::ApplicationSession()
    : Logger("Socket", MinimumLogLevel)
{
    ActiveSessionSocketCount++;
}

ApplicationSession::~ApplicationSession()
{
    ActiveSessionSocketCount--;

    TONK_DEBUG_ASSERT(!SelfRefCount.ObjectInUse());
}

Result ApplicationSession::Initialize(const TonkSocketConfig& config)
{
    Result result;

    try
    {
        setConfig(config);

        // Set logger prefix
        if (config.UDPListenPort == 0)
            Logger.SetPrefix("[Client Port] ");
        else
        {
            std::ostringstream oss;
            oss << "[Port " << std::to_string(config.UDPListenPort) << "] ";
            Logger.SetPrefix(oss.str());
        }

        Context = MakeSharedNoThrow<asio::io_context>();
        if (!Context)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }
        Context->restart();
        Ticker = MakeUniqueNoThrow<asio::steady_timer>(*Context);
        AsioConnectStrand = MakeSharedNoThrow<asio::io_context::strand>(*Context);

        if (!Ticker || !AsioConnectStrand)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        /*
            Asio bug work-around: If we do not keep some work queued for Asio it
            will internally call stop() for some reason and then use a ton of
            CPU expecting us to end our worker threads.  The best way I've found
            to prevent the work-queue to go to 0 items is to keep a timer queued
            from before the first worker starts running.  The timer OnTick()
            keeps re-queuing a timer tick so the work-queue is never empty.
        */
        postNextTimer();

        TONK_DEBUG_ASSERT(Config.WorkerCount >= protocol::kMinWorkerCount);
        TONK_DEBUG_ASSERT(Config.WorkerCount <= protocol::kMaxWorkerCount);

        // Start worker thread loops spinning
        for (unsigned ii = 0; ii < Config.WorkerCount; ++ii) {
            Threads.push_back(MakeUniqueNoThrow<std::thread>(&ApplicationSession::workerLoop, this));
        }

        result = MainSocket.Initialize({
            &ReadBufferAllocator,
            &SelfRefCount,
            &Logger,
            Context,
            this,
            &P2PKeyMap,
            &MappedPort,
            &IdMap
        },
            IPVersion::V4,
            (uint16_t)Config.UDPListenPort,
            &Config,
            reinterpret_cast<TonkSocket>(this),
            AsioConnectStrand);

        // Clear application-provided pointers to avoid dereferencing them later
        Config.InterfaceAddress = nullptr;

        if (result.IsFail()) {
            goto OnError;
        }

        // If UPnP is requested and a port was explicitly bound for server-mode:
        if (0 != (Config.Flags & TONK_FLAGS_ENABLE_UPNP))
        {
            const uint16_t port = MainSocket.GetLocalPort();
            if (port != 0)
            {
                // TBD: Add for P2P connect ports also?
                MainSocket.AddFirewallRule();

                // Kick off adding a UDP port mapping via the GatewayPortMapper module
                MappedPort = gateway::RequestPortMap(port, config.InterfaceAddress);
            }
        }
    }
    catch (...)
    {
        TONK_DEBUG_BREAK(); // Should never happen except out of memory error
        result = Result("ApplicationSession::Initialize", "Exception!", ErrorType::Tonk);
        goto OnError;
    }

OnError:
    if (result.IsFail())
    {
        Logger.Error("ApplicationSession::Initialize failed: ", result.ToJson());
        Shutdown();
    }

    return result;
}

void ApplicationSession::postNextTimer()
{
    static const unsigned kTickIntervalUsec = 1000 * 1000; /// 1 second
    Ticker->expires_after(std::chrono::microseconds(kTickIntervalUsec));

    Ticker->async_wait(([this](const asio::error_code& error)
    {
        if (!error) {
            postNextTimer();
        }
    }));
}

void ApplicationSession::setConfig(const TonkSocketConfig& config)
{
    Config = config;

    // Clean up config
    static const unsigned kMinBufferBytes = 4000; // bytes
    static const unsigned kMaxBufferBytes = 4000000; // bytes

    if (Config.UDPSendBufferSizeBytes < kMinBufferBytes) {
        Config.UDPSendBufferSizeBytes = kMinBufferBytes;
    }
    else if (Config.UDPSendBufferSizeBytes > kMaxBufferBytes) {
        Config.UDPSendBufferSizeBytes = kMaxBufferBytes;
    }

    if (Config.UDPRecvBufferSizeBytes < kMinBufferBytes) {
        Config.UDPRecvBufferSizeBytes = kMinBufferBytes;
    }
    else if (Config.UDPRecvBufferSizeBytes > kMaxBufferBytes) {
        Config.UDPRecvBufferSizeBytes = kMaxBufferBytes;
    }

    // If worker count is "auto":
    if (Config.WorkerCount <= 0)
    {
        const unsigned cpu_cores = std::thread::hardware_concurrency();
        if (cpu_cores <= 2) {
            Config.WorkerCount = 2;
        }
        else {
            Config.WorkerCount = cpu_cores - 1;
        }
    }
    if (Config.WorkerCount < protocol::kMinWorkerCount) {
        Config.WorkerCount = protocol::kMinWorkerCount;
    }
    if (Config.WorkerCount > protocol::kMaxWorkerCount) {
        Config.WorkerCount = protocol::kMaxWorkerCount;
    }

    // Make several callbacks optional:
    if (!Config.OnIncomingConnection)
    {
        // Disallow incoming connections if there is no handler
        Config.MaximumClients = 0;

        Config.OnIncomingConnection = [](
            TonkAppContextPtr /*context*/,
            TonkConnection /*connection*/,
            const TonkAddress* /*address*/,
            TonkConnectionConfig* /*configOut*/) -> uint32_t
        {
            return TONK_DENY_CONNECTION;
        };
    }
    if (!Config.OnP2PConnectionStart)
    {
        Config.OnP2PConnectionStart = [](
            TonkAppContextPtr /*context*/,
            TonkConnection /*connection*/,
            const TonkAddress* /*address*/,
            TonkConnectionConfig* /*configOut*/) -> uint32_t
        {
            return TONK_DENY_CONNECTION;
        };
    }
}

Result ApplicationSession::tonk_connect(
    TonkConnectionConfig config,
    const char* serverHostname,
    uint16_t serverPort,
    TonkConnection* connectionOut)
{
    MakeCallbacksOptional(config);

    Result result;

    Connection* connection = new (std::nothrow) Connection;
    *connectionOut = reinterpret_cast<TonkConnection>(connection);

    if (!connection)
        result = Result("ApplicationSession::tonk_connect", "Out of memory", ErrorType::Tonk);
    else
    {
        result = connection->InitializeOutgoingConnection({
            &SelfRefCount,
            &MainSocket,
            &MainSocket.AddressMap,
            &IdMap,
            &P2PKeyMap,
            MappedPort.get(),
            Context,
            nullptr, // No FloodDetector backref for outgoing connections
            UDPAddress(), // No Disconnect addr key
            this
        },
            &Config,
            config,
            serverHostname,
            serverPort);
    }

    if (result.IsFail())
    {
        delete connection;
        *connectionOut = nullptr;
    }

    return result;
}

#define SSDP_REQUEST \
    "M-SEARCH * HTTP/1.1\r\n" \
    "HOST: 239.255.255.250:1900\r\n" \
    "MAN: \"ssdp:discover\"\r\n" \
    "MX: 2\r\n" \
    "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n" \
    "USER-AGENT: Tonk GatewayPortMapper\r\n" \
    "\r\n"

Result ApplicationSession::tonk_advertise(
    const char* ipString,
    uint16_t port,
    const void* data,
    uint32_t bytes)
{
    // Treat empty string as IPv4 broadcast
    if (*ipString == '\0') {
        ipString = "255.255.255.255";
    }

    // Convert IP string to IP address
    asio::error_code ec;
    asio::ip::address ipAddr = asio::ip::make_address(ipString, ec);
    if (ec) {
        return Result("Invalid IP string", ec.message(), ErrorType::Asio, ec.value());
    }

    // Allocate datagram
    DatagramBuffer datagram;
    datagram.AllocatorForFree = &ReadBufferAllocator;
    datagram.Data = datagram.AllocatorForFree->Allocate(bytes + protocol::kAdvertisementOverheadBytes);
    datagram.FreePtr = datagram.Data;
    datagram.Bytes = bytes + protocol::kAdvertisementOverheadBytes;

    // Construct datagram
    memcpy(datagram.Data, data, bytes);
    siamese::WriteU32_LE(datagram.Data + bytes, protocol::kMagicAdvertisement);
    datagram.Data[bytes + 4] = static_cast<uint8_t>(protocol::Unconnected_Advertisement);
    datagram.Data[bytes + 4 + 1] = 0;
    datagram.Data[bytes + 4 + 2] = 0;

    // Shipit
    MainSocket.Send(UDPAddress(ipAddr, port), datagram, &SelfRefCount);

    return Result::Success();
}

Result ApplicationSession::tonk_inject(
    uint16_t sourcePort,
    const uint8_t* data,
    uint32_t bytes
)
{
    // Address is set to IPv4 loopback
    UDPAddress addr = UDPAddress(
        asio::ip::address_v4::loopback(),
        sourcePort);

    // Pass it to the main socket only
    return MainSocket.InjectRecvfrom(addr, data, bytes);
}

void ApplicationSession::tonk_socket_destroy()
{
    Shutdown();
}

void ApplicationSession::workerLoop()
{
    SetCurrentThreadName("ApplicationSession:Worker");

    //Logger.Debug("ApplicationSession: Entering worker-thread loop");

    asio::error_code ec;

    while (!Terminated)
    {
        Context->run(ec);
        if (ec) {
            Logger.Warning("Context run failed: ", ec.message(), " code=", ec.value());
        }
    }

    //Logger.Debug("ApplicationSession: Leaving worker-thread loop");
}

void ApplicationSession::OnP2PStartConnect(
    uint16_t myRendezvousVisibleSourcePort,
    uint32_t rendezvousServerConnectionId,
    const protocol::P2PConnectParams& startParams)
{
    SelfRefCount.IncrementReferences();
    AsioConnectStrand->post([myRendezvousVisibleSourcePort, rendezvousServerConnectionId, startParams, this]()
    {
        onP2PStartConnect(myRendezvousVisibleSourcePort, rendezvousServerConnectionId, startParams);
        SelfRefCount.DecrementReferences();
    });
}

void ApplicationSession::initializeP2PSockets(const protocol::P2PConnectParams& startParams)
{
    Logger.Debug("Initializing P2P sockets in response to P2P connect request from peer");

    P2PSocketsInitialized = true;

    // Create an IPv4/IPv6 socket based on the peer address type
    const IPVersion bindIPVersion = startParams.PeerExternalAddress.address().is_v6() ? IPVersion::V6 : IPVersion::V4;

    siamese::PCGRandom portPrng;
    portPrng.Seed(siamese::GetTimeUsec());

    for (unsigned portIndex = 0; portIndex < protocol::kNATMaxPorts; ++portIndex)
    {
        uint16_t port = protocol::CalculateExactNATPort(startParams.SelfExternalPort, portIndex);

        // After the fuzzy ports, explicitly choose more random port numbers
        if (port == 0 && portIndex >= protocol::kNATRound23Fuzzy_PortCount) {
            port = protocol::CalculateRandomNATPort(portPrng);
        }

        static const unsigned kPortBindRetries = 2;
        for (unsigned j = 0; j < kPortBindRetries; ++j)
        {
            Result result = ProbeListenerSockets[portIndex].Initialize({
                &ReadBufferAllocator,
                &SelfRefCount,
                &Logger,
                Context,
                this,
                &P2PKeyMap,
                &MappedPort,
                &IdMap
            },
                bindIPVersion,
                port,
                &Config,
                reinterpret_cast<TonkSocket>(this),
                AsioConnectStrand);

            if (result.IsGood())
            {
                if (port != 0) {
                    Logger.Debug("Bound NAT probe socket ", portIndex, " to port ", port);
                }
                break;
            }

            Logger.Warning("Error while initializing probe socket ",
                portIndex, " bound to port ", port, ": ", result.ToJson());

            port = 0;
        }
    }
}

void ApplicationSession::onP2PStartConnect(
    uint16_t myRendezvousVisibleSourcePort,
    uint32_t rendezvousServerConnectionId,
    const protocol::P2PConnectParams& startParams)
{
    if (SelfRefCount.IsShutdown())
    {
        Logger.Warning("P2P connection aborted during shutdown");
        return;
    }

    // Attempt to find a duplicate connection request
    IConnection* icon = P2PKeyMap.FindByKey(startParams.GetExpectedIncomingProbeKey());
    if (icon)
    {
        Logger.Warning("Ignoring simultaneous P2P connect request with the same incoming probe key");
        icon->SelfRefCount.DecrementReferences();
        return;
    }

    // Note that this function is called from the Asio green thread,
    // so the P2P sockets will only be initialized once
    if (!P2PSocketsInitialized) {
        initializeP2PSockets(startParams);
    }

    Logger.Info("Server requesting that we P2P connect to ",
        startParams.PeerExternalAddress.address().to_string(), ":", startParams.PeerExternalAddress.port(),
        " - WinTies=", startParams.WinTies, " Interval=",
        startParams.ProtocolRoundIntervalMsec, ". Waiting for probe key = ",
        HexString(startParams.GetExpectedIncomingProbeKey()),
        ". Requesting ports from ", myRendezvousVisibleSourcePort);

    Connection* connection = new(std::nothrow) Connection;

    if (!connection)
    {
        Logger.Error("OOM during new P2P connection");
        return;
    }

    Result result = connection->InitializeP2PConnection({
        &SelfRefCount,
        &MainSocket,
        &MainSocket.AddressMap,
        &IdMap,
        &P2PKeyMap,
        MappedPort.get(),
        Context,
        nullptr, // No FloodDetector backref for P2P connections
        UDPAddress(), // No Disconnect addr key
        this
    },
        &Config,
        rendezvousServerConnectionId,
        startParams);

    if (result.IsFail())
    {
        // Result already logged in InitializeP2PConnection()
        delete connection;
        connection = nullptr;
    }
}

void ApplicationSession::SendNATProbe(
    unsigned socketIndex,
    uint8_t* probeData,
    const UDPAddress& destAddress,
    RefCounter* refCounter)
{
    TONK_DEBUG_ASSERT(socketIndex < protocol::kNATMaxPorts || socketIndex == kNATPortMainSocketIndex);
    TONK_DEBUG_ASSERT(P2PSocketsInitialized);

    DatagramBuffer datagram;
    datagram.Data = probeData;
    datagram.Bytes = protocol::kNATProbeBytes;

    if (socketIndex == kNATPortMainSocketIndex)
    {
        Logger.Debug("Sent probe from main socket -> port ", destAddress.port());
        MainSocket.Send(destAddress, datagram, refCounter);
    }
    else if (ProbeListenerSockets[socketIndex].IsValid())
    {
        Logger.Debug("Sent probe from socket ", socketIndex, " -> port ", destAddress.port());
        ProbeListenerSockets[socketIndex].Send(destAddress, datagram, refCounter);
    }
    else {
        Logger.Warning("Probe socket index ", socketIndex, " unavailable");
    }
}

void ApplicationSession::Shutdown()
{
    Logger.Debug("ApplicationSession::Shutdown - App requesting termination");

    // Mark socket as shutdown
    SelfRefCount.AppRelease();

    // Wait for Connections to stop
    const uint64_t t0 = siamese::GetTimeMsec();
    while (IdMap.GetConnectionCount() > 0)
    {
        const uint64_t waitedMsec = siamese::GetTimeMsec() - t0;

        // Give connections 1 second to terminate then close sockets
        static const uint64_t kDelayBeforeCloseSocketMsec = 1000;
        if (waitedMsec >= kDelayBeforeCloseSocketMsec)
        {
            TONK_DEBUG_BREAK(); // May indicate a hang somewhere
            Logger.Warning("Wait timeout for connections to close");
            break;
        }

        // Wait a little longer
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const uint64_t waitedMsec0 = siamese::GetTimeMsec() - t0;
    if (SelfRefCount.ObjectInUse()) {
        Logger.Warning("Waited ", waitedMsec0, " msec for connections to close.  Object is in use");
    }
    else {
        Logger.Warning("Waited ", waitedMsec0, " msec for connections to close.  Object is not in use");
    }

    // Close sockets
    if (AsioConnectStrand)
    {
        SelfRefCount.IncrementReferences();
        AsioConnectStrand->post([this]()
        {
            // Close the socket to shake out any other background work
            MainSocket.CloseSocket();

            if (P2PSocketsInitialized)
            {
                // Close all the NAT probe sockets
                // They will not be opened after this point while in shutdown
                for (unsigned i = 0; i < protocol::kNATMaxPorts; ++i) {
                    ProbeListenerSockets[i].CloseSocket();
                }
            }

            SelfRefCount.DecrementReferences();
        });
    }

    // Wait forever until references are released
    while (SelfRefCount.ObjectInUse()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const uint64_t waitedMsecTotal = siamese::GetTimeMsec() - t0;
    Logger.Info("Waited ", waitedMsecTotal, " total msec for references to be released");

    if (Context) {
        // Note there is no need to stop or cancel timer ticks or sockets
        Context->stop();
    }

    // Allow worker threads to stop
    // Note this has to be done after cancel/close of sockets above because
    // otherwise Asio can hang waiting for any outstanding callbacks.
    Terminated = true;

    // Wait for threads to stop
    for (auto& thread : Threads)
    {
        if (thread)
        {
            try
            {
                if (thread->joinable()) {
                    thread->join();
                }
            }
            catch (std::system_error& err) {
                Logger.Warning("Exception while joining thread: ", err.what());
            }
            thread = nullptr;
        }
    }

    // Shutdown sockets
    MainSocket.Shutdown();
    if (P2PSocketsInitialized) {
        for (unsigned i = 0; i < protocol::kNATMaxPorts; ++i) {
            ProbeListenerSockets[i].Shutdown();
        }
    }

    Threads.clear();

    Ticker = nullptr;
    AsioConnectStrand = nullptr;
    Context = nullptr;

    Logger.Debug("Read allocator memory used: ", ReadBufferAllocator.GetUsedMemory() / 1000, " KB");

    // Shut down port mapper
    MappedPort = nullptr;
}


} // namespace tonk
