/** \file
    \brief Tonk Implementation: UDP Socket
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

#include "TonkineseUDP.h"
#include "TonkineseConnection.h"
#include "TonkineseFirewall.h"

namespace tonk {

static logger::Channel ModuleLogger("UDP", MinimumLogLevel);


//------------------------------------------------------------------------------
// Artifical Packetloss

#ifdef TONK_ENABLE_ARTIFICIAL_PACKETLOSS

// If PRNG produces a value below this, then it is a simulated packetloss event
static uint32_t m_ArtificialPacketlossThreshold = 0;

void UDPSocket::SetArtificialPacketloss(float ploss)
{
    m_ArtificialPacketlossThreshold = static_cast<uint32_t>(0x7fffffff * ploss);
}

#endif // TONK_ENABLE_ARTIFICIAL_PACKETLOSS


//------------------------------------------------------------------------------
// UDPSocket

/// Set options for UDP sockets
static Result SetUDPSocketOptions(
    asio::ip::udp::socket& socket,
    unsigned sendBufferSizeBytes,
    unsigned recvBufferSizeBytes)
{
    asio::error_code ec;
    const char* operation;

    operation = "broadcast=true";
    socket.set_option(asio::socket_base::broadcast(true), ec);
    if (ec) {
        goto OnError;
    }

    operation = "send_buffer_size";
    socket.set_option(asio::socket_base::send_buffer_size(sendBufferSizeBytes), ec);
    if (ec) {
        goto OnError;
    }

    operation = "receive_buffer_size";
    socket.set_option(asio::socket_base::receive_buffer_size(recvBufferSizeBytes), ec);
    if (ec) {
        goto OnError;
    }

    operation = "reuse_address";
    socket.set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        goto OnError;
    }

    return Result::Success();

OnError:
    return Result("set_option", ec.message() + " from " + operation, ErrorType::Asio, ec.value());
}

Result UDPSocket::Initialize(
    const Dependencies& deps,
    IPVersion bindIPVersion,
    uint16_t port,
    const TonkSocketConfig* socketConfig,
    TonkSocket apiTonkSocket,
    std::shared_ptr<asio::io_context::strand> asioConnectStrand)
{
    Deps = deps;
    SocketConfig = socketConfig;
    APITonkSocket = apiTonkSocket;
    AsioConnectStrand = asioConnectStrand;
    TONK_DEBUG_ASSERT(asioConnectStrand != nullptr);
    SocketClosed = false;

#ifdef TONK_ENABLE_ARTIFICIAL_PACKETLOSS
#ifdef TONK_DEFAULT_ARTIFICIAL_PACKETLOSS
    SetArtificialPacketloss(TONK_DEFAULT_ARTIFICIAL_PACKETLOSS);
#endif
    ArtificialPacketlossPRNG.Seed(siamese::GetTimeUsec());
#endif

    Result result;

    try
    {
        result = FloodDetect.Initialize(
            socketConfig->MaximumClientsPerIP,
            socketConfig->MinuteFloodThresh
        );
        if (result.IsFail()) {
            goto OnError;
        }

        // Create a UDP socket for the connection attempt
        Socket = MakeUniqueNoThrow<asio::ip::udp::socket>(*Deps.Context);
        if (!Socket)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        asio::error_code ec;

        // If application specified an interface address:
        UDPAddress bindAddress;
        if (socketConfig->InterfaceAddress)
        {
            // Convert provided interface address to Asio address
            asio::ip::address bindAddr = asio::ip::make_address(socketConfig->InterfaceAddress, ec);
            if (ec)
            {
                result = Result("Provided InterfaceAddress was invalid", ec.message(), ErrorType::Asio, ec.value());
                goto OnError;
            }

            // Bind to the provided interface address
            bindAddress = UDPAddress(bindAddr, port);
        }
        else
        {
            // Create an IPv4/IPv6 socket based on the server address type
            asio::ip::udp internet_protocol = (bindIPVersion == IPVersion::V4)
                ? asio::ip::udp::v4() : asio::ip::udp::v6();

            // Note: Asio default endpoint address is the any address e.g. INADDR_ANY
            bindAddress = UDPAddress(internet_protocol, port);
        }

        // Open socket
        Socket->open(bindAddress.protocol(), ec);
        if (ec)
        {
            result = Result("UDP socket open failed", ec.message(), ErrorType::Asio, ec.value());
            goto OnError;
        }

        // Set common socket options before bind
        result = SetUDPSocketOptions(
            *Socket,
            socketConfig->UDPSendBufferSizeBytes,
            socketConfig->UDPRecvBufferSizeBytes);
        if (result.IsFail()) {
            goto OnError;
        }

        // Bind socket
        Socket->bind(bindAddress, ec);
        if (ec)
        {
            result = Result("UDP socket bind failed", ec.message(), ErrorType::Asio, ec.value());
            goto OnError;
        }

        // Get local bound port
        const UDPAddress local_ep = Socket->local_endpoint(ec);
        if (ec)
        {
            result = Result("local_endpoint failed", ec.message(), ErrorType::Asio, ec.value());
            goto OnError;
        }
        LocalBoundPort = local_ep.port();

#ifdef TONK_DONT_FRAGMENT
        result = dontFragment(true);
        if (result.IsFail()) {
            goto OnError;
        }
#endif // TONK_DONT_FRAGMENT

#ifdef TONK_IGNORE_ICMP_UNREACHABLE
        result = ignoreUnreachable(true);
        if (result.IsFail()) {
            goto OnError;
        }
#endif // TONK_IGNORE_ICMP_UNREACHABLE

        // Allocate the first read buffer
        uint8_t* readBuffer = Deps.ReadBufferAllocator->Allocate(kReadBufferBytes);
        if (!readBuffer)
        {
            result = Result::OutOfMemory();
            goto OnError;
        }

        // Post the first read
        Deps.SocketRefCount->IncrementReferences();
        postNextRead(readBuffer);
    }
    catch (...)
    {
        TONK_DEBUG_BREAK(); // Should never happen except out of memory error
        result = Result("UDPSocket::CreateSocket", "Exception!", ErrorType::Asio);
    }

OnError:
    if (result.IsFail())
    {
        Deps.Logger->Error("UDPSocket::Initialize failed: ", result.ToJson());
        CloseSocket();
    }
    return result;
}

void UDPSocket::AddFirewallRule()
{
    // Add firewall rule from a background thread
    Deps.SocketRefCount->IncrementReferences();
    asio::post(*Deps.Context, [this]()
    {
        Firewall_AddPort(LocalBoundPort);
        Deps.SocketRefCount->DecrementReferences();
    });
}

Result UDPSocket::dontFragment(bool enabled)
{
#ifdef _WIN32
    const DWORD behavior = enabled ? TRUE : FALSE;

    const int iResult = ::setsockopt(
        Socket->native_handle(),
        IPPROTO_IP,
        IP_DONTFRAGMENT,
        (const char *)&behavior,
        sizeof(behavior));

    if (0 != iResult) {
        return Result("UDPSocket::dontFragment", "setsockopt failed", ErrorType::LastErr, ::WSAGetLastError());
    }
#else
    TONK_UNUSED(enabled);
#endif

    return Result::Success();
}

Result UDPSocket::ignoreUnreachable(bool enabled)
{
#ifdef _WIN32
    u_long behavior = enabled ? FALSE : TRUE;

    // FALSE = Disable behavior where, after receiving an ICMP Unreachable message,
    // WSARecvFrom() will fail.  Disables ICMP completely; normally this is good.
    // But when you're writing a client endpoint, you probably want to listen to
    // ICMP Port Unreachable or other failures until you get the first packet.
    // After that call IgnoreUnreachable() to avoid spoofed ICMP exploits.

    const int iResult = ::ioctlsocket(
        Socket->native_handle(),
        SIO_UDP_CONNRESET,
        &behavior);

    if (0 != iResult) {
        return Result("UDPSocket::ignoreUnreachable", "ioctlsocket failed", ErrorType::LastErr, ::WSAGetLastError());
    }
#else
    TONK_UNUSED(enabled);
#endif

    return Result::Success();
}

Result UDPSocket::InjectRecvfrom(
    const UDPAddress& addr,
    const uint8_t* data,
    unsigned bytes)
{
    // Grab the receive timestamp right away
    const uint64_t receiveUsec = siamese::GetTimeUsec();

    uint8_t* buffer = Deps.ReadBufferAllocator->Allocate(bytes);
    if (!buffer) {
        return Result::OutOfMemory();
    }
    memcpy(buffer, data, bytes);

    const BufferResult result = onUDPDatagram(receiveUsec, addr, buffer, bytes);
    if (result == BufferResult::Free) {
        Deps.ReadBufferAllocator->Free(buffer);
    }

    return Result::Success();
}

void UDPSocket::CloseSocket()
{
    SocketClosed = true;

    if (Socket)
    {
        asio::error_code error;
        Socket->close(error);

        if (error) {
            Deps.Logger->Warning("Failure while destroying UDP socket: ", error.message());
        }
    }
}

void UDPSocket::Send(
    const UDPAddress& destAddress,  // Destination address for send
    DatagramBuffer       datagram,  // Buffer to send
    RefCounter*        refCounter)  // Reference count to decrement when done
{
    // Note this function is responsible for decrementing the provided RefCounter when done

    TONK_DEBUG_ASSERT(IsValid());

    // If the application has set a send hook:
    if (SocketConfig->SendToHook)
    {
        SocketConfig->SendToHook(
            SocketConfig->SendToAppContextPtr,
            destAddress.port(),
            datagram.Data,
            datagram.Bytes);

        datagram.Free();
        return;
    }

    refCounter->IncrementReferences();
    TONK_IF_DEBUG(++OutstandingSends);

    // Asynchronous sendto()
    Socket->async_send_to(asio::buffer(datagram.Data, datagram.Bytes), destAddress,
        [this, datagram, refCounter](const asio::error_code& error, std::size_t /*sentBytes*/)
    {
        datagram.Free();

        // If an error occurred in sendto():
        if (error)
        {
            Result result("async_send_to failed",
                error.message(),
                ErrorType::Asio,
                error.value());

            Deps.Logger->Error("Send failed: ", result.ToJson());

            // TBD: These errors happen when switching on/off a VPN so we could
            // smooth that transition further by attaching ID to outgoing data
            // pre-emptively once we see a send failure.
        }

        TONK_IF_DEBUG(--OutstandingSends);
        refCounter->DecrementReferences();
    });
}

