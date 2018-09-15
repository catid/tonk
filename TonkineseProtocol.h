/** \file
    \brief Tonk Implementation: Wire Protocol Definition
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

/**
    Tonk Datagram Format:

    Encrypted Message Payload:
        <Frame (2 bytes)> <Message (0..2047 bytes)>
        <Frame (2 bytes)> <Message (0..2047 bytes)>
        <Frame (2 bytes)> <Message (0..2047 bytes)>
        ...

    Unencrypted Footer:
        [Sequence Number (0..3 bytes)]
        [Encryption Nonce (0..3 bytes)]
        [Handshake (12 bytes)]
        <Timestamp (3 bytes)>           ((Required))
        <Flags (1 byte)>                ((Required))
        <Encryption Tag (2 bytes)>      ((Required))

    The Encryption Tag covers all the datagram data up to but not including
    the Timestamp and Flags data.  This data is tagged after the rest of the
    data to avoid including the tag processing time in the timestamps.
*/

/**
    Encryption:

    The encryption scheme is not intended to provide data security.
    The goals of the encryption scheme are to:

    (1) Reject packets from unrelated connections
    (2) Reject packets duplicated by the network
    (3) Offer some obfuscation of the data
    (4) Offer some protection against modifying packets
    (5) Minimal impact to execution time

    See the SimpleCipher.h header for details.
*/

/**
    Connection Flags Byte:

    Datagrams that are part of a connection have the high bit set to 0.

        [A=1 B C D E F G H] (8 bits)
        MSB <-       -> LSB

    [A] = 1 : Packet is part of a connection
    [B] = Handshake field is present?  1 = yes
    [C] = Should compress sequence numbers?  1 = yes
    [D] = Only FEC Recovery Datagram?  1 = yes
    [E F] = # Bytes in <Encryption Nonce (0..3 bytes)> field
    [G H] = # Bytes in <Sequence Number (0..3 bytes)> field

    When [D] is set, the entire message payload is a single Recovery message,
    which saves two bytes of message frame overhead.

    When [C] is set, the receiver is allowed to compress sequence numbers to
    save overhead.  This is cleared by the sender if a packet is received out
    of order.
*/

/**
    Unconnected Types:

    Messages can be received outside of a connection when the Flags byte
    has the high bit set to 1.  This is used for P2P and Advertisements.

    Footer:
        <A=1 (1 bit)> <Type(7 bits)>
        <Encryption Tag (2 bytes)>

    [A(high bit)] = 0 : Packet is not part of a connection.
    [Type] is defined by the UnconnectedType enumeration below.
    [Encryption Tag] : Meaning defined by Type.
*/

/**
    Timestamp:

    Truncated tick counter when sending the UDP datagram.
    Truncated to 24 bits (3 bytes).  LSB = 8 microseconds.
    Stored in little-endian byte order;
    the first byte is the least-significant byte.

    Example:

        Timestamp A = 0000 0000  0100 0000  0000 0000
        Timestamp B = 0000 0000  0100 0000  0000 0010
                      MSB <-                   -> LSB

    Timestamp B in memory:

        Lower address byte -> 02 40 00 <- Higher address byte.

    B - A = 16 microseconds, or "B is 16 microseconds ahead of A."

    Note that all other fields are also stored in little-endian byte order.
*/

/**
    Sequence Number:

    This is a Siamese reliable packet number,
    which rolls over after 0x400000 packets have been sent.

    When the size of this field is 0 bytes, it means the datagram
    contains no reliable messages.

    The reliable packet number is truncated in this field, based on
    the last received acknowledgement message.

    Example:

        Last acknowledged sequence number = 0x3456a0
        Current datagram sequence number  = 0x3457b1

    The transmitted sequence number will be 0x57b1, which unambiguously
    represents the full 3 byte sequence number.
*/

/**
    Tonk Frame Bitfield:
        [Message Type(5 bits)] [Length(11 bits)]
        MSB <-                            -> LSB

     Length can range from 0..2047, and all values are valid.
     Message Type is described by the MessageType enumeration below.
*/

#include "TonkineseTools.h"

