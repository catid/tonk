/** \file
    \brief Mau : Network Simulator for UDP data
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Mau nor the names of its contributors may be
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

#ifndef CAT_MAU_H
#define CAT_MAU_H

/** \mainpage
    Mau : Network Simulator for UDP data

    Mau.h provides a simple C API for a powerful, multithreaded network sim.
    Designed for real-time unit testing and performance testing applications,
    Mau provides a proxy UDP port that simulates some network conditions,
    supporting simultaneous connections over the same parallel link.
    It also provides send hooks that bypass sockets, enabling more tests to
    run in parallel on the same PC with less overhead.

    Gilbert-Elliott channel model:

    + Packetloss rate
    + After a loss occurs, the probability that a packet makes it through

    Router knobs:

    + Router throughput in MBPS
    + Depth of router queue in KB
    + Random Early Detection mode

    Latency knobs:

    + Minimum latency

    Re-ordering knobs:

    + Re-order minimum extra latency
    + Re-order maximum extra latency
    + Re-order rate (uniform dist), adding variable extra delay to packets
    + Decaying rate at which the next packet will have the same extra delay

    Tampering knobs:

    + Duplicate packet rate (uniform dist)
    + Packet single bit flip rate (uniform dist)

    Non-goals:

    - Does not simulate cross-traffic patterns.  Can be added in-app.

    Mau uses the Asio library for portable networking and strands.
*/

/// Library version
#define MAU_VERSION 3

// Tweak if the functions are exported or statically linked
//#define MAU_DLL /* Defined when building/linking as DLL */
//#define MAU_BUILDING /* Defined by the library makefile */

#if defined(MAU_BUILDING)
# if defined(MAU_DLL)
    #define MAU_EXPORT __declspec(dllexport)
# else
    #define MAU_EXPORT
# endif
#else
# if defined(MAU_DLL)
    #define MAU_EXPORT __declspec(dllimport)
# else
    #define MAU_EXPORT extern
# endif
#endif

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


//------------------------------------------------------------------------------
// Shared Constants/Datatypes

/// These are the result codes that can be returned from the mau_*() functions
typedef enum MauResult_t
{
    /// Success code
    Mau_Success = 0,

    /// A function parameter was invalid
    Mau_InvalidInput       = 1,

    /// An error occurred during the request
    Mau_Error              = 2,

    /// Hostname lookup failed
    Mau_HostnameLookup     = 3,

    /// Packet was too large
    Mau_PacketTooLarge     = 4,

    /// Network library (asio) returned a failure code
    Mau_NetworkFailed      = 5,

    /// Socket creation failed
    Mau_SocketCreation     = 6,

    /// Port is in use
    Mau_PortInUse          = 6,

    /// Out of memory
    Mau_OOM                = 15,

    MauResult_Count, /* for asserts */
    MauResult_Padding = 0x7fffffff /* int32_t padding */
} MauResult;

/// Evaluates true if the result is a success code
#define MAU_GOOD(result) (Mau_Success == (result))

/// Evaluates true if the result is a failure code
#define MAU_FAILED(result) (Mau_Success != (result))

/// MauProxy: Represents a UDP port socket running the Mau proxy
typedef struct MauProxy_t { int impl; }* MauProxy;

/// MauAppContextPtr: Points to application context data
typedef void* MauAppContextPtr;

/// MauProxy Channel Configuration
typedef struct MauChannelConfig_t
{
    /// Packetloss knobs:

    /// Using the same seed each time will attempt to reproduce the same
    /// loss sequences and may be useful to reproduce issues more reliably.
    /// Set to 0 to pick a random seed each time.
    uint32_t RNGSeed = 1;

    /// Gilbert-Elliott channel model:

    /// Packetloss rate
    float LossRate = 0.01f; ///< [0, 1]

    /// After a loss occurs, this is the probability that a packet makes it through
    float DeliveryRate = 0.5f; ///< [0, 1]

    /// Router knobs:

    /// Router throughput in MBPS
    float Router_MBPS = 1.f; ///< MBPS

    /// Depth of router queue in milliseconds
    unsigned Router_QueueMsec = 100; ///< milliseconds

    /// Is Random Early Detection (RED) enabled?
    bool Router_RED_Enable = true;

    /**
        Add random packet drops beyond some depth.
        Loss rate will be rescaled after this fraction.

        Example:

        Router_RED_QueueFraction = 0.6
        Queue depth = 0.7

        RED loss rate = (0.7 - 0.6) / (1.0 - 0.6) = 40%
        RED loss rate = (0.9 - 0.6) / (1.0 - 0.6) = 75%
    */
    float Router_RED_QueueFraction = 0.5f; ///< fraction

    ///< Maximum packet loss rate (PLR) to impose
    float Router_RED_MaxPLR = 0.02f; /// loss rate

    /// Latency knobs:

    /// Minimum latency
    uint32_t LightSpeedMsec = 20; ///< in milliseconds

    /// Re-ordering knobs:

    /// Re-order minimum extra latency
    uint32_t ReorderMinimumLatencyMsec = 50; ///< in milliseconds

    /// Re-order maximum extra latency
    uint32_t ReorderMaximumLatencyMsec = 150; ///< in milliseconds

    /// Re-order rate (uniform dist), adding variable extra delay to packets
    float ReorderRate = 0.005f; ///< [0, 1]

    /// Decaying rate at which the next packet will have the same extra delay
    /// After reorder of packet N, reorder rate for N+1, N+2, ... = DR.
    /// Until a packet is not reordered and then reorder rate is normal again.
    float OrderRate = 0.3f; ///< [0, 1]

    /// Tampering knobs:

    /// Duplicate packet rate (uniform dist)
    float DuplicateRate = 0.001f; ///< [0, 1]

    /// Packet single bit flip rate (uniform dist)
    float CorruptionRate = 0.001f; ///< [0, 1]
} MauChannelConfig;