void UDPSocket::postNextRead(uint8_t* readBuffer)
{
    // Clear source address to allow us to check if it's filled later
    // during error handling
    SourceAddress = UDPAddress();

    TONK_IF_DEBUG(++OutstandingRecvs);

    // Dispatch asynchronous recvfrom()
    Socket->async_receive_from(
        asio::buffer(readBuffer, kReadBufferBytes),
        SourceAddress,
        [this, readBuffer](const asio::error_code& error, size_t bytes_transferred)
    {
        // Grab the receive timestamp right away
        const uint64_t receiveUsec = siamese::GetTimeUsec();

        TONK_IF_DEBUG(--OutstandingRecvs);

        uint8_t* nextReadBuffer = readBuffer;

        // If there was an error reported:
        if (error)
        {
            // If no source address was provided:
            if (SourceAddress.address().is_unspecified())
            {
                if (!SocketClosed) {
                    Deps.Logger->Warning("Socket receive failed unexpectedly");
                }

                Deps.SocketRefCount->StartShutdown(
                    Tonk_NetworkFailed,
                    Result("async_receive_from failed",
                        error.message(),
                        ErrorType::Asio,
                        error.value()));

                // Note this stops posting read requests
                Deps.SocketRefCount->DecrementReferences();

                return;
            }

            // Otherwise we assume it is due to ICMP message from a peer:

            // Handle peer-specific failure that is not catastrophic
            IConnection* icon = AddressMap.FindByAddr(SourceAddress);
            if (icon) {
                Connection* connection = reinterpret_cast<Connection*>(icon);
                connection->OnICMPError(error);
            }

            // Fall-thru to continue reading datagrams
        }
        else if (bytes_transferred <= 0)
        {
            if (!SocketClosed) {
                Deps.Logger->Warning("Socket closed unexpectedly");
            }

            // Start shutdown
            Deps.SocketRefCount->StartShutdown(Tonk_NetworkFailed, Result("UDP socket closed", "Socket closed", ErrorType::Tonk, Tonk_NetworkFailed));

            // Note no read request is posted here
            Deps.SocketRefCount->DecrementReferences();

            return;
        }
        else
        {
#ifdef TONK_FOIL_BUFFER_STEALING
            // Clear remaining receive buffer to foil buffer-stealing attacks
            memset(buffer + bytes_transferred, 0xfe, bufferBytes - bytes_transferred);
#endif

#ifdef TONK_ENABLE_ARTIFICIAL_PACKETLOSS
            if (m_ArtificialPacketlossThreshold > 0)
            {
                const uint32_t word = ArtificialPacketlossPRNG.Next() & 0x7fffffff;
                if (word < m_ArtificialPacketlossThreshold)
                {
                    //ModuleLogger.Trace("ARTIFICAL DROPPED DATAGRAM HERE");

                    // Reuse the same read buffer to post another read request
                    postNextRead(nextReadBuffer);

                    return; // Do not handle this datagram
                }
            }
#endif // TONK_ENABLE_ARTIFICIAL_PACKETLOSS

            TONK_DEBUG_ASSERT(bytes_transferred <= kReadBufferBytes);
            const unsigned receivedBytes = static_cast<unsigned>(bytes_transferred);

            const BufferResult result = onUDPDatagram(
                receiveUsec,
                SourceAddress,
                readBuffer,
                receivedBytes);

            if (result == BufferResult::InUse) {
                // Allocate a new buffer for the next read
                nextReadBuffer = Deps.ReadBufferAllocator->Allocate(kReadBufferBytes);
            }
        }

        // Post next read buffer
        postNextRead(nextReadBuffer);
    });
}