namespace tonk { namespace protocol {


//------------------------------------------------------------------------------
// Protocol Encoding Constants

/// Timestamp: 0 or 3 bytes
static const unsigned kTimestampBytes = 3;

/// Sequence Number: 0..3 bytes
static const unsigned kSequenceNumberBytes = 3;

/// Handshake: 0 or 12 bytes
static const unsigned kHandshakeBytes = 12;

/// Encryption Nonce: 0..3 bytes
static const unsigned kNonceBytes = 3;

/// Flags: 1 byte
static const unsigned kFlagsBytes = 1;

/// Encryption Tag: 2 bytes
static const unsigned kEncryptionTagBytes = 2;

/// The final 6 bytes are not included in the Encryption Tag field
static const unsigned kUntaggedDatagramBytes = 3 + 1 + 2;

/// Defines how the timestamp is combined with the flags during tag generation
inline uint32_t TimestampFlagTagInt(uint32_t timestamp24, uint8_t flags)
{
    return (timestamp24 << 8) | flags;
}

/// Maximum number of bytes added for random padding
static const unsigned kMaxRandomPaddingBytes = 15;

/// Maximum overhead possible.
/// This does not include random padding which is part of the message payload
static const unsigned kMaxOverheadBytes = \
    kTimestampBytes + kSequenceNumberBytes + kHandshakeBytes + \
    kNonceBytes + kFlagsBytes + kEncryptionTagBytes;

/// All message headers are 2 bytes
static const unsigned kMessageFrameBytes = 2;

/// Number of internal messaging channels
static const unsigned kNumChannels = 26;

/// Channel number for start of a multi-part reliable message
static const unsigned kReliableStartChannel0 = 0;

/// Channel number for end of a multi-part (or single-part) reliable message
static const unsigned kReliableEndChannel0 = TonkChannel_Count;

/// Channel number for start of a multi-part low-pri reliable message
static const unsigned kLowPriStartChannel0 = TonkChannel_Count * 2;

/// Channel number for end of a multi-part (or single-part) low-pri message
static const unsigned kLowPriEndChannel0 = TonkChannel_Count * 3;

/// Channel number for reliable in-order control messages
static const unsigned kControlChannel = TonkChannel_Count * 4;

/// Channel number for TonkChannel_Unordered message
static const unsigned kUnorderedChannel = TonkChannel_Count * 4 + 1;

static_assert(kNumChannels == (kUnorderedChannel + 1), "Channel count must match");

/// These are the Tonk Protocol Message types
enum MessageType
{
    MessageType_TimeSync = 0,
    MessageType_Acknowledgements = 1,
    MessageType_AckAck = 2,
    MessageType_Unreliable = 3,
    MessageType_Padding = 4,

    /// The remaining types are all assumed reliable:
    MessageType_StartOfReliables = 5,

    /// Reliable and LowPri types are decompressed from Compressed blocks.
    /// Unreliable, Unordered, and Control types are not compressed
    MessageType_Compressed = 5,

    /// First Reliable message channel
    MessageType_Reliable = 6,

    /// First LowPri message channel
    MessageType_LowPri = MessageType_Reliable + kLowPriStartChannel0,

    /// Internal control message.  Also unordered.  Not compressed
    MessageType_Control = MessageType_Reliable + kControlChannel,

    /// Unordered message.  Not compressed
    MessageType_Unordered = MessageType_Reliable + kUnorderedChannel,

