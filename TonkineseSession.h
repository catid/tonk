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

#pragma once

#include "TonkineseUDP.h"
#include "TonkineseConnection.h"
#include "TonkineseMaps.h"

/**
    Architecture:

    (1) TonkSocket - This is the app object that is created when the app calls
    the tonk_socket_create() function, and is destroyed when the app calls the
    tonk_socket_destroy() function.

    TonkSocket tonk_socket_destroy() will block until all references to it are
    released.  The only objects that have references to TonkSocket are UDPSocket
    objects, which release their references to TonkSocket when they go out of
    scope.

    Across the TonkSocket there is a consistent map from peer IDs to Connection
    objects, so that no two Connection objects share the same ID.  Furthermore
    the TonkSocket has a map from Peer2Peer keys to Peer2Peer connections,
    which are in progress.

    (2) UDPSocket - This represents one UDP socket.  When the socket is created
    the TonkSocketConfig will specify initial ports to open up.  Additionally
    ports will be opened for peer2peer connections to facilitate NAT traversal.

    The UDPSocket will have a map from source addresses to Connection objects.

    (3) Connection - This represents one remote peer that the application has
    approved to connect to this computer.

    The application associates context with a Connection via the
    OnIncomingConnection(), OnOutgoingConnection(), and OnP2PConnectionStart()
    callbacks it provides in the TonkSocketConfig.

    Connections are initiated by:
    + The local machine, in which case the connection is a "Client."
    + The remote machine, in which case the connection is a "Server."
    + A third rendezvous server, faciliating a "Peer2Peer" (P2P) connection.

    Connections can be addressed by their their source address and destination
    port, unique ID (in case their source IP address or port has changed), or
    for peer2peer connections by the key provided by the rendezvous server.
    Received UDP datagrams are passed to Connection objects based on one of
    these three criterion.  The UDP datagrams may still be rejected if the
    datagram does decode successfully due to e.g. truncation or bad checksum.

    Each Connection has one UDPSocket that it uses for outgoing data, but it may
    receive data from multiple incoming UDPSockets in parallel.  All incoming
    data is serialized into a green thread for that Connection object and
    processed one datagram at a time.
*/

namespace tonk {


//------------------------------------------------------------------------------
// ApplicationSession

class ApplicationSession
    : public INATPortPool
{
public:
    ApplicationSession();
    virtual ~ApplicationSession();

    Result Initialize(const TonkSocketConfig& config);
    void Shutdown();


    /// API:
    Result tonk_connect(
        TonkConnectionConfig config,
        const char* hostname,
        uint16_t serverPort,
        TonkConnection* connectionOut);
    Result tonk_advertise(
        const char* ipString,
        uint16_t port,
        const void* data,
        uint32_t bytes);
    Result tonk_inject(
        uint16_t sourcePort,
        const uint8_t* data,
        uint32_t bytes
    );
    void tonk_socket_destroy();

protected:
    /// Logging channel
    logger::Channel Logger;

    /// Asio context
    std::shared_ptr<asio::io_context> Context;

    /// Allocator for read buffers for UDP sockets
    BufferAllocator ReadBufferAllocator;

    /// There is one main socket that either bound to a given user port or 0 for
    /// a client-only ApplicationSession.
    UDPSocket MainSocket;

    /// For Peer2Peer NAT traversal we open up more..
    UDPSocket ProbeListenerSockets[protocol::kNATMaxPorts];

    /// Config provided via Initialize()
    TonkSocketConfig Config;

    /// Map from Connection Id -> Connection object
    ConnectionIdMap IdMap;

    /// Map from P2P Connection Key -> Connection object
    P2PConnectionKeyMap P2PKeyMap;

    /// Should worker thread be terminated?
    std::atomic<bool> Terminated = ATOMIC_VAR_INIT(false);

    /// Worker thread
    std::vector< std::unique_ptr<std::thread> > Threads;

    /// Strand that serializes connection requests
    std::shared_ptr<asio::io_context::strand> AsioConnectStrand;

    /// Tick timer : This is only used to prevent asio from using 100% CPU
    std::unique_ptr<asio::steady_timer> Ticker;

    /// Self-reference count.
    /// This is used to wait for all Connection objects to die before shutting down
    RefCounter SelfRefCount;

    /// Are the P2P sockets initialized?
    bool P2PSocketsInitialized = false;

    /// Object that keeps a port mapping alive
    std::shared_ptr<gateway::MappedPortLifetime> MappedPort;


    /// INatPortPool:
    void OnP2PStartConnect(
        uint16_t myRendezvousVisibleSourcePort,
        uint32_t rendezvousServerConnectionId,
        const protocol::P2PConnectParams& startParams
    ) override;
    void SendNATProbe(
        unsigned socketIndex,
        uint8_t* probeData,
        const UDPAddress& destAddress,
        RefCounter* refCounter
    ) override;

    /// Post next timer tick
    void postNextTimer();

    /// Worker loop that runs the Asio context
    void workerLoop();

    /// Set Config member
    void setConfig(const TonkSocketConfig& config);

    /// Completion of OnP2PStartConnect() in AsioConnectStrand
    void onP2PStartConnect(
        uint16_t myRendezvousVisibleSourcePort,
        uint32_t rendezvousServerConnectionId,
        const protocol::P2PConnectParams& startParams);

    /// Initialize P2P sockets
    void initializeP2PSockets(const protocol::P2PConnectParams& startParams);
};


} // namespace tonk