UDPSocket::BufferResult UDPSocket::onUDPDatagram(
    const uint64_t receiveUsec,
    const UDPAddress& addr,
    uint8_t* readBuffer,
    const unsigned receivedBytes)
{
    // If datagram is truncated:
    if (protocol::DatagramIsTruncated(readBuffer, receivedBytes))
    {
        Deps.Logger->Warning("Ignoring truncated datagram (", receivedBytes, " bytes)");
        return BufferResult::Free;
    }

    DatagramBuffer datagram;
    datagram.Data = readBuffer;
    datagram.Bytes = receivedBytes;
    datagram.AllocatorForFree = Deps.ReadBufferAllocator;
    datagram.FreePtr = readBuffer;

    // Route the data to its connection by source address
    IConnection* icon = AddressMap.FindByAddr(addr);
    // Note the connection reference count was incremented by FindByAddr()
    if (icon)
    {
        // Shrink the datagram buffer before passing to connection
        Deps.ReadBufferAllocator->Shrink(datagram.Data, datagram.Bytes);

        // Pass the datagram to the Connection
        Connection* connection = reinterpret_cast<Connection*>(icon);
        connection->OnUDPDatagram(receiveUsec, addr, datagram);

        return BufferResult::InUse;
    }

    // Source not recognized
    return onUnrecognizedSource(
        addr,
        receiveUsec,
        datagram);
}

