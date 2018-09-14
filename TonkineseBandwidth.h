/** \file
    \brief Tonk Implementation: Bandwidth Control
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

/**
    TonkCC: Congestion Control Algorithm


    TonkCC is rate-based (rather than CWND), controlled by the receiver via
    Acknowledgement messages (rather than at the sender), has a Slow Start
    initialization, and Additive Increase Additive Decrease (AIAD) reactions
    to congestion by either packet-loss or delay signals.

    TonkCC is designed for delivering data at a high rate without introducing
    much additional delay, in an application-layer reliable UDP protocol.
    The main restriction this introduces is that OS-specific features are
    avoided, so e.g. MAC timestamps, raw sockets, and so on are not available.
    Furthermore all datagrams are delivered on relatively coarse timers and
    timing data may be inaccurate for many datagrams that arrive, so having
    access to the timing data for every datagram is useful.


    Prior Work:

    PropRate [4] introduced us to the idea of AIAD trailing a delay-based
    congestion indicator, where the increase and decrease rates are optimized.
    PropRate is based on CWND, has a monitor mode using a packet train,
    does not use packetloss as a signal, and is controlled at the sender.
    PropRate's delay-based congestion indicator it uses is baed on RTT
    measurements.  PropRate continuously estimates the bottleneck bandwidth
    and uses it to adjust the rate of increase and decrease.

    LEDBAT [2][5] introduced the idea we use of subtracting a minimum windowed
    filtered biased OWD measurement over a short and long time scale to estimate
    queuing delay.  LEDBAT uses a long time scale on the order of minutes,
    so it is subject to clock skew, whereas TonkCC uses a much shorter window
    to eliminate clock skew issues (as in Copa).

    Copa [1] demonstrated that it is effective to switch in/out of TCP-
    compatible modes.  It uses the double window delta of LEDBAT but with RTT,
    so it is affected by reverse traffic.  Copa uses the inter-packet gap to
    estimate a target send rate, which breaks down at higher rates unless the
    algorithm is incorporated more tightly with the operating system, which is
    unfortunately not an option for TonkCC.  Copa uses the same increase rate
    as the decrease rate, which the PropRate authors recommend to use only when
    optimizing for higher bandwidth.

    In addition to developing BBR, Google has been working on real-time
    congestion control schemes with the RMCAT working group.  Google moved ahead
    to implement their ideas in practice into a new algorithm for WebRTC called
    GoogCC [6].  GoogCC uses some fairly complicated filtering to go further
    than OWD and get differences between OWDs to estimate if they are trending
    to increase or decrease.  The trend line detector does not seem to reject
    outliers.  GoogCC uses packet loss rate as an additional congestion signal.
    Unforunately the algorithm is too complicated for me to reproduce in Tonk,
    and seems to be a work in progress with uncertain fairness between flows.

    TonkCC incorporates some features from each of these.  It uses the AIAD
    scheme of PropRate, controlled by the congestion signal from LEDBAT
    (with a shorter trigger) and packet loss as in GoogCC, with a slow start
    phase and TCP compatible mode similar to Copa.  The final algorithm is
    engineered to be very simple, leaving off a number of good improvements.


    Algorithm Description:

    TonkCC estimates route bottleneck Congestion Delay:
    With each incoming datagram, the (receive - send) timestamp deltas are
    compared over two time scales: (1) 4 seconds and (2) Smallest of:
    Minimum One-way Delay, Timer Interval * 6, and Longest Inter-Packet Gap * 2.
    The smallest (shortest trip time) in sets (1) and (2) are called the
    RecentOWD and the MinOWD.  And (RecentOWD - MinOWD) is an estimate
    of the instantaneous congestion in the network assuming that the router
    queue buffer drains around once every 4 seconds.  Note this calculation is
    on the receiver side with direct access to all OWDs, and it is invariant
    to clock drift over these time scales, and it is unaffected by traffic
    flowing in the reverse direction (congestion is only measured one-way).

    TonkCC estimates packetloss using an EWMA smoothing filter, updated each
    time a new bandwidth estimate is produced by the BandwidthEstimator.

    Congestion is detected when:
        (1) Congestion Delay exceeds 10 milliseconds.
        (2) Packet Loss Rate exceeds 10 percent.

    Caution is raised when:
        (1) Received bandwidth is under 75% the current bandwidth target.

    Otherwise No Congestion is detected.


    TonkCC Slow Start Initialization:

    Starting from a minimum rate, the controller will double the requested
    send rate each time at least 75% of the previous send rate is achieved.
    When congestion is first detected, Slow Start mode is stopped.


    TonkCC Steady State Mode:

        During no congestion, rate is increased.
        During caution, rate is held steady.
        During congestion, rate is reduced.

    There are several more details and tweaks in the UpdateCC() function code,
    which improved performance during simulations.


    Future Work:

    Before settling on this simple algorithm for Tonk, I first spent some time
    implementing several protocols using short pulses for congestion control.
    The idea was eventually realized in a way that worked on real networks and
    was interestingly invariant to RTT in that longer RTT did not lead to more
    delay.  For a satellite or IoT network this is a promising approach.
    For TonkCC this amount of complexity was too high, and it required some
    additional overhead in Acknowledgement messages to describe the pulse shape
    and one bit in each sent packet to indicate whether or not the datagram
    was part of a pulse.

    PropRate's monitor mode uses a packet train to help improve its step rate,
    which remains to be evaluated.  GoogCC's complex filtering may be a better
    signal than the current method, but this is not measured.  Copa has a
    velocity parameter that it tunes to react faster to bandwidth changes.

    All of these potential improvements were left out of TonkCC intentionally
    to keep the complexity of the software low for its first release.


    References (in /docs/ directory):

    [1] Copa: Practical Delay-Based Congestion Control for the Internet
        Venkat Arun, Hari Balakrishnan
    [2] Less-than-Best-Effort Service: A Survey of End-to-End Approaches
        Davis Ros, Michael Welzl
    [3] TCP-LP: A Distributed Algorithm for Low Priority Data Transfer
        Aleksandar Kuzmanovic, Edward W. Knightly
    [4] TCP Congestion Control Beyond Bandwidth-Delay Product for Mobile Cellular Networks
        Wai Kay Leong, Zixiao Wang, Ben Leong
    [5] Evaluation of Different Decrease Schemes for LEDBAT Congestion Control
        Mirja Kuhlewind, Stefan Fisches
    [6] A Google Congestion Control Algorithm for Real-Time Communication
        S. Holmer, H. Lundin
        https://webrtc.googlesource.com/src/+/master/modules/congestion_controller/goog_cc/
*/