    /// Count of message types
    MessageType_Count
};

/// Message frame header: High 5 bits are for the message type
static const unsigned kMessageTypeBits = 5;
static const unsigned kMessageTypeMask = 31;

// There are only room for 32 message types in the 5 bit type field
static_assert(MessageType_Reliable + kNumChannels == kMessageTypeMask + 1, "Too many types");
static_assert(MessageType_Count == 32, "Too many types");

/// Message frame header: Low 11 bits are for the length
static const unsigned kMessageLengthBits = 11;
static const unsigned kMessageLengthMask = 2047;

// We only have 2 bytes for the message frame header
static_assert(kMessageLengthBits + kMessageTypeBits == 16, "Too many bits");

/// Connection id count
static const uint32_t kConnectionIdCount = 0x1000000;

/// Size of the AckAck message.
/// This contains the last sequence number received from the last ack
static const unsigned kAckAckBytes = 3;


//------------------------------------------------------------------------------
// Protocol Behavior Constants

/// This will enable datagram nonce compression.
/// This is dangerous because re-ordering can cause messages to be received out
/// of order leading to data corruption.  The first time re-ordering is detected
/// the kSeqCompMask flag bit is cleared, which will diable sequence compression
#define TONK_ENABLE_NONCE_COMPRESSION

/// This will enable reliable sequence number compression
#define TONK_ENABLE_SEQNO_COMPRESSION

/// Random padding adds extra overhead (about 8 bytes on average) in order to
/// obscure the purpose of the encrypted datagrams.
#define TONK_ENABLE_RANDOM_PADDING

/// Minimum number of Asio worker threads.
/// This is set to 2 with the intent to encourage the datagram receive thread to
/// run more often, hopefully providing better timing data
static const unsigned kMinWorkerCount = 2; ///< 2 threads

/// Provide some reasonable upper limit for workers
static const unsigned kMaxWorkerCount = 256; ///< 256 workers

/// Minimum time between time sync updates
static const unsigned kTimeSyncIntervalUsec = 2 * 1000 * 1000; ///< 2 seconds

/// Time between connection requests
static const unsigned kConnectionIntervalUsec = 500 * 1000; ///< 500 msec

/// Minimum time between acknowledgements in microseconds
static const unsigned kMinAckIntervalUsec = 50 * 1000; ///< 50 msec

/// Maximum amount of time between datagrams before timestamps are attached
static const uint64_t kMaxTimestampIntervalUsec = 200 * 1000; ///< milliseconds

/// Initial number of timestamps to send shortly after connecting
static const unsigned kInitialTimestampSendCount = 128;

/// Number of timestamps to send in a row
static const unsigned kBurstTimestampSendCount = 2;

/// Hard limit on the number of ARQ retransmits we do per OnTick()
static const unsigned kRetransmitLimit = 10;

/// If datagrams are received out of order more than this number of datagrams,
/// then the old packet will not be accepted and will count as a lost datagram.
/// This is used for the nonce sliding window size
static const unsigned kMaxReorderedDatagrams = 8192; ///< datagrams

/// Bandwidth assumed bytes in an UDP/IPv4 header
static const unsigned kUDPIPV4_HeaderBytes = 28; ///< bytes

/// Bandwidth assumed bytes in an UDP/IPv6 header
static const unsigned kUDPIPV6_HeaderBytes = 48; ///< bytes

/// Maximum number of bytes per UDP/IPv4 datagram to stay under MTU
static const unsigned kUDPIPV4_MaxDatagramBytes = 1350; ///< bytes

/// Maximum number of bytes per UDP/IPv6 datagram to stay under MTU
static const unsigned kUDPIPV6_MaxDatagramBytes = 1330; ///< bytes

/// Maximum and minimum possible datagram byte limits
static const unsigned kMaxPossibleDatagramByteLimit = kUDPIPV4_MaxDatagramBytes;
static const unsigned kMinPossibleDatagramByteLimit = kUDPIPV6_MaxDatagramBytes;

/// Bytes allocated per packet in the packet compressor
static const unsigned kCompressionAllocateBytes = \
    kMaxPossibleDatagramByteLimit - kMaxOverheadBytes;

/// Minimum number of bytes to append per split message chunk
static const unsigned kMessageSplitMinimumBytes = 20; ///< bytes

/**
    Minimum interval between packet sends.
    This is also the Nagle (Flush) interval for clustering messages together.

    The biggest effect of this is that Acks are clocked during slow-start
    periodically at this interval.  So if the timer is slower, slow-start
    takes longer to detect the channel bandwidth.  For very short links like
    on a LAN, adding 5 millisecond additional latency on Acks is pretty high,
    and it might make Tonk slightly slower to start than TCP.
    TBD: Measure the difference.

    When operating below bandwidth limits of the network, this is not too
    important and might be optimized to be 20+ milliseconds instead to reduce
    mobile handset power usage.

    When being used for file transfer + realtime game, or for VoIP or Video Chat
    with variable quality levels, and other cases where the bandwidth limits of
    the network are being hit, having a shorter send interval is important as
    the last packet sent on the tick is subject to additional router queuing.

    I found that on my Windows desktop Asio has about 500 microseconds overhead
    for a timer callback, so picking 5 milliseconds reduces the impact of Asio
    timing jitter down to around 10%-ish.

    TBD: Tonk could switch to 30 millisecond interval for slower traffic?
    For now keeping it at a shorter interval until some data is collected.
    TBD: Is having a very short timer tick interval going to hurt CPU usage?
*/
static const unsigned kSendTimerTickIntervalUsec = 5000; ///< 5 milliseconds

/// Minimum bytes per second for the sender
static const unsigned kMinimumBytesPerSecond = 1000;

/// If no data is sent for a while, allow bandwidth usage to burst.
/// Slow start burst is 10 packets
static const int kBandwidthBurstTokenLimit = 1300 * 10;

/// Initial one-way-delay estimate in microseconds
static const unsigned kInitialOWDUsec = 100 * 1000;

/// Hard limit on the number of FEC retransmits we do per OnTick()
static const unsigned kRecoveryLimit = 8;

/// Minimum interval between recovery packets triggered by OWD timer
static const unsigned kMinimumRecoveryIntervalUsec = 10 * 1000; ///< 10 msec

/// Heuristic: Maximum interval length.
/// This is about 2.5x higher than WiFi lag spikes
static const uint32_t kBWMaxIntervalUsec = 325 * 1000; ///< 325 ms

/// Heuristic: Minimum bandwidth sample interval duration in microseconds.
/// Based on some common parameters:
/// - 10 ms of OS jitter
/// - 10 ms cellular network jitter (so 2x this for two samples at least)
/// - 4 ms cellular network jitter is also common
static const uint32_t kMinBWIntervalUsec = 22 * 1000; ///< 22 ms

/// Heuristic: Minimum bandwidth sample interval duration in datagrams
static const unsigned kMinBWIntervalDatagrams = 10; ///< 10 datagrams

/// Initial bandwidth probe offset
static const unsigned kInitialBPSProbeOffset = 20 * 1000; ///< 20KB

/// Minimum delay to probe in bandwidth control code
static const unsigned kMinimumDelayProbeUsec = 10 * 1000; ///< 10 msec

/// At least 1% FEC recovery packet rate
#define TONK_MIN_FEC_RATE 0.01f

/// Set minimum timeout to 300 ms, because when delay is low, latency spikes can cause this to trip incorrectly.
/// This sort of latency spike happens frequently on WiFi as the radio takes around 130 ms to periodically scan for other networks.
static const unsigned kNoAckSquelchTimeoutUsec = 300 * 1000; ///< 300 ms
static_assert(protocol::kNoAckSquelchTimeoutUsec >= protocol::kMinAckIntervalUsec * 3, "This should be much larger than the ack interval");

/// Slow start BPS reported to application
static const unsigned kSlowStartBPS = 100000; ///< 100 KBPS

/// Leave slow start after 130 KBPS - about 100 datagrams per second,
/// or 10 packets every 100 milliseconds, which is close enough to
/// a good statistics interval to accelerate faster.
static const unsigned kLeaveSlowStartBPS = 130 * 1000; ///< BPS

/// Packet loss rate above 10% is considered too high
#define TONK_HIGH_PLR_THRESHOLD 0.1f /**< 10% */

/// Packet loss rate below 2% is considered reasonable
#define TONK_LOW_PLR_THRESHOLD 0.02f /**< 2% */

/// UPnP lease duration in seconds
static const unsigned kUPnPLeaseDurationSecs = 3 * 60; ///< 3 minutes

/// UPnP lease duration in seconds
static const unsigned kUPnPLeaseRenewSecs = kUPnPLeaseDurationSecs - 10;

/// Number of retries for a few steps in UPnP port mapping
static const int kPortMappingRetries = 3; ///< Try a few times

/// Interval between requesting the LAN info
static const unsigned kLANInfoIntervalUsec = 5 * 1000 * 1000; ///< 5 seconds

/// Size of dummy packet for generating probe traffic
static const int kDummyBytes = 1300; ///< bytes

/// Expected queue delay we should see during BW probes if estimations are good
static const unsigned kDetectQueueDelayUsec = 10000; ///< microseconds


//------------------------------------------------------------------------------
// Unconnected Types Constants

/// Bytes of overhead for Advertisement on top of user data
/// [UserData(...)] [Magic(4 bytes)] [Flags = 00] [Checksum = 00 00]
static const unsigned kAdvertisementOverheadBytes = 4 + 3; ///< bytes

/// Magic value sent in advertisement datagram
static const uint32_t kMagicAdvertisement = 0x4b6e6f54; ///< "TonK"

/// This enumerates the meaning of the low 7 bits of the Flags byte,
/// when the high bit of the Flags byte is 0
enum UnconnectedType
{
    /// Advertisement sent by tonk_advertise()
    Unconnected_Advertisement = 0,