UDPSocket::BufferResult UDPSocket::onUnrecognizedSource(
    const UDPAddress& addr,
    const uint64_t receiveUsec,
    DatagramBuffer datagram)
{
    /*
        Footer protocol:
            ...
            [Handshake (12 bytes)]
            <Timestamp (3 bytes)>
            <Flags (1 byte)>
            <Encryption Tag (2 bytes)>
    */
    const uint8_t* footer = datagram.Data + datagram.Bytes - protocol::kEncryptionTagBytes - protocol::kFlagsBytes;
    protocol::FlagsReader flags(footer[0]);

    // If this is an unconnected datagram:
    if (!flags.IsConnectionDatagram())
    {
        const unsigned unconnectedType = flags.GetUnconnectedType();

        // If this is a NAT probe:
        if (protocol::Unconnected_P2PNATProbe == unconnectedType &&
            protocol::kNATProbeBytes == datagram.Bytes)
        {
            uint32_t key = siamese::ReadU16_LE(footer - 2);
            key = (key << 16) | siamese::ReadU16_LE(footer + 1);

            // If there is a NAT probe in progress:
            IConnection* icon = Deps.P2PKeyMap->FindByKey(key);
            // Note the connection reference count was incremented by FindByKey()
            if (icon) {
                Connection* connection = reinterpret_cast<Connection*>(icon);
                connection->OnNATProbe(addr, key, &AddressMap, this);
            }
            else {
                Deps.Logger->Warning("NAT probe received with key = ", HexString(key), " not found in map");
            }

            return BufferResult::Free;
        }

        // If this is an advertisement
        if (protocol::Unconnected_Advertisement == unconnectedType &&
            protocol::kAdvertisementOverheadBytes <= datagram.Bytes &&
            protocol::kMagicAdvertisement == siamese::ReadU32_LE(footer - 4))
        {
            // If the application is not requesting advertisements, skip the async work
            if (!SocketConfig->OnAdvertisement) {
                return BufferResult::Free;
            }

            // Run callback in the connect strand to avoid blocking datagram processing
            Deps.SocketRefCount->IncrementReferences();
            AsioConnectStrand->post([this, addr, datagram]()
            {
                std::string ipString = addr.address().to_string();

                // Convert IP address to string and pass to app with data
                SocketConfig->OnAdvertisement(
                    SocketConfig->AppContextPtr,
                    APITonkSocket,
                    ipString.c_str(),
                    addr.port(),
                    datagram.Data,
                    datagram.Bytes - protocol::kAdvertisementOverheadBytes);

                datagram.Free();

                Deps.SocketRefCount->DecrementReferences();
            });

            return BufferResult::InUse;
        }

        // Unexpected type from unrecognized source
        Deps.Logger->Warning("Ignoring unexpected unconnected type ", unconnectedType, " from unrecognized source (bytes = ", datagram.Bytes, ")");

        return BufferResult::Free;
    }

    const unsigned handshakeBytes = flags.HandshakeBytes();
    if (handshakeBytes <= 0)
    {
        // Non-handshake from unrecognized source
        Deps.Logger->Warning("Ignoring non-handshake packet from unrecognized source (bytes = ", datagram.Bytes, ")");

        Deps.SocketRefCount->IncrementReferences();
        AsioConnectStrand->post([this, addr]()
        {
            DatagramBuffer datagram;
            datagram.AllocatorForFree = Deps.ReadBufferAllocator;
            datagram.Bytes = protocol::kS2CUnknownSourceBytes;
            datagram.Data = Deps.ReadBufferAllocator->Allocate(protocol::kS2CUnknownSourceBytes);
            datagram.FreePtr = datagram.Data;

            if (datagram.Data)
            {
                protocol::GenerateS2CUnknownSource(datagram.Data);
                Send(addr, datagram, Deps.SocketRefCount);
            }

            Deps.SocketRefCount->DecrementReferences();
        });

        return BufferResult::Free;
    }

    // Get pointer to the handshake data
    const uint8_t* handshakeData = footer - handshakeBytes - protocol::kTimestampBytes;

    // Decode handshake type
    const protocol::HandshakeType handshakeType = protocol::handshake::GetType(handshakeData);

    // If this is a connection request datagram:
    if (handshakeType == protocol::HandshakeType_C2SConnectionRequest)
    {
        // Shrink the datagram buffer before passing to connection
        Deps.ReadBufferAllocator->Shrink(datagram.Data, datagram.Bytes);

        const uint64_t key = protocol::handshake::GetKey(handshakeData);

        Deps.SocketRefCount->IncrementReferences();
        AsioConnectStrand->post([this, key, addr, receiveUsec, datagram]()
        {
            onConnectRequest(key, addr, receiveUsec, datagram);
            Deps.SocketRefCount->DecrementReferences();
        });

        return BufferResult::InUse;
    }

    // If this is a peer2peer connection request:
    if (handshakeType == protocol::HandshakeType_Peer2PeerConnect)
    {
        const uint64_t key = protocol::handshake::GetKey(handshakeData);

        // Note: The P2PKeyMap should match the low 4 bytes of the encryption key
        const uint32_t connKey = static_cast<uint32_t>(key);

        IConnection* icon = Deps.P2PKeyMap->FindByKey(connKey);
        // Note the connection reference count was incremented by FindByKey()

        // If there is a NAT probe in progress:
        if (icon)
        {
            // Shrink the datagram buffer before passing to connection
            Deps.ReadBufferAllocator->Shrink(datagram.Data, datagram.Bytes);

            Connection* connection = reinterpret_cast<Connection*>(icon);
            connection->OnP2PConnection(
                receiveUsec,
                addr,
                datagram,
                key,
                &AddressMap,
                this);
            return BufferResult::InUse;
        }

        Deps.Logger->Warning("Peer2Peer connection with unrecognized key = ", HexString(key));

        // Unrecognized connection request
        return BufferResult::Free;
    }

    // If this handshake is updating a source address:
    if (handshakeType == protocol::HandshakeType_C2SUpdateSourceAddress)
    {
        const uint32_t id = protocol::handshake::GetConnectionId(handshakeData);
        const uint64_t key = protocol::handshake::GetKey(handshakeData);

        IConnection* icon = Deps.IdMap->FindById(id);
        // Note the connection reference count was incremented by FindById()
        if (icon)
        {
            Connection* connection = reinterpret_cast<Connection*>(icon);
            if (connection->GetEncryptionKey() == key)
            {
                // Shrink the datagram buffer before passing to connection
                Deps.ReadBufferAllocator->Shrink(datagram.Data, datagram.Bytes);

                Deps.Logger->Warning("Switching connection address for ID ", id);

                connection->PeerAddressChangesAllowed = true;

                connection->OnUDPDatagram(receiveUsec, addr, datagram);
                return BufferResult::InUse;
            }

            // FindById() has incremented the reference count, so decrement it here
            connection->SelfRefCount.DecrementReferences();
        }

        Deps.Logger->Warning("Reconnect failed: Unrecognized ID ", id);
        return BufferResult::Free;
    }

    // Not a handshake:
    Deps.Logger->Warning("Unrecognized handshake type");
    return BufferResult::Free;
}