#pragma once

#include "TonkineseTools.h"
#include "TonkineseProtocol.h"
#include "TimeSync.h"

namespace tonk {


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Sender Side

/**
    BandwidthShape

    This is the bandwidth shaping requested by the receiver with each
    acknowledgement message.

    AppBPS: Application bandwidth
    This is the bandwidth that should be reported to the application for
    available bandwidth.

    FECRate: Forward Error Correction (FEC) minimum rate
    Controls the FEC rate relative to the application bandwidth.
    If the application sends less than AppBPS, then FEC rate is based on the
    amount of data actually sent.  This is the rate sent while not probing.
*/

/// BandwidthShape : Bandwidth probe shape updated by receiver
struct BandwidthShape
{
    /// Application send rate in bytes per second
    unsigned AppBPS = 0; ///< Bytes/Second

    /// Rate 0.01 - 0.51 of FEC relative to the actual application send rate
    /// Used while not probing for higher bandwidth
    float FECRate = 0.f; ///< Rate


    /// Bytes used in compressed representation
    static const unsigned kCompressedBytes = 2 + 1;

    /// Compresses the shape into a byte buffer.
    /// Precondition: Buffer is at least kCompressBytes in size
    void Compress(uint8_t* buffer) const;

    /// Decompresses the shape from the given buffer.
    /// Precondition: Buffer is at least kCompressBytes in size
    void Decompress(const uint8_t* buffer);
};

/// Overhead bytes added to Siamese library Acknowledgement message.
/// 3 bytes for PeerNextExpectedNonce and bytes for BandwidthShape
static const unsigned kAckOverheadBytes = 3 + BandwidthShape::kCompressedBytes;

/**
    Packet Pacing with a Token Bucket

    Packet pacing means to send data at a regular rate rather than in bursts.
    Tonk sends more data every kSendTimerTickIntervalMsec milliseconds.

    For Tonk Bandwidth Control, packet pacing is important in order to get good
    signal from the queue-induced delay that we use for bandwidth detection.
    When sent at or below the link bandwidth, packet pacing minimizes the delay
    experienced by each packet through the network, which helps with Tonk's goal
    of delivering data as quickly as possible.

    There is a good discussion of why packet pacing is preferred over sending
    in bursts in the "TCP Pacing Revisited" paper here:
    http://people.cs.pitt.edu/~ihsan/pacing_cal.pdf

    The Token Bucket method is used to implement the rate limiter, which is
    described thoroughly by these two Wikipedia articles:
    https://en.wikipedia.org/wiki/Token_bucket
    https://en.wikipedia.org/wiki/Leaky_bucket
*/

/// Sender's bandwidth control algorithm
class SenderBandwidthControl
{
public:
    struct Dependencies
    {
        TimeSynchronizer* TimeSync;
        logger::Channel* Logger;
    };