    /// P2P NAT Traversal Probe datagram sent by tonk_p2p_connect()
    Unconnected_P2PNATProbe = 1,

    /// Mask out the high bit of the Flags byte
    Unconnected_Mask = 127
};


//------------------------------------------------------------------------------
// Message Serializers

/// Write a message frame header
TONK_FORCE_INLINE void WriteMessageFrameHeader(
    uint8_t* messageData,
    unsigned messageType,
    unsigned messageBytes)
{
    TONK_DEBUG_ASSERT(messageType <= 31);
    TONK_DEBUG_ASSERT(messageBytes <= 2047);
    const unsigned headerWord = \
        (messageType << kMessageLengthBits) | messageBytes;

    siamese::WriteU16_LE(messageData, static_cast<uint16_t>(headerWord));
}


//------------------------------------------------------------------------------
// Footer: Flags and Optional Fields

/// Returns true if the datagram is truncated
bool DatagramIsTruncated(const uint8_t* data, size_t bytes);

/// Bitmasks for each of the flag fields:
/// These constants are not meant to be comprehensive but sufficient
/// to enable maintainers to search for and update any code using
/// flag bytes, when the protocol definition changes.
static const unsigned kConnectionMask = 0x80;
static const unsigned kHandshakeMask  = 0x40;
static const unsigned kSeqCompMask    = 0x20;
static const unsigned kOnlyFECMask    = 0x10;
static const unsigned kEncryptionNonceMask = 3 << 2;
static const unsigned kSequenceNumberMask  = 3;

/// Flags field interpreter
struct FlagsReader
{
    // Flags byte
    const uint8_t FlagsByte;


