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

#include "TonkineseInterfaces.h"
#include "TonkineseIncoming.h"
#include "TonkineseOutgoing.h"
#include "TimeSync.h"
#include "TonkineseMaps.h"
#include "TonkineseNAT.h"
#include "TonkineseFlood.h"

/**
    Codepath for client making a connection:

    (1) tonk_connect() calls ApplicationSession::tonk_connect
    (2) new Connection : Create a Connection object
    (3) Connection::InitializeOutgoingConnection() calls asio async_resolve
    (4) Connection::onResolve() calls startConnectingToNextAddress()
    (5) startConnectingToNextAddress() calls:
        HostnameResolverState::GetNextServerAddress()
        SessionOutgoing::SetPeerUDPAddress()
        SessionOutgoing::StartHandshakeC2SConnect()
        And it unmaps the old server address to this connection,
        and it maps the new server address to this connection.

    And connection requests start flying.

    (6) UDPSocket::OnUDPDatagram() receives a valid datagram from the server
        Note that any correctly decoding datagram indicates success
    (7) ConnectionMaps::FindByAddr() looks up the Connection object
    (8) Connection::OnUDPDatagram() calls SessionIncoming::ProcessDatagram()
    (9) SessionIncoming::ProcessDatagram() calls Connection::OnAuthenticatedDatagram()

    OnAuthenticatedDatagram() recognizes that the Connection is now established.

    After a connection is established, the Connection is symmetrical peer2peer.
*/

/**
    Codepath for server receiving a connection:

    (1) Server receives a datagram in UDPSocket::OnUDPDatagram() from client
    (2) OnUDPDatagram() processes the request and calls onConnectRequest()
        onConnectRequest() is called from the AsioConnectStrand asio strand.
    (3) new Connection : Create a Connection object
    (4) Connection::InitializeIncomingConnection() calls completeIncomingConnection()
        completeIncomingConnection() is called from the AsioEventStrand asio strand.
    (5) completeIncomingConnection() sets up some state:
        ConnectionMaps::InsertAndAssignId() assigns a connection id and maps it
        queueAddressAndIdChangeMessage() informs peer of its connection id and source address
    (6) Config->OnIncomingConnection() is called.
        If it returns nullptr, then the connection is aborted.
        Otherwise pending data is flushed and timers start ticking.

    After a connection is established, the Connection is symmetrical peer2peer.
*/

/**
    Client-Server Connection Lifecycle:

    (1) Unrecognized client sends connection request:

        Unknown UDP/IP source IPv4/IPV6 address + UDP port
        Unassigned ID

        Received: UDP datagram with a Connection Footer including the client's encryption key.
        Work item to react to the datagram is put in the Pre-Connected green thread.

    (2) In Pre-Connected green thread:

        All connection requests are serialized briefly into this green thread to avoid
        delaying datagram processing.

        (a) Check if the source address is now known and forward so this data is not lost.
        (b) Check if too many clients are already connected.
        (c) Allocate a Connection object.  Assign unique ID.
        (d) Pass task to complete connection to the Connection's green thread.
        (e) Insert Connection into source address map and ID map.

        After (e) all new datagrams for this client will be passed to it.
        The Pre-Connected green thread may contain further packets from the
        client that can be safely received out of order in step (a) above.

    (3) In Connection green thread:

        This green thread serializes all the Tonk tick, disconnect, connect
        events for this Connection object.  The connection event is always
        the first event processed by this green thread.

        (a) Complete initialization of the Connection object.  Set peer address.
        (b) Invoke the application OnConnect() callback.  Get UserContext ptr.

        The app callback is able to send packets at this point or do any other call.
        After this point timer tick events and received datagrams may be processed
        in the Connection's green thread.

    (4) When disconnection occurs for any reason:

        An atomic flag is set to indicate disconnection has occurred, and the
        reason for disconnection is latched in.

        The timer tick event will stop firing for the application.

        The OnClose() callback will fire from the Connection's green thread,
        containing the reason for disconnection.

        The Connection object will be removed from the source address map
        and the ID map so that it no longer receives further datagrams.

        A DiscoFlush lambda task is posted to the Connection's green thread,
        and when it executes it sets a flag, indicating that all Tonk green
        thread tasks are flushed except for the timer tick.

        At some point the application code will call tonk_close() which sets
        an atomic flag in the Connection object indicating that all application
        references are released.

        Finally the timer tick will notice that the DiscoFlush flag and the
        tonk_close() flag are set.  It will Shutdown() the Connection
        object and then free it.


    The server's UDP socket receives a datagram to start a new connection:

    -> Get UDP datagram:
        + Recognized Source Address (found in src map)
        - Ignored ID Footer
        - Ignored Connection Footer

        -> Pass datagram to the Connection's green thread.

    -> Get UDP datagram:
        - Unrecognized Source Address (not in src map)
        + Recognized ID Footer
        - Ignored Connection Footer

        -> Pass datagram to the Connection's green thread.

        After the datagram is authenticated it will update the
        peer address and the source address map.

    -> Get UDP datagram:
        - Unrecognized Source Address (not in src map)
        - No/Unrecognized ID Footer
        + With Connection Footer

        A connection request is being made.

        -> Pass datagram to the Pre-Connected green thread.

        Connection follows the flow described above.

    -> Get UDP datagram:
        - Unrecognized Source Address (not in src map)
        - No/Unrecognized ID Footer
        - No Connection Footer

        Unrecognized data may be from an existing client but
        we cannot identify them without an ID footer, so the
        server will reply with a "Client Unrecognized" datagram.

        -> Send "Client Unrecognized" datagram from Pre-Connected green thread.
*/