    struct Parameters
    {
        /// Maximum bytes per second set by application.
        /// 0 = No limit
        unsigned MaximumBPS = 0; ///< Bytes per second
    };

    /// Initialize subsystem
    void Initialize(
        const Dependencies& deps,
        const Parameters& params);

    /// Recalculate the available bytes to send on this tick
    void RecalculateAvailableBytes(uint64_t nowUsec);

    /// Quickly drop send rate to minimum
    void Squelch();

    /// Get the number of FEC recovery packets to send right now.
    /// This is updated by RecalculateAvailableBytes()
    unsigned CalculateRecoverySendCount(uint64_t nowUsec);

    /// Scale factor used for FEC recovery token bucket
    static const int kReliableRewardTokens = 1024;

    /// Update available bytes
    TONK_FORCE_INLINE void OnSendDatagram(
        int compressionSavings, ///< How many bytes were saved due to compression?
        int wireBytes,          ///< How many bytes are actually being sent?
        bool isReliable)        ///< Does the datagram contain reliable messages?
    {
        // Subtract the wire bytes used from the available token counter
        AvailableTokens -= wireBytes;

        // If this message was reliable:
        if (isReliable) {
            // Credit some tokens to the recovery send counter.
            // This means that we send more recovery datagrams proportional to
            // the number of packets to protect at the chosen FEC rate.
            RecoverySendTokens += kReliableRewardTokens;
        }

        // Credit some tokens for sending more probe data
        ProbeSendTokens += wireBytes + compressionSavings;
    }

    /// Number of bytes that can be sent right now.
    /// This may be negative indicating a deficit in available buffer space
    TONK_FORCE_INLINE int GetAvailableBytes() const
    {
        return AvailableTokens;
    }

    // Limit ProbeBPS to (AppBPS * 1.5) or lower
    static const unsigned kProbeTokenRatio = 2;

    /// Number of bytes that can be sent right now for bandwidth probing.
    /// This may be negative indicating a deficit.
    TONK_FORCE_INLINE int GetAvailableProbeBytes() const
    {
        return ProbeSendTokens / kProbeTokenRatio;
    }

    /// Should we send another probe?
    bool ShouldSendProbe(int bytes)
    {
        return GetAvailableProbeBytes() >= bytes
            && GetAvailableBytes() >= bytes;
    }

    /// Spend some of the available probe bytes
    TONK_FORCE_INLINE void SpendProbeBytes(int bytes)
    {
        ProbeSendTokens -= bytes * kProbeTokenRatio;
    }

    /// Provides realtime feedback from receiver
    void OnReceiverFeedback(BandwidthShape shape);


    //--------------------------------------------------------------------------
    // Public API: Threadsafe functions

    /// Threadsafe: Return current bytes per second
    TONK_FORCE_INLINE int GetAppBPS() const
    {
        return Shape.AppBPS;
    }

    /// Threadsafe: Calculate how long it will take to send data at the
    /// bandwidth limit, including available tokens
    unsigned CalculateSendTimeMsec(unsigned bytes);

protected:
    Dependencies Deps;
    Parameters Params;

    /// Number of tokens available for sending in the current tick.
    /// Negative if tokens were borrowed in the past
    std::atomic<int> AvailableTokens = ATOMIC_VAR_INIT(0);

    /// Use a timer to ensure that recovery packets are sent regularly even at low data rate
    Timer RecoveryTimer;

    /// Time that the last send interval started
    uint64_t LastIntervalStartUsec = 0;

    /// Current target bandwidth shape from receiver
    BandwidthShape Shape;

    /// These are tokens earned for each byte transmitted.
    /// These are spent to inject FEC recovery packets towards Shape.FECRate
    int RecoverySendTokens = 0;

    /// Tokens spent for each FEC recovery packet, based on Shape.FECRate
    int RecoverySendCost = 0;