    // Constructor
    FlagsReader(uint8_t flagsByte) : FlagsByte(flagsByte) { }

    /// Returns true if this is a connection datagram type.
    /// Returns false if this is one of the UnconnectedTypes.
    TONK_FORCE_INLINE bool IsConnectionDatagram() const
    {
        return (FlagsByte & kConnectionMask) != 0;
    }

    /// Returns the UnconnectedType if IsConnectionDatagram() is false
    TONK_FORCE_INLINE unsigned GetUnconnectedType() const
    {
        return FlagsByte & Unconnected_Mask;
    }

    TONK_FORCE_INLINE bool IsOnlyFEC() const
    {
        return 0 != (FlagsByte & kOnlyFECMask);
    }
    TONK_FORCE_INLINE bool ShouldCompressSequenceNumbers() const
    {
        return 0 != (FlagsByte & kSeqCompMask);
    }
    TONK_FORCE_INLINE unsigned SequenceNumberBytes() const
    {
        return FlagsByte & kSequenceNumberMask;
    }
    TONK_FORCE_INLINE unsigned EncryptionNonceBytes() const
    {
        return (FlagsByte & kEncryptionNonceMask) >> 2;
    }
    TONK_FORCE_INLINE unsigned HandshakeBytes() const
    {
        return ((FlagsByte & kHandshakeMask) != 0) ? kHandshakeBytes : 0;
    }

