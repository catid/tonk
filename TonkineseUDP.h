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

#pragma once

#include "TonkineseTools.h"
#include "TonkineseInterfaces.h"
#include "TonkineseMaps.h"
#include "TonkineseNAT.h"
#include "TonkineseFlood.h"

#include <memory>
#include <atomic>

namespace tonk {


//------------------------------------------------------------------------------
// Constants

/// Number of bytes to allocate for read buffers before recv + trimming
static const unsigned kReadBufferBytes = protocol::kMaxPossibleDatagramByteLimit;

/// Define this to enable the artifical packetloss code
#define TONK_ENABLE_ARTIFICIAL_PACKETLOSS
//#define TONK_DEFAULT_ARTIFICIAL_PACKETLOSS 0.03f /* Do not check this in;) */

/// This is only useful for MTU detection if we decide to do that
//#define TONK_DONT_FRAGMENT

/// This a paranoia feature to disallow spoofed packets from disconnecting us.
/// It is useful to leave off so that we can detect disconnection faster
//#define TONK_IGNORE_ICMP_UNREACHABLE


//------------------------------------------------------------------------------
// Datatypes

/// IPv4 versus IPv6 : Helps to control buffer sizes
enum class IPVersion
{
    Unspecified,
    V4,
    V6
};


//------------------------------------------------------------------------------
// UDPSocket

class UDPSocket
    : public IUDPSender
{
public:
    TONK_FORCE_INLINE UDPSocket()
    {
        ActiveUDPSocketCount++;
    }
    TONK_FORCE_INLINE virtual ~UDPSocket()
    {
        ActiveUDPSocketCount--;
    }

    struct Dependencies
    {
        BufferAllocator* ReadBufferAllocator;
        RefCounter* SocketRefCount;
        logger::Channel* Logger;
        std::shared_ptr<asio::io_context> Context;
        INATPortPool* NATPool;
        P2PConnectionKeyMap* P2PKeyMap;
        std::shared_ptr<gateway::MappedPortLifetime>* MappedPortPtr;
        ConnectionIdMap* IdMap;
    };

    Result Initialize(
        const Dependencies& deps,
        IPVersion bindIPVersion,
        uint16_t port,
        const TonkSocketConfig* socketConfig,
        TonkSocket apiTonkSocket,
        std::shared_ptr<asio::io_context::strand> asioConnectStrand);

    void Shutdown();

    /// Inject a recvfrom()
    Result InjectRecvfrom(
        const UDPAddress& addr,
        const uint8_t* data,
        unsigned bytes);

    /// Get the bound port number
    TONK_FORCE_INLINE uint16_t GetLocalPort() const
    {
        return LocalBoundPort;
    }

    /// Closes the socket, causing pending operations to abort and new operations to fail
    void CloseSocket();

    /// IUDPSender:
    void Send(
        const UDPAddress& destAddress,  ///< [in] Destination address for send
        DatagramBuffer       datagram,  ///< [in] Buffer to send
        RefCounter*        refCounter   ///< [in] Reference count to decrement when done
    ) override;

    TONK_FORCE_INLINE bool IsValid() const
    {
        return Socket != nullptr;
    }

#ifdef TONK_ENABLE_ARTIFICIAL_PACKETLOSS
    /// Set artifical packetloss rate between [0..1]
    static void SetArtificialPacketloss(float ploss = 0.01f);
#endif

    /// Add a firewall rule to allow inbound data on this port
    void AddFirewallRule();

    /// Map from source address to connection
    ConnectionAddrMap AddressMap;

protected:
    Dependencies Deps;
    const TonkSocketConfig* SocketConfig = nullptr;
    TonkSocket APITonkSocket = nullptr;

    /// UDP socket
    std::unique_ptr<asio::ip::udp::socket> Socket;

    /// Should the socket be closed right now?
    std::atomic<bool> SocketClosed = ATOMIC_VAR_INIT(false);

    /// Address associated with the last packet we received (maybe not our peer)
    UDPAddress SourceAddress;

    /// Strand that serializes connection requests
    std::shared_ptr<asio::io_context::strand> AsioConnectStrand;

#ifdef TONK_ENABLE_ARTIFICIAL_PACKETLOSS
    /// PRNG used during testing to decide when to drop packets artificially
    siamese::PCGRandom ArtificialPacketlossPRNG;
#endif

    /// Detector for floods
    FloodDetector FloodDetect;

    /// Locally bound port
    uint16_t LocalBoundPort = 0;

    TONK_IF_DEBUG(std::atomic<unsigned> OutstandingSends = ATOMIC_VAR_INIT(0));
    TONK_IF_DEBUG(std::atomic<unsigned> OutstandingRecvs = ATOMIC_VAR_INIT(0));


    /// Post next UDP recvfrom read.  Buffer must be kReadBufferBytes in size
    void postNextRead(uint8_t* buffer);

    /// Enable IP Do Not Fragment bit in IP header
    Result dontFragment(bool enabled);

    /// Ignore ICMP unreachable messages on the UDP socket - Use the ones from TCP
    Result ignoreUnreachable(bool enabled);

    /// Result of handling UDP datagrams with onUDPDatagram()
    enum class BufferResult
    {
        InUse,
        Free
    };

    /**
        onUDPDatagram()

        Handle data from remote peer and return how to handle buffers.
        The caller should already have validated that the received bytes
        are nonzero.

        receiveUsec: Time at which the data arrived.
        addr: Source address
        readBuffer: Datagram data (allocated by ReadBufferAllocator)
        receivedBytes: Datagram size in bytes

        Returns BufferResult::InUse if the buffer is consumed and will be
        freed by another thread.
        Returns BufferResult::Free if the buffer is not needed anymore
        and can be reused for example.
    */
    BufferResult onUDPDatagram(
        const uint64_t receiveUsec,
        const UDPAddress& addr,
        uint8_t* readBuffer,
        const unsigned receivedBytes);

    /// Handle a UDP datagram from an unrecognized source
    BufferResult onUnrecognizedSource(
        const UDPAddress& addr,
        const uint64_t receiveUsec,
        DatagramBuffer datagram);

    /// Handle handshake messages from unrecognized sources
    void onConnectRequest(
        uint64_t encryptionKey,
        const UDPAddress& addr,
        uint64_t receiveUsec,
        DatagramBuffer datagram);
};


} // namespace tonk