    /// These are tokens earned for each byte submitted by the application.
    /// These are spent to inject extra packets to probe bandwidth at a higher
    /// rate than the application produces
    int ProbeSendTokens = 0;
};


//------------------------------------------------------------------------------
// DatagramInfo

/// Describes a received datagram for the receiver's bandwidth control algorithm
struct DatagramInfo
{
    /// Nonce of the datagram.  Used to detect data received out of order
    NonceT Nonce = 0; ///< Incremented once per packet

    /// Estimated one-way-delay (OWD) for this datagram in microseconds.
    /// This does not include processing time only network delay and perhaps
    /// some delays from the Operating System when it is heavily loaded.
    /// Set to 0 if trip time is not available
    unsigned NetworkTripUsec = 0;

    /// Send timestamp of the datagram in TS24 units.
    /// This is used for calculating relative delay between packet pairs, and
    /// for calculating bandwidth used at the sender side.
    Counter24 SendTS24 = 0; ///< TS24 units

    /// Timestamp this datagram was received.  Maybe slightly behind real time.
    /// This is useful for checking relative delay between packet pairs, and for
    /// calculating bandwidth at the receiver side.
    uint64_t ReceiveUsec = 0; ///< Microseconds

    /// Number of data bytes in the datagram (including UDP/IP headers).
    /// This is used for calculating send and receive bandwidth.
    unsigned DatagramBytes = 0; ///< Bytes

    /// Takes the send timestamp from the sender and expands to 64 bits,
    /// and then takes the difference with local time converted to TS24.
    /// (Local receive time in TS24) - (Remote send time in TS24)
    /// This is used for comparing the relative transmission times of two
    /// packet pairs without adding a dependency on time synchronization.
    Counter32 ReceiveSendDeltaTS24; ///< TS24 units
};


//------------------------------------------------------------------------------
// ReceiverStatistics

/// Aggregate statistics returned infrequently back to sender side
struct ReceiverStatistics
{
    /// Bytes per second received
    unsigned GoodputBPS = 0; ///< Bytes per second

    /// Packet loss rate
    float LossRate = 0.f; ///< Rate 0..1

    /// Trip time in microseconds
    unsigned TripUsec = 0; ///< Microseconds


    /// Bytes used to represent the structure in compressed form
    static const unsigned kCompressBytes = 2 + 1 + 2;

    /// Compresses the stats into a byte buffer.
    /// Precondition: Buffer is at least kCompressBytes in size
    void Compress(uint8_t* buffer) const;

    /// Decompresses the stats from the given buffer.
    /// Precondition: Buffer is at least kCompressBytes in size
    void Decompress(const uint8_t* buffer);
};


//------------------------------------------------------------------------------
// InterPacketGapEstimator

/// Statistics tracking inter-packet gap (IPG)
class InterPacketGapEstimator
{
public:
    /// Handle a datagram being received.
    /// It is okay if the datagram is re-ordered
    void OnDatagram(uint64_t receiveUsec);

    /// Handle an interval ending
    void OnIntervalEnd();

    /// Get the smoothed maximum IPG
    unsigned Get() const
    {
        return SmoothedMaxUsec;
    }

protected:
    /// Time that last in-sequence datagram was received
    uint64_t LastReceiveUsec = 0;

    /// Maximum inter-packet gap in the past interval
    unsigned MaxUsec = 0;