/// Hook function provided by app for sending datagrams, rather than using sockets
typedef void(*MauSendHookFunction)(
    MauAppContextPtr context,
    uint16_t destPort,
    const void* data,
    unsigned bytes);

/// MauProxy Configuration
typedef struct MauProxyConfig_t
{
    /// Version of mau.h
    uint32_t Version = MAU_VERSION;

    /// Kernel buffer sizes for UDP ports
    uint32_t UDPSendBufferSizeBytes = 64000; ///< 64KB
    uint32_t UDPRecvBufferSizeBytes = 64000; ///< 64KB

    /// Socket UDP port to listen on for data from client.
    uint32_t UDPListenPort = 10200;

    /// Maximum bytes per datagram
    uint32_t MaxDatagramBytes = 1500;

    /// Context for SendHook
    MauAppContextPtr SendHookContext = nullptr;

    /// If non-zero this is used instead of sockets
    MauSendHookFunction SendHook = nullptr;
} MauProxyConfig;


//------------------------------------------------------------------------------
// MauProxy API
//
// Functions to create and shutdown MauProxy objects.

/**
    mau_proxy_create()

    Creates a Mau proxy object that can proxy a UDP session.
    The source of the first UDP packet received is considered the client.

    Packets are forwarded back and forth between the client and server, subject
    to configured channel parameters.

    When MauProxyConfig::SendHook is not specified:
    Initially it will have to resolve the server hostname, so until that is
    completed any received client packets will be queued up for delivery.

    Returns Mau_Success on success, proxyOut is set to the created proxy.
    Returns other codes on failure, proxyOut will be 0.
*/
MAU_EXPORT MauResult mau_proxy_create(
    const MauProxyConfig*    config, ///< [in] Configuration for the socket
    const MauChannelConfig* channel, ///< [in] Channel configuration
    const char*            hostname, ///< [in] Hostname of the server
    uint16_t                   port, ///< [in] UDP Port for the server
    MauProxy*              proxyOut  ///< [out] Created proxy, or 0 on failure
);

/**
    mau_proxy_inject()

    Inject a packet directly rather than using sockets.

    Returns Mau_Success if there were no errors during the injection.
    Returns an error code if any error was encountered.
*/
MAU_EXPORT MauResult mau_proxy_inject(
    MauProxy proxy,      ///< [in] Proxy object
    uint16_t sourcePort, ///< [in] Source port
    const void* data,    ///< [in] Datagram buffer
    unsigned bytes       ///< [in] Datagram bytes
);

/**
    mau_proxy_config()

    Change a MauProxy channel configuration.
*/
MAU_EXPORT void mau_proxy_config(
    MauProxy proxy, ///< [in] Proxy object
    const MauChannelConfig* channel ///< [in] Channel configuration
);

/**
    mau_proxy_stop()

    Disable the proxy.  No callbacks will be called after this.
    This is useful to synchronize the shutdown of a client/server unit test,
    or to simulate a long period without data.
*/
MAU_EXPORT void mau_proxy_stop(
    MauProxy   proxy ///< [in] Proxy object
);

/**
    mau_proxy_destroy()

    Destroy the socket object, shutting down background threads and freeing
    system sources.

    If this returns an error code then the test is invalidated.

    Returns Mau_Success if there were no errors during the proxying.
    Returns an error code if any error was encountered.
*/
MAU_EXPORT MauResult mau_proxy_destroy(
    MauProxy proxy ///< [in] Proxy object
);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CAT_MAU_H