    /// Returns the minimum number of bytes in the packet given these flags
    TONK_FORCE_INLINE unsigned ConnectedFooterBytes() const
    {
        return kEncryptionTagBytes + kFlagsBytes + kTimestampBytes +
            HandshakeBytes() + EncryptionNonceBytes() + SequenceNumberBytes();
    }
};

/// Data pointer should point to the next byte following the field to read
TONK_FORCE_INLINE uint32_t ReadFooterField(const uint8_t* data, unsigned bytes)
{
    TONK_DEBUG_ASSERT(bytes < 4);
    const uint32_t mask = ~(0xffffffff << (bytes * 8));
#ifdef GF256_ALIGNED_ACCESSES
    data -= bytes;
    const uint32_t value = ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
#else
    // Masking is faster than shifts as it is hidden by the read cost,
    // whereas shift-based calculations happen after the read..
    const uint32_t value = *(uint32_t*)(data - bytes);
#endif
    return value & mask;
}

/// Data pointer should point to the first byte of the field
TONK_FORCE_INLINE void WriteFooterField(uint8_t* data, uint32_t field, unsigned bytes)
{
    TONK_UNUSED(bytes);
    TONK_DEBUG_ASSERT(bytes < 4);
#ifdef GF256_ALIGNED_ACCESSES
    data[0] = (uint8_t)field;
    data[1] = (uint8_t)(field >> 8);
    data[2] = (uint8_t)(field >> 16);
#else
    // Masking is faster than shifts as it is hidden by the read cost,
    // whereas shift-based calculations happen after the read..
    *(uint32_t*)data = field;
#endif
}


//------------------------------------------------------------------------------
// Peer2Peer NAT Traversal Protocol
//
// Described in HolePunch.README.txt

/// Should we disable the random rounds 4+?
// TBD: Currently disabled because it doesn't work in my testing.
// I'm keeping the code around for now in the hope it can be fixed...
#define TONK_DISABLE_RANDOM_ROUND

/// Description of each NAT probe protocol round
enum NATProbeRounds
{
    NATProbeRound1_Exact  = 1, ///< Round 1 from HolePunch.README.txt
    NATProbeRound2_Fuzzy  = 2, ///< Round 2 from HolePunch.README.txt
    NATProbeRound3_Fuzzy2 = 3, ///< Round 3 from HolePunch.README.txt
    NATProbeRound4_Random = 4, ///< Round 4+ are completely random
};

/// Bytes per UDP/IP probe datagram (For IPv4 this is 47 bytes)
static const unsigned kNATProbeBytes = 5;

/// Number of ports to use in NATProbeRound1_Exact
static const unsigned kNATRound1Exact_PortCount = 16;

/// Number of ports to use in NATProbeRound23_Fuzzy
static const unsigned kNATRound23Fuzzy_PortCount = 32;

/// Cover +/- 128 ports during rounds 2 and 3 for 99.9% success rate
/// assuming certain common NAT configurations
static const unsigned kNATFuzzyPortRange = 128;

/// Number of ports to use in NATProbeRound4_Random
static const unsigned kNATRound4Random_PortCount = 1;

/// Probes per port to send while in NATProbeRound4_Random
static const unsigned kProbesPerPortForRandom = 1;

/// Maximum number of ports to open
#ifdef TONK_DISABLE_RANDOM_ROUND
static const unsigned kNATMaxPorts = kNATRound23Fuzzy_PortCount;
#else
static const unsigned kNATMaxPorts = 512;
#endif

/// Maximum amount of time to wait to the next NAT protocol round
static const uint64_t kMaxP2PRoundFutureDelayUsec = 4000 * 1000; ///< 4 seconds

/// Maximum interval between Peer2Peer rounds
static const uint64_t kMaxP2PRoundIntervalUsec = 500 * 1000; ///< 0.5 seconds

/// Minimum interval between Peer2Peer rounds
static const uint64_t kMinP2PRoundIntervalUsec = 200 * 1000; ///< 200 milliseconds

/// Connection timeout used when round 3 is enabled
static const uint64_t kP2PConnectionTimeout = 30 * 1000 * 1000; ///< 30 seconds

/// Interval between rounds 4+
static const uint64_t kP2PRound4Interval = 50 * 1000; ///< 50 ms

/// PCG Random seed tweaks
static const uint64_t kPcgDomainTieBreaker = 0xbaadbeef;
static const uint64_t kPcgDomainNotTieBreaker = 0xdeadd00d;

/// Calculate which port number to use for each port index given external port
/// that is visible to the rendezvous server.  For the first kNATRoundPortCount
/// ports it will cluster around the external port, and will use 0 for the rest.
uint16_t CalculateExactNATPort(uint16_t externalPort, unsigned portIndex);

/// Calculate which port number to use when randomly selecting ports near the
/// external port that is visible to the rendezvous server.
uint16_t CalculateFuzzyNATPort(uint16_t externalPort, siamese::PCGRandom& portPrng);

/// Choose a random client port between 1024 and 65535
uint16_t CalculateRandomNATPort(siamese::PCGRandom& portPrng);

/// Contains all the peer2peer connection parameters
struct P2PConnectParams
{
    static const unsigned kMinBytes = 1 + 8 + 2 + 2 + 2 + 2 + 4;
    static const unsigned kMaxBytes = 1 + 8 + 2 + 2 + 2 + 2 + 16;

    /// Set to true for one of the two peers
    bool WinTies = false;

    /// Randomly chosen for both peers
    uint64_t EncryptionKey = 0;

    /// Interval between sending probes
    /// = (min(OWD_a) + min(OWD_b)) * 2
    uint16_t ProtocolRoundIntervalMsec = 0;

    /// Truncated timestamp sent by rendezvous server indicating shot time
    /// = t0 + max(min(OWD_a), min(OWD_b)) * 2
    uint16_t ShotTS16 = 0; ///< 16-bit truncated timestamp units

    /// Unpacked shot time from ShotTS16
    uint64_t ShotTimeUsec = 0; ///< In local time

    /// My own external port visible to rendezvous server.
    /// This is used for the client to decide which ports to bind to in
    /// addition to the main socket.  Since NAT routers often pick the external
    /// port based on the bound port, this greatly improves the odds of success
    uint16_t SelfExternalPort = 0;