    /// Smoothed maximum inter-packet gap seen over time
    unsigned SmoothedMaxUsec = 0;
};


//------------------------------------------------------------------------------
// BandwidthEstimator

/**
    BandwidthEstimator

    The goal is to use as few packets as possible to estimate the received
    bandwidth, so that statistics can be gathered more quickly, leading to
    faster reaction times in bandwidth control.

    The smallest delay in an interval is used to represent that interval
    for network trip time.  The sum of the bytes over that interval
    is used to calculate instantaneous bandwidth.

    The theory of the instantaneous statistics estimator is that packets travel
    through the network and OS in small bursts due to all sorts of effects.
    The sender side sends in small bursts to avoid CPU-intensive precise packet
    release timing that would require burning a whole core.  The burst interval
    is small enough to avoid impacting latency (e.g. 5 millisecond bursts) and
    packets may be sent in-between if the application calls tonk_flush().
    Bursts end up being detectable via the one-way delay.

    When receiving data below the channel bandwidth limit:

    The burst delay patterns of the sender will be visible in the received
    packets, and we should carefully decide where to begin/end bins of packets
    for gathering statistics.  This is the main case the algorithm handles.
    When receiving data above/near the channel bandwidth limit:

    Packets will be received in a roughly consistent rate, so almost any range
    of packets will be good for estimating the received bandwidth and delay.

    Example rising-delay bursts:
                                x
          x          x        x x
        x x          x        x x
        x x        x x      x x x
      x x x      x x x    x x x x
    x x x x    x x x x    x x x x ...
    Time ------|==========|---------->

    We want to measure at the starts of these bursts.  This fully
    covers the entire timeline, and each burst represents the received
    bandwidth accurately.

    The first few packets in a rising-delay burst have lower delay,
    which increases to a maximum.  This type of burst is caused by
    sending a burst of packets close together, which spread out due
    to network router queues along the route.

    Operating system jitter and latency spikes on sender/receiver will
    cause artificial delay, which will flush over time back to a minimum
    delay.  We want to reject these incorrect signals.

    WiFi adapters will periodically scan for networks, causing periodic
    128 millisecond delays.  To account for that, the smoothed maximum
    inter-packet gap (IPG) is continuously calculated, and statistics
    collection must wait for at least IPG * 2 to elapse.

    We collect data over an interval until:
    + At least 22 milliseconds has elapsed, to avoid OS/network jitter issues.
    We do not wait for RTT or OWD based delays because it may be a long link.
    + At least 10 packets have been received, for statistical significance.
    At low rates we wait at least 325 ms and for at least two packets.
*/

struct BurstMeasurement
{
    /// Timestamp for this measurement
    uint64_t TimestampUsec = 0;

    /// Smoothed estimate of goodput
    unsigned SmoothedGoodputBPS = 0;

    /// Pretty accurate measure of BPS the sender is achieving over a small
    /// recent time interval.  If there is packetloss, this is a smaller rate
    /// than the sender actually sent.
    unsigned LatestProbeBPS = 0;

    /// Smoothed estimate of maximum trip time
    unsigned SmoothedMaxTripUsec = 0;

    /// Smoothed estimate of minimum trip time
    unsigned SmoothedMinTripUsec = 0;

    /// Smoothed PLR
    float SmoothedPLR = 0.f;

    /// Smallest ReceiveSendDeltaTS24 in the burst
    Counter32 MinReceiveSendDeltaTS24 = 0;
};

class BandwidthEstimator
{
public:
    /// Update on datagram received.
    /// Returns true if statistics were updated
    bool UpdateOnDatagram(const DatagramInfo& datagram);

    /// Latest measurement results
    BurstMeasurement Measurement;

    /// Statistics tracking inter-packet gap (IPG)
    InterPacketGapEstimator InterPacketGap;

protected:
    /// Interval start
    DatagramInfo Start;

    /// Seeking a minimum value
    bool SeekingMinimum = true;

    /// Previous in-order datagram info
    DatagramInfo Previous;

    /// DatagramBytes sum before Minimum
    unsigned IntervalDatagramBytes = 0;

    /// Datagram count in interval
    //unsigned IntervalDatagramCount = 0;

    /// Count before Minimum
    unsigned IntervalInSeqCount = 0;

    /// Maximum seen so far in this interval
    unsigned IntervalMaxTripUsec = 0;

    /// Next expected nonce
    uint64_t NextExpectedNonce = 0;


    /// Called when interval ends
    void OnInterval(unsigned intervalUsec);
};


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Receiver Side

#define TONKCC_CONGESTION_PLR (0.1f) /**< 10% */

/// Receiver's bandwidth control algorithm
class ReceiverBandwidthControl
{
public:
    struct Dependencies
    {
        TimeSynchronizer* TimeSync;
        logger::Channel* Logger;
    };

    /// Initialize bandwidth control
    void Initialize(const Dependencies& deps);

    /// Called just after a datagram has been processed by the application.
    /// The time at which this is called includes the processing time of the
    /// application for this packet
    void OnProcessedDatagram(const DatagramInfo& datagram);

    /// Update send shape from the timer tick
    /// smoothedSendDelayUsec: Peer's smoothed value for OWD for ACKs we send
    void UpdateSendShape(uint64_t nowUsec, unsigned smoothedSendDelayUsec);

    /// Returns true if the receiver should send an ack immediately.
    /// This is needed during slow-start and when bandwidth has changed a lot
    TONK_FORCE_INLINE bool GetShouldAckFast() const
    {
        return ShouldAckFast;
    }

    TONK_FORCE_INLINE void OnSendAck()
    {
        ShouldAckFast = false;
    }