namespace tonk {


//------------------------------------------------------------------------------
// Constants

/// Encryption can be disabled just for outgoing connections to enable traces
/// for games in production with other live players who are using encryption
//#define TONK_DISABLE_OUTGOING_ENCRYPTION


//------------------------------------------------------------------------------
// DNS Hostname Resolution

struct HostnameResolverState
{
    /// Server UDP port
    uint16_t ServerUDPPort = 0;

    /// Asio resolver object
    std::unique_ptr<asio::ip::tcp::resolver> Resolver;

    /// Resolved server addresses
    std::vector<asio::ip::address> ResolvedIPAddresses;

    /// Current connection address being used from the array
    unsigned ConnectAddrIndex = 0;

    /// Last address to check before giving up, since we start from
    /// a random address in the list
    unsigned ConnectRequestsRemaining = 0;


    /// Initialize the resolved address list from the asio resolver results
    void InitializeResolvedAddressList(
        const asio::ip::tcp::resolver::results_type& results);

    /// Get next server address to connect to
    Result GetNextServerAddress(asio::ip::address& addr);
};


//------------------------------------------------------------------------------
// NATHolePuncher

/**
    Tonk NAT Hole Puncher

    This is some extra state that is allocated just for P2P connections for the
    Connection::HolePuncher member variable.

    The algorithm is described in HolePunch.README.txt.
*/

class NATHolePuncher
{
public:
    NATHolePuncher(const protocol::P2PConnectParams& startParams)
        : StartParams(startParams)
    {
    }

    /// Start parameters
    protocol::P2PConnectParams StartParams;

    /// Connection Id for the rendezvous server
    /// Used to check if the server connection is dead
    uint32_t RendezvousServerConnectionId = 0;

    /// Seen a probe packet?
    std::atomic<bool> SeenProbe = ATOMIC_VAR_INIT(false);
    std::atomic<uint32_t> SentProbes = ATOMIC_VAR_INIT(0);

    /// Timer for the protocol phases
    std::unique_ptr<asio::steady_timer> RoundTicker;

    /// Protocol round
    unsigned ProtocolRound = 0;