    /// If the peer was able to map an external port on their NAT router,
    /// then this is that port.  Otherwise this will be 0
    uint16_t PeerNATMappedPort = 0;

    /// External address of other peer visible to rendezvous server
    UDPAddress PeerExternalAddress;

    bool Write(siamese::WriteByteStream& stream) const;
    bool Read(siamese::ReadByteStream& stream);

    /**
        NAT Probe packets have a wire format of 5 bytes:

            <High probe key bytes(2 bytes)>
            <Flags(1 bytes)>
            <Low probe key bytes(2 bytes)>

        The probe key sent is the high 4 bytes of the 8 byte encryption key
        from the "tie-breaker" session.  And it will be the low 4 bytes of the
        8 byte encryption key from the "non-tie-breaker" session.
        The rendezvous server assigns one of the two peers with WinTies=1 and
        the other with WinTies=0.
    */
    TONK_FORCE_INLINE uint32_t GetOutgoingProbeKey() const
    {
        if (WinTies) {
            return static_cast<uint32_t>(EncryptionKey >> 32);
        }
        else {
            return static_cast<uint32_t>(EncryptionKey);
        }
    }

    TONK_FORCE_INLINE uint32_t GetExpectedIncomingProbeKey() const
    {
        if (WinTies) {
            return static_cast<uint32_t>(EncryptionKey);
        }
        else {
            return static_cast<uint32_t>(EncryptionKey >> 32);
        }
    }

    /**
        P2P handshake footer will contain the 8 byte encryption key, but the
        order of the low and high 32 bits will be different to accommodate the
        differences in the probe keys described above.  When a handshake is
        received, the low 4 bytes must match the expected probe key on both
        sides.

        This means that the sender of a handshake will swap the low and high
        32 bits of the key if the sender is a "tie breaker" and will not swap
        if the sender is the "non-tie-breaker."
    */
    TONK_FORCE_INLINE uint64_t GetOutgoingHandshakeKey() const
    {
        if (WinTies) {
            // Swap low and high words
            return (EncryptionKey << 32) | static_cast<uint32_t>(EncryptionKey >> 32);
        }
        else {
            return EncryptionKey;
        }
    }

    TONK_FORCE_INLINE uint64_t GetExpectedIncomingHandshakeKey() const
    {
        if (WinTies) {
            return EncryptionKey;
        }
        else {
            // Swap low and high words
            return (EncryptionKey << 32) | static_cast<uint32_t>(EncryptionKey >> 32);
        }
    }

    TONK_FORCE_INLINE uint64_t GetPcgDomain() const
    {
        if (WinTies) {
            return protocol::kPcgDomainTieBreaker;
        }
        else {
            return protocol::kPcgDomainNotTieBreaker;
        }
    }
};


//------------------------------------------------------------------------------
// Handshakes

/// Magic field that indicates this is a Tonk connection request
static const uint32_t kMagicC2SConnectionRequest = 0x6b6e6f54; ///< "Tonk"

/// Magic field that indicates this is a Tonk cookie key response
static const uint32_t kMagicS2CCookieResponse    = 0x546f6e6b; ///< "konT"

/// Magic field that indicates this is a Tonk P2P connection request
static const uint32_t kMagicP2PConnectionRequest = 0x50325054; ///< "TP2P"

/// Types of handshake footers
enum HandshakeType
{
    /// Not a handshake
    HandshakeType_NotHandshake,

    /// Client requesting a new connection
    /// <Magic (4 bytes)> <Key (8 bytes)>
    HandshakeType_C2SConnectionRequest,

    /// Server specifying a connection cookie key.
    /// This is done during periods of high connection rates
    /// to reduce the impact of connection floods.
    /// The client must send a new connection request with this key
    /// <Magic (4 bytes)> <CookieKey (8 bytes)>
    HandshakeType_S2CCookieResponse,

    /// Disconnection notification.  This is also used for connection rejections
    /// <Type (1 byte)> <0 (3 bytes)> <Key (8 bytes)>
    HandshakeType_Disconnect,

    /// Server reporting an unknown source address
    /// <Type (1 byte)> <0 (11 bytes)>
    HandshakeType_S2CUnknownSourceAddress,

    /// Client reporting an updated source address
    /// <Type (1 byte)> <Connection ID (3 bytes)> <Key (8 bytes)>
    HandshakeType_C2SUpdateSourceAddress,