    /// Provides realtime feedback from receiver
    TONK_FORCE_INLINE BandwidthShape GetSenderShape() const
    {
        return NextShape;
    }

    /// Get statistics for infrequent reporting
    ReceiverStatistics GetStatistics() const;

protected:
    Dependencies Deps;

    /// Estimator for bandwidth (BW)
    BandwidthEstimator Bandwidth;

    /// Next bandwidth shape to send
    BandwidthShape NextShape;

    /// Should the receiver send an ack immediately
    /// or wait for the next ack timer to expire?
    bool ShouldAckFast = false;

    using WindowMinTS24 = siamese::WindowedMinMax<Counter32, siamese::WindowedMinCompare<Counter32> >;

    /// Minimum OWD over a short window (RecentOWD_WindowUsec)
    WindowMinTS24 RecentOWD;

    /// RecentOWD window duration in microseconds
    unsigned RecentOWD_WindowUsec = 100 * 1000;

    /// Minimum OWD over a longer window (MinOWD_WindowUsec)
    WindowMinTS24 MinOWD;

    /// MinOWD window duration in microseconds
    unsigned MinOWD_WindowUsec = 1000 * 1000;

    using WindowedMinUnsigned = siamese::WindowedMinMax<unsigned, siamese::WindowedMaxCompare<unsigned> >;

    /// Maximum BPS received over a short window (RecentOWD_WindowUsec)
    WindowedMinUnsigned RecentMaxBW;

    /// Is the CC method in slow-start?
    bool InSlowStart = true;

    /// Controls the rate of change in bandwidth
    int Momentum = 1;

    /// Start time of the current new trend
    uint64_t LastTickUsec = 0;


    enum class Decision
    {
        PushUpdateInAck,
        NoChange
    };

    /// Update bandwidth target
    Decision UpdateCC(
        uint64_t nowUsec,
        unsigned smoothedSendDelayUsec);
};


//------------------------------------------------------------------------------
// Serializers

/// Represent a 32-bit integer with 16 bits using fixed point.
/// 5 bits of exponent, representing offsets of 0..20.
/// 11 bits of mantissa, providing a precision of 1/2048 = 0.048828125%.
/// The return value will decompress within 0.1% of the input word
TONK_FORCE_INLINE uint16_t FixedPointCompress32to16(uint32_t word)
{
    if (word == 0) {
        return 0;
    }

    const unsigned nonzeroBits = NonzeroLowestBitIndex(word) + 1;
    TONK_DEBUG_ASSERT(nonzeroBits >= 1 && nonzeroBits <= 32);

    if (nonzeroBits <= 11) {
        TONK_DEBUG_ASSERT(word < 2048);
        return (uint16_t)word;
    }

    const unsigned shift = nonzeroBits - 11;
    TONK_DEBUG_ASSERT(shift < 32);

    TONK_DEBUG_ASSERT((word >> shift) < 2048);

    return (uint16_t)((word >> shift) | (shift << 11));
}

TONK_FORCE_INLINE uint32_t FixedPointDecompress16to32(uint16_t fpword)
{
    return (uint32_t)(((uint32_t)fpword & 2047) << ((uint32_t)fpword >> 11));
}

/// Represent a 16-bit integer with 8 bits using fixed point.
/// 4 bits of exponent, representing offsets of 0..15.
/// 4 bits of mantissa, providing a precision of 1/16 = 6.25%.
/// The return value will decompress within 13% of the input word
TONK_FORCE_INLINE uint8_t FixedPointCompress16to8(uint16_t word)
{
    if (word == 0) {
        return 0;
    }

    const unsigned nonzeroBits = NonzeroLowestBitIndex(word) + 1;
    TONK_DEBUG_ASSERT(nonzeroBits >= 1 && nonzeroBits <= 16);

    if (nonzeroBits <= 4) {
        TONK_DEBUG_ASSERT(word < 16);
        return (uint8_t)word;
    }

    const unsigned shift = nonzeroBits - 4;
    TONK_DEBUG_ASSERT(shift < 16);

    TONK_DEBUG_ASSERT((word >> shift) < 16);

    return (uint8_t)((word >> shift) | (shift << 4));
}

TONK_FORCE_INLINE uint16_t FixedPointDecompress8to16(uint8_t fpword)
{
    return (uint16_t)(((uint16_t)fpword & 15) << ((uint16_t)fpword >> 4));
}


} // namespace tonk
