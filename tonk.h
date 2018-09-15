/** \file
    \brief Tonk Main C API Header
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

/*
    TODO:
    File transfer C# SDK, and C# update for API changes
    Unity example
    Port to Android/iOS
*/

#ifndef CAT_TONKINESE_H
#define CAT_TONKINESE_H

/** \mainpage
    Tonk : Reliable UDP (rUDP) Network Library in C++

    Tonk.h provides a simple C API for a powerful, multithreaded network engine.
    The Tonk network engine transports message data over the Internet using a
    custom protocol over UDP/IP sockets.  Like TCP, Tonk delivers data reliably
    and in-order.  The main improvement over TCP is that data sent with Tonk
    arrives with much less latency due to congestion control and head-of-line
    blocking issues.

    Tonk is experimental: It's only been unit tested and hasn't been used for
    any major projects yet.  There are probably bugs to fix.  It currently only
    builds for Linux and Windows targets right now.

    Tonk puts each connection into its own thread, so all processing related to
    each connection happens serially.  The application is responsible for
    handling thread safety on the server side between multiple connections.

    Tonk implements a new type of jitter-robust time synchronization protocol
    over the Internet that is more suitable for mobile networks than NTP/PTP.
    Tonk enables applications to compress timestamps down to 2 or 3 bytes.

    Tonk implements a novel HARQ transport based on SiameseFEC, meaning that a
    new type of forward error correction is used to protect data and avoid
    head-of-line blocking that causes latency spikes with TCP sockets.
    SiameseFEC is the world's first infinite-window erasure code, suitable for
    protecting reliable in-order data like a TCP stream, but less suitable for
    audio or video data protection, which does not need an infinite window.

    Tonk implements a new type of congestion control strategy that attempts to
    guarantee low latency for real-time data while full-speed file transfers
    are being performed in the background.  Tonk supports three levels of
    message prioritization, 12 different parallel data streams, unordered
    delivery, and unmetered unreliable delivery.

    Tonk implements several types of fully automated Peer2Peer NAT traversal to
    allow mobile applications to peer with the aid of a rendezvous server.
    It also incorporates WLANOptimizer to reduce latency on some Windows PCs.

    Tonk's philosophy is to trade extra CPU work and extra memory for faster
    data delivery, so it may not be suitable for your application.  For example
    it uses streaming data compression with Zstd to compress packets so there is
    less to send, but this does take more CPU resources and requires memory.
    It also implements the fastest known erasure codes for this application and
    it has to send recovery packets in addition to normal data, which takes more
    CPU/network resources but will avoid head-of-line blocking and deliver the
    data faster.  It uses a multithreaded design, which requires extra complexity
    and overhead but means that multiple connections can be processed in parallel
    to reduce the overall latency.  The peer2peer design allows the application
    to cut out a network hop to the server and this requires a lot of additional
    complexity, but the result is significantly lower latency.  At the same time
    to offset all of these additional costs, a lot of engineering effort went
    into choosing/designing/tuning the best algorithms to minimize overhead.
    TL;DR Tonk is all about reducing latency by taking advantage of faster CPUs.

    Appropriate applications:
        + Data rate limit of 20,000,000 bytes per second, per connection
        + Low-latency messaging
        + Mobile file (e.g. video/image) upload
        + Lossless video/audio streaming
        + Real-time multiplayer gaming
        + Mobile VPN data accelerator
        + Peer2Peer mobile file transfer, chat, or data streaming

    Inappropriate applications:
        - High rate file transfer > 20 MB/s:
            Forward error correction does not work at these higher rates,
            so using a custom UDP-based congestion control works better
        - Lossy real-time audio/video conference or chat:
            SiameseFEC has an infinite window, so using Cauchy Caterpillar is
            more appropriate: https://github.com/catid/CauchyCaterpillar
        - High security applications:
            Tonk does not implement any PKI or encryption

    Message delivery:
        + Unordered unreliable message delivery
        + Unordered reliable message delivery
        + Multiple reliable in-order channels (6)
        + Multiple reliable in-order low-priority channels (6)
        + Breaks up large messages into pieces that fit into datagrams
        + Detects and rejects duplicate packets (incl. unreliable)
        + File transfer API for sending large buffers or files from disk
          (See tonk_file_transfer.h)

    Latency features:
        + 0-RTT connections: Server accepts data in first packet
        + Lower latency than TCP congestion control (10 milliseconds)
        + Enables file transfer coexisting with low latency applications
        + Message prioritization delivers the most important data first
        + Forward Error Correction (FEC) to reduce median latency
        + Ordered reliable data is compressed with Zstd for speedier delivery

    Extras:
        + Jitter-robust time synchronization for mobile applications
        + NAT hole punching using UPnP for Peer2Peer connections
        + NAT hole punching using STUN + NATBLASTER (with rendezvous server)
        + Automatically opens up any needed ports in the Windows firewall
        + Detects and rejects data tampering on the wire
        + SYN-cookies enabled during connection floods to mitigate DoS attacks
        + Fast obfuscation (encryption without security guarantees)
        + Reduced idle traffic using ack-acks

    Not provided (Non-goals):
        - Data security (Requirements are too application-specific)
        - TCP fallback if UDP-based handshakes fail (Application can do this)
        - Unreliable-but-ordered delivery (Can be done using Unreliable channel)
        - MTU detection (Assumes 1488 byte frames or larger)

    Runtime flag options:
        + UPnP port forwarding can be turned on
        + FEC usage is minimal by default but can be turned on
        + Zstd data compression can be turned off

    Tonk uses the Siamese library for its forward error correction (FEC),
        selective acknowledgements (SACK), and jitter buffer.
    Tonk uses the Asio library for portable networking and strands.
    Tonk uses the Cymric library for secure random number generation.
        Cymric uses BLAKE2b and Chacha.
    Tonk uses the Zstd library (modified) for fast in-order packet compression.
    Tonk uses the t1ha library for fast hashing.
*/