    /// Probe datagram
    /// Optimization: Avoid datagram allocations by preallocating memory
    uint8_t ProbeData[protocol::kNATProbeBytes];
};


//------------------------------------------------------------------------------
// Connection

class Connection
    : public IConnection // It is important for IConnection to come first
    , public IIncomingHandler
{
public:
    Connection();
    virtual ~Connection();

    TonkConnection GetTonkConnection()
    {
        return reinterpret_cast<TonkConnection>(this);
    }

    struct Dependencies
    {
        RefCounter* SocketRefCount;
        IUDPSender* UDPSender;
        ConnectionAddrMap* AddressMap;
        ConnectionIdMap* IdMap;
        P2PConnectionKeyMap* P2PKeyMap;
        gateway::MappedPortLifetime* MappedPort;
        std::shared_ptr<asio::io_context> Context;

        // This is set optionally if OnDisconnect() needs to be called on the
        // provided FloodDetector object after disconnect completes.
        FloodDetector* DisconnectHandler;

        // Provide this key
        UDPAddress DisconnectAddrKey;

        INATPortPool* NATPool;
    };

    Result InitializeIncomingConnection(
        const Dependencies& deps,
        const TonkSocketConfig* socketConfig,
        uint64_t encryptionKey,
        const UDPAddress& peerAddress,
        uint64_t receiveUsec,
        DatagramBuffer datagram);

    Result InitializeOutgoingConnection(
        const Dependencies& deps,
        const TonkSocketConfig* socketConfig,
        const TonkConnectionConfig& connectionConfig,
        const char* serverHostname,
        uint16_t serverPort);

    Result InitializeP2PConnection(
        const Dependencies& deps,
        const TonkSocketConfig* socketConfig,
        uint32_t rendezvousServerConnectionId,
        const protocol::P2PConnectParams& startParams);

    void Shutdown();

    TONK_FORCE_INLINE void SetUDPSender(IUDPSender* sender)
    {
        Deps.UDPSender = sender;
        Outgoing.SetUDPSender(sender);
    }


    /// tonk.h API
    void tonk_status(TonkStatus& status);
    void tonk_status_ex(TonkStatusEx& statusEx);
    Result tonk_send(unsigned channel, const uint8_t* data, uint64_t bytes);
    void tonk_flush();
    void tonk_close();
    void tonk_free();
    uint16_t tonk_to_remote_time_16(uint64_t localUsec);
    uint64_t tonk_from_local_time_16(uint16_t networkTimestamp16);
    uint32_t tonk_to_remote_time_23(uint64_t localUsec);
    uint64_t tonk_from_local_time_23(uint32_t networkTimestamp23);
    Result tonk_p2p_connect(Connection* bob);

    /// Handle an ICMP error code.
    /// This function assumes that the caller has incremented SelfRefCount.
    /// It will dispatch a handler for the event in the AsioEventStrand.
    /// When the handler is complete it will decrement the SelfRefCount.
    void OnICMPError(const asio::error_code& error);

    /// Handle a UDP datagram.
    /// This function assumes that the caller has incremented SelfRefCount.
    /// It will dispatch a handler for the event in the AsioEventStrand.
    /// When the handler is complete it will decrement the SelfRefCount.
    void OnUDPDatagram(
        uint64_t receiveUsec,
        const UDPAddress& addr,
        DatagramBuffer datagram);

    /// Get the encryption key for this connection
    TONK_FORCE_INLINE uint64_t GetEncryptionKey() const
    {
        return EncryptionKey;
    }

    /// Handle NAT probe.
    /// This function assumes that the caller has incremented SelfRefCount.
    /// It will dispatch a handler for the event in the AsioEventStrand.
    /// When the handler is complete it will decrement the SelfRefCount.
    void OnNATProbe(
        const UDPAddress& addr,
        uint32_t key,
        ConnectionAddrMap* addrMap,
        IUDPSender* sender);

    /// Handle P2P connection request.
    /// This function assumes that the caller has incremented SelfRefCount.
    /// It will dispatch a handler for the event in the AsioEventStrand.
    /// When the handler is complete it will decrement the SelfRefCount.
    void OnP2PConnection(
        uint64_t receiveUsec,
        const UDPAddress& addr,
        DatagramBuffer datagram,
        uint64_t handshakeKey, // Value from handshake footer
        ConnectionAddrMap* addrMap,
        IUDPSender* sender);

    /// Peer address changes are only allowed if the peer has sent a valid
    /// source address change handshake in the footer.
    std::atomic<bool> PeerAddressChangesAllowed = ATOMIC_VAR_INIT(false);

protected:
    Dependencies Deps;

    /// Configuration from application
    const TonkSocketConfig* SocketConfig = nullptr;
    TonkConnectionConfig ConnectionConfig;

    /// Subsystems
    logger::Channel Logger;
    SessionOutgoing Outgoing;
    SessionIncoming Incoming;
    TimeSynchronizer TimeSync;
    SenderBandwidthControl SenderControl;
    ReceiverBandwidthControl ReceiverControl;

    /// Hostname resolver
    std::unique_ptr<HostnameResolverState> ResolverState;

    /// Peer2Peer NAT Traversal
    std::unique_ptr<NATHolePuncher> HolePuncher;

    /// Are we in an established connection?
    std::atomic<bool> ConnectionEstablished = ATOMIC_VAR_INIT(false);

    /// Timer: Did we not receive data from peer for too long?
    Timer NoDataTimer;

    /// Timer: Is it time to send another flow control update?
    /// This is also used for re-sending the connection request to server
    Timer TimeSyncTimer;

    /// Timer: Is it time to send another acknowledgement?
    Timer AcknowledgementTimer;

    /// Timer: Application OnTick() callback
    Timer AppTimer;

    /// Tick timer
    std::unique_ptr<asio::steady_timer> Ticker;

    /// Strand that serializes read, write, and tick events
    std::unique_ptr<asio::io_context::strand> AsioEventStrand;

    /// Lock protecting SelfAddress and some other members
    mutable Lock PublicAPILock;

    /// PublicAPILock: Self address provided by ControlStream_S2C_ClientAddress
    UDPAddress SelfAddress;

    /// PublicAPILock: Cached status to speed up tonk_status()
    TonkAddress CachedLocalAddress;

    /// PublicAPILock: Last receiver stats seen from peer
    ReceiverStatistics CachedReceiverStats;

    /// Shared encryption key established by connection initiator
    uint64_t EncryptionKey = 0;

    /// Connection id assigned to remote host
    /// This is used on Shutdown() to release the Id to other clients
    uint32_t LocallyAssignedIdForRemoteHost = TONK_INVALID_ID;

    /// Connection id assigned to local host
    uint32_t RemoteAssignedIdForLocalHost = TONK_INVALID_ID;

    /// Port we were able to map, or 0 if none
    uint16_t LocalNATMapExternalPort = 0;

    /// Port reported by ControlChannel_C2S_NATExternalPort, or 0 if none
    uint16_t RemoteNATMapExternalPort = 0;

    /// Keep track of the last Id&Address message to avoid sending duplicates
    uint8_t LastIdAndAddressSent[protocol::kMaxClientIdAndAddressBytes];

    /// Allow app OnClose() callback to fire?
    bool UnlockedAppOnCloseCallback = false;

    /// Has OnClose() been reported yet?
    bool HasReportedClose = false;

    /// Have we removed ID, addr, P2Pkey from maps yet?
    bool RemovedFromMaps = false;

    TONK_IF_DEBUG(std::atomic<bool> ClientIdAndAddressIsSet = ATOMIC_VAR_INIT(false);)

#ifdef TONK_DETAILED_STATS
    /// Detailed statistics
    DetailStatsMemory Deets;
#endif // TONK_DETAILED_STATS

    /// Last timer tick in microseconds.  Used for decompressing timestamps
    std::atomic<uint64_t> LastTickUsec = ATOMIC_VAR_INIT(0);

    /// Get the current time and update LastTickUsec
    TONK_FORCE_INLINE uint64_t GetTicksUsec()
    {
        const uint64_t nowUsec = siamese::GetTimeUsec();
        LastTickUsec = nowUsec;
        return nowUsec;
    }


    /// Initialize subsystems
    Result initSubsystems();

    /// Initialize timer state
    void resetTimers(uint32_t noDataTimeoutUsec, bool connectionEstablished);

    /// Complete incoming connection
    Result completeIncomingConnection(const UDPAddress& peerUDPAddress);

    /// Set the peer UDP address
    void setPeerAddress(const UDPAddress& peerUDPAddress);

    /// Set the peer UDP address and UDP source socket
    void setPeerAddressAndSource(
        ConnectionAddrMap* addrMap,
        IUDPSender* sender,
        const UDPAddress& peerUDPAddress);

    /// Get the server-provided public IP/port of the client
    UDPAddress getSelfAddress() const
    {
        Locker locker(PublicAPILock);
        return SelfAddress;
    }

    /// Queue up a control channel message to client updating its address/id
    void queueAddressAndIdChangeMessage(const UDPAddress& peerUDPAddress);

    /// IIncomingHandler:
    HandshakeResult OnUnauthenticatedHandshake(const uint8_t* data, const unsigned bytes) override;
    void OnAuthenticatedDatagram(const UDPAddress& addr, uint64_t receiveUsec) override;
    Result OnControlChannel(const uint8_t* data, unsigned bytes) override;
    Result OnTimeSync(const uint8_t* data, unsigned bytes) override;

    /// Called when worker timer fails
    void onTimerError(const asio::error_code& error);

    enum class TimerTickResult
    {
        TickAgain,
        StopTicking
    };

    /// Called when worker timer ticks
    TimerTickResult onTimerTick();

    /// Post next timer tick
    void postNextTimer();

    /// Set self address
    void setSelfAddress(uint32_t id, const UDPAddress& addr);

    /// Handle timer tick while disconnected
    TimerTickResult onDiscoTimerTick();

    /// Completion callback for async_resolve()
    void onResolve(const asio::error_code& error, const asio::ip::tcp::resolver::results_type& results);

    /// Start connecting to the next server address that was resolved
    void startConnectingToNextAddress();

    /// Report close
    void reportClose();

    /// Notify peer of our new external NAT mapped port
    void sendNATMapPortToPeer(uint16_t port);

    /// Push a disconnect message
    void sendDisconnectMessageToPeer();

    /// Start P2P connection
    void onP2PStartConnect(const protocol::P2PConnectParams& params);

    /// Called to send a flurry of P2P probes
    /// Note this is not called on the AsioEventStrand
    void onNATProbeRound(const asio::error_code& error);

    /// Post next onNATProbeRound() timer callback
    void startNextNATProbeRoundTimer();

    /// Handle decode failures
    void onDecodeFailure(const Result& error);

    /// Remove this object from any maps during shutdown
    void removeFromMaps();

    /// Handle when ack message timer fires
    void onAckSendTimer(uint64_t nowUsec);

    /// Handle when time sync message timer fires
    void onTimeSyncSendTimer();
};


} // namespace tonk