void UDPSocket::onConnectRequest(
    uint64_t encryptionKey,
    const UDPAddress& addr,
    uint64_t receiveUsec,
    DatagramBuffer datagram)
{
    // All connection requests are serialized into this green thread

    // If client is already connected (indicated by source address found
    // in source address map), then handle the datagram in the green thread
    // of the connection and abort creating a new connection
    IConnection* icon = AddressMap.FindByAddr(addr);
    // Note the connection reference count was incremented by FindByAddr().
    if (icon)
    {
        Connection* connection = reinterpret_cast<Connection*>(icon);
        connection->OnUDPDatagram(receiveUsec, addr, datagram);
        return;
    }

    Connection* connection = nullptr;
    Result result;
    uint64_t cookieKey = 0;
    FloodReaction reaction;

    // If configuration disallows connections from being accepted:
    if (Deps.IdMap->GetConnectionCount() >= SocketConfig->MaximumClients)
    {
        if (SocketConfig->MaximumClients == 0) {
            Deps.Logger->Warning("Rejecting connection: Configuration disallows connections (MaximumClients=0 or OnIncomingConnection=0)");
        }
        else {
            Deps.Logger->Warning("Rejecting connection: Server full");
        }
        goto OnReject;
    }

    // Check for a connection flood
    reaction = FloodDetect.OnConnectionRequest(addr, encryptionKey, cookieKey);

    // If a flood was detected:
    if (reaction == FloodReaction::Deny)
    {
        Deps.Logger->Warning("Rejecting connection: Flood detection");
        goto OnReject;
    }

    // If a cookie is required:
    if (reaction == FloodReaction::Cookie)
    {
        Deps.Logger->Warning("Client cookie mismatch.  Replying with expected cookie");

        datagram.Free();

        // Send connection rejection
        DatagramBuffer cookie;
        cookie.AllocatorForFree = Deps.ReadBufferAllocator;
        cookie.Bytes = protocol::kS2CCookieResponseBytes;
        cookie.Data = Deps.ReadBufferAllocator->Allocate(protocol::kS2CCookieResponseBytes);
        cookie.FreePtr = cookie.Data;

        if (cookie.Data)
        {
            protocol::GenerateS2CCookieResponse(cookieKey, cookie.Data);
            Send(addr, cookie, Deps.SocketRefCount);
        }

        return;
    } // End if cookie is needed

    // Allocate a connection object
    connection = new(std::nothrow) Connection;
    if (!connection)
    {
        TONK_DEBUG_BREAK();
        goto OnReject;
    }

    // Initialize connection object
    // Buffer ownership is passed to connection green thread here
    result = connection->InitializeIncomingConnection({
        Deps.SocketRefCount,
        this,
        &AddressMap,
        Deps.IdMap,
        Deps.P2PKeyMap,
        Deps.MappedPortPtr->get(),
        Deps.Context,
        &FloodDetect, // We will need to call OnDisconnect() for this one
        addr, // Disconnect addr key
        Deps.NATPool
    },
        SocketConfig,
        encryptionKey,
        addr,
        receiveUsec,
        datagram);
    if (result.IsFail())
    {
        TONK_DEBUG_BREAK();
        goto OnReject;
    }

    /*
        This is the right time to insert the connection into the address map.
        If we do it from the Connection::AsioEventStrand, then we may create
        a second Connection object for the same connection before that event
        is handled, because there may be multiple UDP datagrams on the wire
        from that client.

        Because FindByAddr() failed to find this address above,
        and because this is the main way that addresses are inserted into the
        map, it is unlikely that this call to InsertAddress() will fail.

        However it can still fail if the same source address is used for a
        datagram with the ID flag set, causing a client with that ID to be
        assigned to this source address between the top and bottom of this
        function as a race condition.

        This would cause a connection object that's disconnected from the
        address map to be dangling, which would eventually get resolved by
        (1) ID flag causing proper source address to be used or
        (2) Data timeout, causing disconnection.
    */
    if (!AddressMap.InsertAddress(addr, connection)) {
        Deps.Logger->Warning("Race lost: Duplicate address inserted into map (",
            addr.address().to_string(), ":", addr.port(), ")");
    }

    return;

OnReject:
    datagram.Free();

    // Send connection rejection
    DatagramBuffer reject;
    reject.AllocatorForFree = Deps.ReadBufferAllocator;
    reject.Bytes = protocol::kDisconnectBytes;
    reject.Data = Deps.ReadBufferAllocator->Allocate(protocol::kDisconnectBytes);
    reject.FreePtr = reject.Data;

    if (reject.Data)
    {
        protocol::GenerateDisconnect(encryptionKey, reject.Data);
        Send(addr, reject, Deps.SocketRefCount);
    }
}

void UDPSocket::Shutdown()
{
    TONK_DEBUG_ASSERT(OutstandingSends == 0);
    TONK_DEBUG_ASSERT(OutstandingRecvs == 0);

    Socket = nullptr;
    AsioConnectStrand = nullptr;
}


} // namespace tonk