/// Library version
#define TONK_VERSION 8

// Tweak if the functions are exported or statically linked
//#define TONK_DLL /* Defined when building/linking as DLL */
//#define TONK_BUILDING /* Defined by the library makefile */

#if defined(TONK_BUILDING)
# if defined(TONK_DLL) && defined(_WIN32)
    #define TONK_EXPORT __declspec(dllexport)
# else
    #define TONK_EXPORT
# endif
#else
# if defined(TONK_DLL) && defined(_WIN32)
    #define TONK_EXPORT __declspec(dllimport)
# else
    #define TONK_EXPORT extern
# endif
#endif

#include <stdint.h>


#ifdef __cplusplus
#define TONK_CPP(x) x
extern "C" {
#else
#define TONK_CPP(x)
#endif


//------------------------------------------------------------------------------
// Shared Constants/Datatypes

/// These are the result codes that can be returned from the tonk_*() functions
typedef enum TonkResult_t
{
    /// Success code
    Tonk_Success            = 0,

    /// A function parameter was invalid
    Tonk_InvalidInput       = 1,

    /// An error occurred during the request
    Tonk_Error              = 2,

    /// Hostname lookup failed
    Tonk_HostnameLookup     = 3,

    /// Provided TonkChannel number is invalid.
    /// Accepted input is TonkChannel_Unreliable,
    /// TonkChannel_Reliable0 + N, and TonkChannel_LowPri0 + N,
    /// where N = [0 ... TonkChannel_Last]
    Tonk_InvalidChannel     = 4,

    /// Message was too large.
    /// Reliable messages must be less than 64000 TONK_MAX_RELIABLE_BYTES bytes.
    /// Unreliable messages must not exceed 1280 TONK_MAX_UNRELIABLE_BYTES bytes
    Tonk_MessageTooLarge    = 5,

    /// Network library (asio) returned a failure code
    Tonk_NetworkFailed      = 6,

    /// PRNG library (cymric) returned a failure code
    Tonk_PRNGFailed         = 7,

    /// FEC library (siamese) returned a failure code
    Tonk_FECFailed          = 8,

    /// Authenticated peer network data was invalid
    Tonk_InvalidData        = 9,

    /// Connection rejected by remote host
    Tonk_ConnectionRejected = 10,

    /// Connection ignored by remote host
    Tonk_ConnectionTimeout  = 11,

    /// The application requested a disconnect
    Tonk_AppRequest         = 12,

    /// The remote application requested a disconnect
    Tonk_RemoteRequest      = 13,

    /// The remote application stopped communicating
    Tonk_RemoteTimeout      = 14,

    /// Received network data could not be authenticated
    Tonk_BogonData          = 15,

    /// Out of memory
    Tonk_OOM                = 16,

    /// Tonk dynamic library not found on disk
    Tonk_DLL_Not_Found      = 17,

    TonkResult_Count, /* for asserts */
    TonkResult_Padding = 0x7fffffff /* int32_t padding */
} TonkResult;

/// Evaluates true if the result is a success code
#define TONK_GOOD(result) (Tonk_Success == (result))

/// Evaluates true if the result is a failure code
#define TONK_FAILED(result) (Tonk_Success != (result))

/// Convert a TonkResult result code into a string
TONK_EXPORT const char* tonk_result_to_string(
    TonkResult result  ///< [in] TonkResult code to convert
);

/// Maximum number of bytes to return, including string null terminator
#define TONK_ERROR_JSON_MAX_BYTES 1024

/// JSON string representation of an error result
typedef struct TonkJson_t
{
    /// Guaranteed to be a null-terminated UTF-8 string
    /// Warning: Data may be invalid Json if truncated
    char JsonString[TONK_ERROR_JSON_MAX_BYTES];
} TonkJson;

/// The TonkStatus::Flags bitfield is a combination of these flags
typedef enum TonkFlags_t
{
    /// When set: The connection initated from the local socket
    TonkFlag_Initiated      = 0x10000000,

    /// When set: Time synchronization is ready
    TonkFlag_TimeSync       = 0x00000001,

    /// When set: UPnP/NATPMP port mapping on the router was successful
    TonkFlag_NATMap_Local   = 0x00000100,

    /// When set: UPnP/NATPMP port mapping was successful for the peer
    TonkFlag_NATMap_Remote  = 0x00000200,

    /// When set: Connection attempt is in progress
    TonkFlag_Connecting     = 0x01000000,

    /// When set: Connected
    TonkFlag_Connected      = 0x02000000,

    /// When set: Disconnected
    TonkFlag_Disconnected   = 0x04000000,
} TonkFlags;

/// Number of bytes in the NetworkString field (including null '\0' terminator)
#define TONK_IP_STRING_BYTES 40

/// Maximum number of characters in the NetworkString field
#define TONK_IP_STRING_MAX_CHARS (TONK_IP_STRING_BYTES - 1)

/// This data-structure is part of TonkStatus below
typedef struct TonkAddress_t
{
    /// IPv4 or IPv6 human-readable IP address.  Guaranteed to be null-terminated
    char NetworkString[TONK_IP_STRING_BYTES];

    /// UDP Port
    uint16_t UDPPort;
} TonkAddress;

/// Before the ID assignment message is received, tonk_status() returns this ID
#define TONK_INVALID_ID 0x7fffffff

/// This is a data-structure returned by tonk_status().
/// This structure details the current state of the TonkConnection
typedef struct TonkStatus_t
{
    /// Combination of TonkFlags
    uint32_t Flags;

    /// This is the ID for the remote host assigned by the local host
    uint32_t LocallyAssignedIdForRemoteHost;

    /// Requested app send rate in bytes per second (BPS).
    /// Tonk will probe available bandwidth at higher BPS using redundant data
    /// and when it detects a higher send rate is possible, AppBPS increases.
    uint32_t AppBPS;

    /// Estimated time for new Reliable messages to be sent.
    /// This includes the Unreliable/Unordered messages that will go out first
    uint32_t ReliableQueueMsec; ///< milliseconds

    /// Estimated time for new LowPri messages to be sent.
    /// This includes the Reliable messages that will go out before these.
    /// Apps can use this to keep the network fed for file transfer purposes
    uint32_t LowPriQueueMsec; ///< milliseconds

    /// Interval between timer OnTick() calls in microseconds
    uint32_t TimerIntervalUsec; ///< microseconds
} TonkStatus;

/// This is a data-structure returned by tonk_status().
/// This structure details the current state of the TonkConnection
typedef struct TonkStatusEx_t
{
    /// Shared 64-bit random key for this connection.
    /// This will not change
    uint64_t ConnectionKey;

    /// This is the network address of the local UDP socket provided by the peer.
    /// This may change infrequently
    TonkAddress Local;

    /// This is the latest network address of the remote UDP socket.
    /// This may change infrequently
    TonkAddress Remote;

    /// External (Internet) port that is mapped to the local port.
    /// Set to 0 if no mapping has succeeded so far
    uint16_t LocalNATMapExternalPort;

    /// External (Internet) port that is mapped to the remote port.
    /// Set to 0 if no mapping has succeeded so far
    uint16_t RemoteNATMapExternalPort;

    /// This is the ID for the local host assigned by the remote host.
    /// This may change infrequently
    uint32_t RemoteAssignedIdForLocalHost;

    /// Rate of data sent to peer, reported by peer, in BPS
    uint32_t PeerSeenBPS; ///< bytes per second

    /// Packetloss of messages sent to peer, reported by peer
    float PeerSeenLossRate; ///< 0..1

    /// One-way trip time for messages sent to peer, reported by peer
    uint32_t PeerTripUsec; ///< microseconds

    /// Rate of data received from peer, in BPS
    uint32_t IncomingBPS; ///< bytes per second

    /// Packetloss of messages received from peer
    float IncomingLossRate; ///< 0..1

    /// Average Maximum One-Way Delay (OWD) Network Trip time from peer
    uint32_t TripUsec; ///< microseconds
} TonkStatusEx;

/// TonkSocket: Represents a UDP port socket running the Tonk protocol
typedef struct TonkSocket_t { int impl; }* TonkSocket;

/// TonkConnection: Represents an incoming or outgoing connection with the socket
typedef struct TonkConnection_t { char impl; }* TonkConnection;

/// TonkAppContextPtr: Points to application context data tied to each connection
typedef void* TonkAppContextPtr;

/**
    TonkChannel

    Each message that is sent between TonkConnections is assigned to a numbered
    channel that is provided to the OnData() callback on the receiver.

    There are four types of message delivery:


    (1) Unreliable sent with TonkChannel_Unreliable.
    Unreliable messages can be up to TONK_MAX_UNRELIABLE_BYTES = 1280 bytes.

    Unreliable messages are delivered out of order.
    Unlike UDP datagrams, unreliable messages will never be duplicated.


    (2) Unordered sent with TonkChannel_Unordered.
    Unordered messages can be up to TONK_MAX_UNORDERED_BYTES = 1280 bytes.

    Unordered messages are delivered out of order.
    Eventually all unordered messages will be delivered.


    (3) Reliable messages sent with TonkChannel_Reliable0 + N.
    There are up to TonkChannel_Count = 6 reliable channels.
    Reliable messages can be up to TONK_MAX_RELIABLE_BYTES = 64000 bytes.

    For messages that can be even larger than this, the tonk_file_transfer.h
    buffer API may be a more suitable option.  For example, JSON data can grow
    beyond 64000 bytes unexpectedly.

    Reliable messages are delivered in order with other reliable messages.
    If a message is sent on (TonkChannel_Reliable0 + 1) followed by a message
    sent on (TonkChannel_Reliable0 + 2), then both are delivered in that order.
    Messages that cannot fit into one UDP datagram will be split up.

    The + N channel number from 0..TonkChannel_Last is given meaning by the app.
    As examples, it can be used to indicate the type of data being sent or
    it can be used to indicate that some data is part of a file transfer.


    (4) Low-priority messages sent with TonkChannel_LowPri0 + N.
    There are up to TonkChannel_Count = 6 LowPri reliable channels.
    LowPri messages can be up to TONK_MAX_LOWPRI_BYTES = 64000 bytes.
    As mentioned above, the tonk_file_transfer.h API may be more suitable.

    LowPri messages are delivered in order with other LowPri messages.
    If a message is sent on (TonkChannel_LowPri0 + 1) followed by a message sent
    on (TonkChannel_LowPri0 + 2), then both are delivered in that order.
    Messages that cannot fit into one UDP datagram will be split up.


    Delivery Order:

    Unreliable, Reliable, and LowPri data are not delivered in order with
    respect to eachother.  For example an Unreliable message that is sent
    after a Reliable message may be delivered before the Reliable message.
    Also a LowPri message sent before a Reliable message may be delivered
    after the Reliable message.

    When deciding what messages to deliver first, the Unordered and Unreliable
    messages are sent first, and then the Reliable messages, and then the
    LowPri messages.  The details of the message interleaving are complicated,
    and are currently undocumented.
*/

#define TONK_MAX_UNRELIABLE_BYTES 1280 /**< for TonkChannel_Unreliable */
#define TONK_MAX_UNORDERED_BYTES  1280 /**< for TonkChannel_Unordered */
#define TONK_MAX_RELIABLE_BYTES  64000 /**< for TonkChannel_Reliable0 + N */
#define TONK_MAX_LOWPRI_BYTES    64000 /**< for TonkChannel_LowPri0 + N */

typedef enum TonkChannel_t
{
    /// Number of reliable channels
    TonkChannel_Count       = 6,

    /// Last reliable channel
    TonkChannel_Last        = TonkChannel_Count - 1,

    /// In-order reliable message channels
    TonkChannel_Reliable0   = 50,
    TonkChannel_Reliable1   = 51,
    TonkChannel_Reliable2   = 52,
    TonkChannel_Reliable3   = 53,
    TonkChannel_Reliable4   = 54,
    TonkChannel_Reliable5   = 55,

    /// Low-priority in-order reliable message channels
    TonkChannel_LowPri0     = 150,
    TonkChannel_LowPri1     = 151,
    TonkChannel_LowPri2     = 152,
    TonkChannel_LowPri3     = 153,
    TonkChannel_LowPri4     = 154,
    TonkChannel_LowPri5     = 155,

    /// Unordered (Reliable) message channel
    TonkChannel_Unordered   = 200,

    /// Unreliable (Out-of-band) message channel
    TonkChannel_Unreliable  = 255
} TonkChannel;

/// TonkConnection Configuration
typedef struct TonkConnectionConfig_t
{
    /// Application context that will be returned by the callbacks below
    TonkAppContextPtr AppContextPtr TONK_CPP(= 0);

    /**
        [Optional] OnConnect() Callback

        Called in any case where the application has successfully connected to
        a remote peer:

        (1) tonk_connect() has successfully completed an outgoing connection.
        (2) TonkSocketConfig::OnIncomingConnection accepted a connection.
        (3) OnP2PConnectionStart() was called and now connection completed.

        If this is not specified (=0) then connection status can be checked via
        the tonk_status() function.
    */
    void (*OnConnect)(
        TonkAppContextPtr context,  ///< Application-provided context pointer
        TonkConnection connection   ///< Object that can be passed to tonk_*() API calls
        ) TONK_CPP(= 0);

    /**
        [Optional] OnData() Callback

        Called when data arrives from a TonkConnection.

        The data pointer will be valid until the end of the function call.

        It is important not to modify any of the provided data, since that
        data is later used for FEC decoding of other data.

        It is safe to call any tonk_*() API calls during this function.
    */
    void (*OnData)(
        TonkAppContextPtr context,  ///< Application-provided context pointer
        TonkConnection connection,  ///< Object that can be passed to tonk_*() API calls
        uint32_t          channel,  ///< Channel number attached to each message by sender
        const uint8_t*       data,  ///< Pointer to a buffer containing the message data
        uint32_t            bytes   ///< Number of bytes in the message
        ) TONK_CPP(= 0);

    /**
        [Optional] OnTick() Callback

        Periodic callback for each TonkConnection.

        It is safe to call any tonk_*() API calls during this function.
    */
    void (*OnTick)(
        TonkAppContextPtr context,  ///< Application-provided context pointer
        TonkConnection connection,  ///< Object that can be passed to tonk_*() API calls
        uint64_t          nowUsec   ///< Current timestamp in microseconds
        ) TONK_CPP(= 0);

    /**
        OnClose() Callback

        Called after the TonkConnection has closed.

        After this, other callbacks will not be called for this TonkConnection.

        After this, the TonkConnection object will still be valid:
        + tonk_status() call flags will indicate disconnected.
        + tonk_send() and other calls will fail but not crash.

        The application must call tonk_free() to free the TonkConnection object,
        which will invalidate the TonkConnection handle.
    */
    void (*OnClose)(
        TonkAppContextPtr context,  ///< Application-provided context pointer
        TonkConnection connection,  ///< Object that can be passed to tonk_*() API calls
        TonkResult         reason,  ///< TonkResult code indicating why the connection closed
        const char*    reasonJson   ///< JSON string detailing why the connection closed
        ) TONK_CPP(= 0);
} TonkConnectionConfig;

/// TonkSocket Configuration
typedef struct TonkSocketConfig_t
{
    /// Version of tonk.h - Set to TONK_VERSION
    uint32_t Version TONK_CPP(= TONK_VERSION);

    /// Application context
    TonkAppContextPtr AppContextPtr TONK_CPP(= 0);

    /// Suggested: 0 = Match CPU core count - 1
    uint32_t WorkerCount TONK_CPP(= 0);

    /// Kernel buffer sizes for UDP ports
    uint32_t UDPSendBufferSizeBytes TONK_CPP(= 2000 * 1000); ///< 2MB
    uint32_t UDPRecvBufferSizeBytes TONK_CPP(= 2000 * 1000); ///< 2MB

    /// Socket UDP port to listen on for connections
    /// Choose 0 for a random (client) port
    uint32_t UDPListenPort TONK_CPP(= 0);

    /// Client limit
    /// 0 = No incoming connections allowed
    uint32_t MaximumClients TONK_CPP(= 0);

    /// Maximum number of clients from the same IP address.
    /// Connections beyond this limit will be rejected.
    uint32_t MaximumClientsPerIP TONK_CPP(= 10);

    /// Number of connection attempts per minute before a connection flood is
    /// detected.  In response, Tonk will activate SYN cookie defenses.
    uint32_t MinuteFloodThresh TONK_CPP(= 120);

    /// OnTick() rate
    uint32_t TimerIntervalUsec TONK_CPP(= 30 * 1000); ///< 30 milliseconds
#define TONK_TIMER_INTERVAL_MIN (5 * 1000 /**< 5 milliseconds */)
#define TONK_TIMER_INTERVAL_MAX (100 * 1000 /**< 100 milliseconds */)

    /// Timeout before disconnection if no data is received
#define TONK_NO_DATA_TIMEOUT_USEC_MIN (2000 * 1000 /**< 2 second */)
#define TONK_NO_DATA_TIMEOUT_USEC_DEFAULT (20 * 1000 * 1000 /**< 20 seconds */)
#define TONK_NO_DATA_TIMEOUT_USEC_MAX (10 * 60 * 1000 * 1000 /**< 10 minutes */)
    uint32_t NoDataTimeoutUsec TONK_CPP(= TONK_NO_DATA_TIMEOUT_USEC_DEFAULT);

    /// Time between client sending UDP Connection Requests
    uint32_t UDPConnectIntervalUsec TONK_CPP(= 100 * 1000); ///< 100 milliseconds

    /// Timeout before disconnection if connection is not achieved
    uint32_t ConnectionTimeoutUsec TONK_CPP(= 4 * 1000 * 1000); ///< 4 seconds

    /// Interface network address to listen and send on.
    /// This can enable using 2+ WiFi interfaces to upload data simultaneously.
    /// Provide null (0) for all interfaces
    const char* InterfaceAddress TONK_CPP(= 0);

    /// Limit on how much bandwidth can be used by each connection.
    /// 0 = No limit
#define TONK_MAX_BPS (20 * 1000 * 1000 /**< 20 MBPS */)
    uint32_t BandwidthLimitBPS TONK_CPP(= TONK_MAX_BPS);

    /// Option: Enable forward error correction?
    /// This option reduces latency but will increase CPU usage, reducing the
    /// maximum number of clients that can connect at once.
#define TONK_FLAGS_ENABLE_FEC 1

    /// Option: Enable UPnP for this socket?
    /// This option is recommended if peer2peer connections are expected.
#define TONK_FLAGS_ENABLE_UPNP 2

    /// Option: Disable compression for this socket?
    /// Compression can cost a lot of extra CPU so it may be a good idea
#define TONK_FLAGS_DISABLE_COMPRESSION 4

    /// Some combination of the TONK_FLAGS_* above.
    uint32_t Flags TONK_CPP(= 0);

    /// Accept a connection from OnIncomingConnection()
#define TONK_ACCEPT_CONNECTION 1

    /// Deny a connection from OnIncomingConnection()
#define TONK_DENY_CONNECTION 0

    /**
        OnIncomingConnection() Callback

        Called when a new incoming connection completes, which was initiated by
        a remote peer on the network.

        If this function is not defined (function pointer is null) then the
        TonkSocket will not accept incoming client connections.

        Return non-zero to accept the connection, and fill in the configuration.
        Return TONK_DENY_CONNECTION to deny the connection.
    */
    uint32_t (*OnIncomingConnection)(
        TonkAppContextPtr  context, ///< [in] Application context from TonkSocketConfig
        TonkConnection  connection, ///< [in] Object that can be passed to tonk_*() API calls
        const TonkAddress* address, ///< [in] Address of the client requesting a connection
        TonkConnectionConfig* conf  ///< [out] Configuration to use for this connection
        ) TONK_CPP(= 0);

    /**
        [Optional] OnP2PConnectionStart() Callback

        Called just as a peer we are connected to has instructed us to connect
        to another peer using the tonk_p2p_connect() function.

        Unlike the other two On*Connection() functions, this function is called
        as a connection is starting rather than after it has completed.  This
        allows the application to provide data to send within the first round-
        trip of the peer2peer connection using tonk_send() in this function.

        If this function is not defined (function pointer is null) then the
        TonkSocket will not accept peer2peer connection requests.

        The connection address of the other peer is provided, which can be used
        to check if this connection should be permitted or denied.

        Return non-zero to accept the connection, and fill in the configuration.
        Return TONK_DENY_CONNECTION to deny the connection.
    */
    uint32_t (*OnP2PConnectionStart)(
        TonkAppContextPtr  context, ///< [in] Application context from TonkSocketConfig
        TonkConnection  connection, ///< [in] Object that can be passed to tonk_*() API calls
        const TonkAddress* address, ///< [in] Address of the other peer we are to connect to
        TonkConnectionConfig* conf  ///< [out] Configuration to use for this connection
        ) TONK_CPP(= 0);

    /**
        [Optional] OnAdvertisement() Callback

        This receives a message sent by tonk_advertise(), providing the source
        IP address and source port.  This function may receive between 0 and
        TONK_MAX_UNRELIABLE_BYTES bytes of data at any time until the app calls
        the tonk_socket_destroy() function.
    */
    void (*OnAdvertisement)(
        TonkAppContextPtr context, ///< [in] Application context from TonkSocketConfig
        TonkSocket     tonkSocket, ///< [in] TonkSocket
        const char*      ipString, ///< [in] IP address string of peer application
        uint16_t             port, ///< [in] Application port that sent the message
        uint8_t*             data, ///< [in] Message data
        uint32_t            bytes  ///< [in] Message bytes
        ) TONK_CPP(= 0);

    /// SendToHook context
    TonkAppContextPtr SendToAppContextPtr TONK_CPP(= 0);

    /**
        [Optional] SendTo() Hook

        If this is specified, then instead of sending data using Asio sockets,
        Tonk will instead call this function to send data.  The application
        will be responsible for delivering this data.

        The `destPort` will be the destination port.
        It should match the port on injected data for incoming connections
        or the port passed to tonk_connect() for outgoing connections.

        This function may be called from multiple threads simultaneously.
        The application is responisble for thread-safety.
    */
    void (*SendToHook)(
        TonkAppContextPtr context, ///< [in] Application context pointer
        uint16_t         destPort, ///< [in] Destination port
        const uint8_t*       data, ///< [in] Message data
        uint32_t            bytes  ///< [in] Message bytes
        ) TONK_CPP(= 0);
} TonkSocketConfig;


//------------------------------------------------------------------------------
// TonkSocket API
//
// Functions to create and shutdown TonkSocket objects.

/**
    tonk_socket_create()

    Creates a Tonk socket that can receive and initiate connections.

    TonkSocketConfig specifies the callbacks that will be called when events
    happen on the socket.

    If TonkSocketConfig::MaximumClients is non-zero, then the socket will
    accept new connections from remote hosts.  Note that the UDPPort should
    also be set to act as a server.

    Returns Tonk_Success on success, socketOut is set to the created socket.
    Returns other codes on failure, socketOut will be 0.
*/
TONK_EXPORT TonkResult tonk_socket_create(
    const TonkSocketConfig* config, ///< [in] Configuration for the socket
    TonkSocket*          socketOut, ///< [out] Created socket, or 0 on failure
    TonkJson*         errorJsonOut  ///< [out] Receives detailed error message on error
);

/**
    tonk_advertise()

    Sends an unreliable message to the provided IP address, or the entire subnet
    if TONK_BROADCAST is specified for `ipString`.  The `port` parameter must
    match the UDPListenPort set by the listening applications.  The message will
    be delivered to all applications outside of any TonkConnection, via the
    OnAdvertisement() callback.

    Applications that set UDPListenPort = 0 cannot receive messages sent to
    TONK_BROADCAST.  But if a peer sends tonk_advertise() and another peer gets
    that advertisement message in their OnAdvertisement() callback, then the
    source port will be available in the callback even if it was set to 0.

    Anywhere from 0 to TONK_MAX_UNRELIABLE_BYTES bytes of data may be sent,
    and the message is not guaranteed to reach any peers, so it should probably
    be sent periodically.

    One example application for tonk_advertise() is service discovery:
    Clients can call tonk_advertise() with TONK_BROADCAST to send a message
    to all services running on the given port.  Those services can respond by
    connecting to the client, or by sending a tonk_advertise() back to the
    source IP address of the OnAdvertise() message with more information,
    and allow the client to initiate the connection.

    Returns Tonk_Success on success.
    Returns other codes on failure.
*/

/// IP string that can be passed to tonk_advertise() to broadcast to the subnet
#define TONK_BROADCAST ""

TONK_EXPORT TonkResult tonk_advertise(
    TonkSocket tonkSocket, ///< [in] Socket to send advertisement from
    const char*  ipString, ///< [in] Destination IP address
    uint16_t         port, ///< [in] Destination port
    const void*      data, ///< [in] Message data
    uint32_t        bytes  ///< [in] Message bytes
);

/**
    tonk_inject()

    This is an interface that enables an application to specify when and how
    data is received rather than using real sockets.  The application provides
    the source port of the datagram and its contents.

    Returns Tonk_Success on success.
    Returns other codes on failure.
*/
TONK_EXPORT TonkResult tonk_inject(
    TonkSocket   tonkSocket, ///< [in] Socket to inject
    uint16_t     sourcePort, ///< [in] Source port of datagram
    const void*        data, ///< [in] Datagram data
    uint32_t          bytes  ///< [in] Datagram bytes
);

/**
    tonk_socket_destroy()

    Destroy the socket object, shutting down background threads and freeing
    system sources.

    If `shouldBlock` is non-zero:
        This function will block until all connections are closed.
        This is useful to simplify application cleanup for C++ code.
    If `shouldBlock` is zero:
        This call completes in a background thread.
        This is useful to simplify application cleanup for garbage collection.
*/
TONK_EXPORT void tonk_socket_destroy(
    TonkSocket tonkSocket, ///< [in] Socket to shutdown
    uint32_t shouldBlock   ///< [in] Boolean: Should this function block?
);


//------------------------------------------------------------------------------
// TonkConnection API

/**
    tonk_connect()

    Connect to remote host asynchronously.

    The returned TonkConnection can be used with tonk_send() to send data in the
    first connection datagram before a connection request has been sent.

    The first connection request is sent from a background thread on the next
    timer tick or when tonk_flush() is called.

    Optional ErrorJson parameter receives detailed error messages.

    Returns Tonk_Success on success, connectionOut will be valid.
    Returns other codes on failure, connectionOut will be 0.
*/
TONK_EXPORT TonkResult tonk_connect(
    TonkSocket          tonkSocket, ///< [in] Socket to connect from
    TonkConnectionConfig    config, ///< [in] Configuration for the connection
    const char*           hostname, ///< [in] Hostname of the remote host
    uint16_t                  port, ///< [in] Port for the remote host
    TonkConnection*  connectionOut, ///< [out] Set to connection on success, else 0
    TonkJson*         errorJsonOut  ///< [out] Receives detailed error message on error
);

/**
    tonk_status()

    Check status of the connection.
*/
TONK_EXPORT void tonk_status(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatus*          statusOut  ///< [out] Status to return
);

/**
    tonk_status_ex()

    Check extended status of the connection.
*/
TONK_EXPORT void tonk_status_ex(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatusEx*      statusExOut  ///< [out] Extended status to return
);

/**
    tonk_send()

    Send datagram on the given channel.

    This function can fail if the message is too large.
    Call tonk_status() to retrieve the maximum send size.

    Returns Tonk_Success on success.
    Returns other TonkResult codes on error.
*/
TONK_EXPORT TonkResult tonk_send(
    TonkConnection      connection, ///< [in] Connection to send on
    const void*               data, ///< [in] Pointer to message data
    uint64_t                 bytes, ///< [in] Message bytes
    uint32_t               channel  ///< [in] Channel to attach to message
);

/**
    tonk_flush()

    Flush pending datagrams.

    This will send data immediately instead of waiting for more data to be sent.
*/
TONK_EXPORT void tonk_flush(
    TonkConnection      connection  ///< [in] Connection to flush
);

/**
    tonk_close()

    Asynchronously start closing the connection.

    The TonkConnection object is still alive and it is valid to call any of the
    normal API calls.  Those calls will start failing rather than adding more
    data to the outgoing queue.

    Callbacks such as OnTick() and OnData() will still be generated.
    OnClose() will definitely be called at some point after tonk_close(),
    and it will be the last callback received.  OnClose() is an excellent place
    to call tonk_free() to release object memory.
*/
TONK_EXPORT void tonk_close(
    TonkConnection      connection  ///< [in] Connection to close
);

/**
    tonk_free()

    Free the TonkConnection object memory.

    This informs the Tonk library that the application will no longer reference
    a TonkConnection object.

    If the connection is alive, then it will continue to receive OnTick() and
    OnData() callbacks until the OnClose() callback completes.  To ensure that
    the session is disconnected, call tonk_close() and then wait for the
    OnClose() callback to be received.

    It is recommended to call tonk_free() from OnClose() or after OnClose().

    This function can also be called from the destructor of the application's
    UserContext object.  To support broadcasts of data from other users it is
    often useful to have a reference-counted UserContext.  Tonk supports this
    by allowing the application to keep the TonkConnection object alive a bit
    longer than the OnClose() callback.
*/
TONK_EXPORT void tonk_free(
    TonkConnection      connection  ///< [in] Connection to free
);


//------------------------------------------------------------------------------
// Time Synchronization API
//
// These functions allow for generation and transformation of timestamps between
// local and remote hosts.

/// Get current time in microseconds
TONK_EXPORT uint64_t tonk_time(); ///< Returns in microseconds

/**
    The time synchronization protocol uses an LSB of 8 microseconds.
    For a 24-bit timestamp centered around 0:

        8 usec * 2^24 / 2 = 67,108,864 usec ahead & behind

    This means that timeouts must be capped at 67 seconds in the protocol or
    else there will be wrap-around.  As a side-effect of the time synchronization
    math, we lose one bit of data, so the timestamps sent by users are truncated
    to just 23 bits.

    We can bias the value forward since timestamps in the past are more useful:

        8 usec * 2^23 / 2 - 16777216 = 16,777,216 usec ahead (TONK_23_MAX_FUTURE_USEC)
        8 usec * 2^23 / 2 + 16777216 = 50,331,648 usec behind (TONK_23_MAX_PAST_USEC)

    --> This is the range represented by the 23-bit timestamp API.

    For ease of use, 16-bit timestamps are available too.

    For a 16-bit timestamp centered around 0, we choose LSB = 512 usec:

        512 usec * 2^16 / 2 = 16,777,216 usec ahead & behind

    We can bias the value forward since timestamps in the past are more useful:

        512 usec * 2^16 / 2 - 8388608 = 8,388,608 usec ahead (TONK_16_MAX_FUTURE_USEC)
        512 usec * 2^16 / 2 + 8388608 = 25,165,824 usec behind (TONK_16_MAX_PAST_USEC)

    --> This is the 16-bit biased timestamp format we use.
*/

/// 16-bit general purpose timestamps with half-millisecond resolution:
#define TONK_TIME16_MAX_FUTURE_USEC  8388607 /**< 8 seconds */
#define TONK_TIME16_MAX_PAST_USEC   25165824 /**< 25 seconds */
#define TONK_TIME16_LSB_USEC             512 /**< microseconds LSB */

/**
    tonk_to_remote_time_16()

    Returns 16-bit remote timestamp from a local timestamp in microseconds.

    This is a timestamp that is expected to be in the remote host's time.
    It becomes invalid after 25 seconds.  The precision is 512 microseconds.
    See the TONK_TIME16 constants above.

    Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
    as the 'localUsec' parameter.

    Note that if tonk_status() TonkStatus::Flags does not have the
    TonkFlag_TimeSync bit set then the result is undefined.

    Returns the converted 16-bit network timestamp.
*/
TONK_EXPORT uint16_t tonk_to_remote_time_16(
    TonkConnection      connection, ///< [in] Connection with remote host
    uint64_t             localUsec  ///< [in] Local timestamp in microseconds
);

/**
    tonk_from_local_time_16()

    Returns local time given 16-bit local timestamp from a message
    in the OnData() callback.

    Returns the timestamp in local time in microseconds.
*/
TONK_EXPORT uint64_t tonk_from_local_time_16(
    TonkConnection      connection, ///< [in] Connection with remote host
    uint16_t    networkTimestamp16  ///< [in] Local timestamp to interpret
);

/// 23-bit general purpose timestamps with 8-microsecond resolution:
#define TONK_TIME23_MAX_FUTURE_USEC 16777215 /**< 16 seconds */
#define TONK_TIME23_MAX_PAST_USEC   50331648 /**< 50 seconds */
#define TONK_TIME23_LSB_USEC               8 /**< microseconds LSB */

/**
    tonk_to_remote_time_23()

    Returns 23-bit remote timestamp from a local timestamp in microseconds.

    It is recommended to serialize this value into 3 instead of 4 bytes.

    This is a timestamp that is expected to be in the remote host's time.
    It becomes invalid after 50 seconds.  The precision is 8 microseconds.
    See the TONK_TIME23 constants above.

    Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
    as the 'localUsec' parameter.

    Note that if tonk_status() TonkStatus::Flags does not have the
    TonkFlag_TimeSync bit set then the result is undefined.

    Returns the converted 23-bit network timestamp.
*/
TONK_EXPORT uint32_t tonk_to_remote_time_23(
    TonkConnection      connection, ///< [in] Connection with remote host
    uint64_t             localUsec  ///< [in] Local timestamp in microseconds
);

/**
    tonk_from_local_time_23()

    Returns local time given 23-bit local timestamp from a message
    in the OnData() callback.

    Returns the timestamp in local time in microseconds.
*/
TONK_EXPORT uint64_t tonk_from_local_time_23(
    TonkConnection      connection, ///< [in] Connection with remote host
    uint32_t    networkTimestamp23  ///< [in] Local timestamp to interpret
);


//------------------------------------------------------------------------------
// Peer2Peer Rendezvous Server for NAT Traversal

/**
    tonk_p2p_connect()

    Instruct two peers to connect with eachother using a custom NAT-traversal
    protocol described in HolePunch.README.txt.  It works when one side is on
    a friendly ISP like residential Comcast, but often fails if both sides are
    on e.g. corporate or phone connections.

    If either Alice or Bob are not time-synchronized, the function will fail and
    it will return Tonk_InvalidInput.

    Returns Tonk_Success on success.
    Returns other TonkResult codes on error.
*/
TONK_EXPORT TonkResult tonk_p2p_connect(
    TonkConnection aliceConnection,   ///< [in] Peer 1 connection
    TonkConnection   bobConnection    ///< [in] Peer 2 connection
);


//------------------------------------------------------------------------------
// Extra Tools

/// Fill in socket configuration defaults.
/// Provide the TONK_VERSION in the first argument.
/// This is useful when calling from other languages that do not support default
/// values for structures (for example: C#)
TONK_EXPORT TonkResult tonk_set_default_socket_config(
    uint32_t tonk_version,
    TonkSocketConfig* configOut);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CAT_TONKINESE_H