    /// Peer2Peer connect
    /// <Magic (4 bytes)> <Key (8 bytes)>
    HandshakeType_Peer2PeerConnect,

    /// Client requesting service discovery
    /// <Magic (4 bytes)> <Key (8 bytes)>
    HandshakeType_C2SDiscovery,

    /// Server responding to discovery request
    /// <Magic (4 bytes)> <Key (8 bytes)>
    HandshakeType_S2CDiscovery,
};


namespace handshake {


/// Serializers: Data pointer must have kHandshakeBytes
void WriteC2SConnectionRequest(uint8_t* data, uint64_t key);
void WriteS2CCookieResponse(uint8_t* data, uint64_t cookie);
void WriteP2PConnectionRequest(uint8_t* data, uint64_t key);
void WriteDisconnect(uint8_t* data, uint64_t key);
void WriteS2CUnknownSourceAddress(uint8_t* data);
void WriteC2SUpdateSourceAddress(uint8_t* data, uint64_t key, uint32_t id);

/// Deserializers: Data pointer must have kHandshakeBytes
HandshakeType GetType(const uint8_t* data);
uint64_t GetKey(const uint8_t* data);
uint32_t GetConnectionId(const uint8_t* data);


} // namespace handshake


//------------------------------------------------------------------------------
// Packet Generators

static const unsigned kDisconnectBytes =
    protocol::kTimestampBytes + /* 3 bytes */
    protocol::kHandshakeBytes + /* WriteDisconnect() */
    protocol::kFlagsBytes + /* kHandshakeMask */
    protocol::kEncryptionTagBytes; /* 0 */

static const unsigned kS2CUnknownSourceBytes = kDisconnectBytes;
static const unsigned kS2CCookieResponseBytes = kDisconnectBytes;

/// Buffer must have kDisconnectBytes
void GenerateDisconnect(uint64_t key, uint8_t* datagram);

/// Buffer must have kS2CCookieResponseBytes
void GenerateS2CCookieResponse(uint64_t cookie, uint8_t* datagram);

/// Buffer must have kS2CUnknownSourceBytes
void GenerateS2CUnknownSource(uint8_t* datagram);


//------------------------------------------------------------------------------
// Reliable Control Channel

/**
    Tonk sets aside one reliable message channel for internal messages that are
    not delivered to the application but are still encrypted and reliably sent.

    This section describes the protocol that runs over the Control Channel.
*/
enum ControlChannelTypes
{
    /**
        ControlChannel_S2C_ClientIdAndAddress

        Telling the client its own source UDP/IP address/port.

        s2c 0x80 <Client ID (3 bytes)> <UDP Port (2 bytes)> <IP Address (4/16 bytes)>

        This is sent with the Client ID but also whenever the
        client's source address changes.
    */
    ControlChannel_S2C_ClientIdAndAddress = 0x80,

    /**
        ControlChannel_C2S_NATExternalPort

        Client tells the server what external port has been mapped to the
        client's main socket.  This is called as soon as a port mapping
        request completes on the client's router, so may be too late for
        a P2P connection request.

        c2s 0x85 <External UDP Port (2 bytes)>
    */
    ControlChannel_C2S_NATExternalPort = 0x85,

    /**
        ControlChannel_S2C_Peer2PeerConnect

        Telling the client to initiate a peer2peer connection to another client.

        s2c 0x90 <P2PConnectParams>
    */
    ControlChannel_S2C_Peer2PeerConnect = 0x90,

    /**
        ControlChannel_Disconnect

        Disconnecting gracefully.

        This allows datagrams to be passed to OnData() after a disconnect
        starts.  This is already part of the Tonk API contract.

        In some cases the application may put in a disconnect request
        slightly before new datagrams arrive and a race condition will
        happen where a session that the app recently disconnected is
        still delivering data.  Tonk handles this case by indicating in
        the API documentation that tonk_close() is asynchronous, and
        callbacks may continue to be received until OnClose() is
        finally called.

        s2c a0 a0 a0 a0
    */
    ControlChannel_Disconnect = 0xa0,
};

/// Maximum size including the type byte
static const unsigned kMaxClientIdAndAddressBytes = 1 + 3 + 2 + 16;

/// Number of bytes in a control channel NAT External Port message
static const unsigned kControlNATExternalBytes = 1 + 2;

/// Number of bytes in a control channel disconnect message
static const unsigned kControlDisconnectBytes = 1 + 3;


}} // namespace tonk::protocol
