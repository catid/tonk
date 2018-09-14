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

#pragma once

#include "TonkineseTools.h"
#include "TonkineseProtocol.h"
#include "TimeSync.h"

/**
    Tonk Bandwidth Control (TBC)

    Tonk incorporates both Congestion Control and Flow Control into one
    receiver-side algorithm called Bandwidth Control.  In Bandwidth Control,
    the receiver decides how much data should be sent by the sender in each
    acknowledgement message that the receiver returns to the sender.

    Tonk Bandwidth Control does not require any data to be sent reliably.
    It uses the received bandwidth, one-way delay, and packetloss as signals.

    The receiver analyzes incoming packet timestamps and sizes, when packets
    arrive, and how long it takes to process the packets.  It continuously
    recalculates optimal bandwidth limits for the sender to use, and
    communicates the sender's new bandwidth limit with a few bytes attached
    to each acknowledgement message, which effectively probes different
    bandwidth rates over time.  The receiver uses these probes to
    continuously explore the available bandwidth.
*/

/**
    Tonk Bandwidth Control : Applications

    Tonk is designed with three major applications in mind:
    (1) Low latency messaging - Minimize latency (Aggressive = 0)
    (2) High-speed large data transfer - Maximize bandwidth (Aggressive = 11)
    (3) Realtime VoIP/VideoConference - Moderate and reactive bandwidth usage

    All of these applications are expected to run primarily on mobile networks
    and sometimes also via a traditional ISP.

    For all three applications, flow control is needed to avoid building up
    a large queue delay at the receiver as packets if packets are arriving
    faster than they can be processed.

    For low latency messaging (1) such as online games, the message rate
    should be carefully chosen so that a burst of traffic does not cause the
    average delay to rise to durations that would be visible to the user.
    QoS applied to different types of data can prioritize sending the latency-
    sensitive data first and the bulk latency-insensitive data later.

    For application (2), the application needs to know from Tonk how deep the
    buffer queues are getting.  For file transfer case (2), this is used to
    make sure the network is fed with data to send so that the network is
    never starved on the sender isde.

    For realtime reactive traffic (3) the sender needs to know the available
    bandwidth so that the codec can be chosen appropriately to maximize the
    available resources and avoid building up a queue that would break the
    live stream.
*/

/**
    Tonk Bandwidth Control : Probing algorithm

    The sender is instructed to ramp bandwidth usage over time as described
    by the BandwidthShape data structure below, which effectively probes
    available bandwidth.  There are three cases that can then be observed
    at the receiver, regardless of what was requested:


    (1) Bandwidth received tracks the bandwidth sent.
    Delay does not spike when the bandwidth sent and received ramps up.

    In case (1) the supplied bandwidth from the sender does not exceed the
    capacity of the channel, and the receiver can request additional bandwidth
    above any value used for probing.

    If the delay is correlated and increasing with the sent bandwidth,
    then there is a trade-off for the receiver: If it values low latency
    then the receiver should not request higher bandwidth.


    (2) Bandwidth received tracks the bandwidth sent, partially.
    Delay spikes when the bandwidth received hits a maximum value.

    In case (2) the sender is operating near the maximum bandwidth of the
    channel.  The point before delay starts increasing and where the
    bandwidth levels off can be considered the maximum bandwidth.

    The receiver can then make several trade-offs between bandwidth and
    delay, such as choosing lower probe amplitudes or backing off from the
    maximum bandwidth to avoid adding delay.


    (3) Bandwidth received is uncorrelated with send bandwidth.
    Delay spikes at higher send bandwidths.

    The bandwidth must be reduced and probed again at a lower value.

    Other cases than (1)(2)(3) indicate that some cross traffic or
    network topology change has occurred in the middle of a probe and
    invalidated the data, so additional data should be collected.


    Prior work:

    The probing algorithm is inspired by some prior work.  It incorporates
    a bandwidth probe similar to Google's BBR [1].  Using the delay as a
    signal was explored recently by the authors of PropRate [2] and has
    been a popular approach for a long time [3][4][5][6].  Delay is used in
    addition to bandwidth for better mobile network performance [2] and to
    better serve the needs of applications that are delay sensitive.


    The novel elements introduced by Tonk Bandwidth Control (TBC):

    (a) It supports Forward Error Correction using the SiameseFEC library,
    and the receiver also instructs the sender to send additional recovery
    symbols in addition to the original and retransmitted datagrams.
    The goal of FEC is to reduce delay caused by RED and other packetloss
    for realtime applications.

    (b) The best of bandwidth [1] and delay [2] measurements are combined
    to produce a more accurate estimate of the channel characteristics.

    (c) Calculations are performed at the receiver to improve the quantity
    of data available for analysis.  The results are transmitted to the
    sender to limit bandwidth usage, which improves reaction time.
    The flow control is also combined with the congestion control since
    both control algorithms are running on the receiver side.
    The sender becomes a simple packet scheduler, which in the client/server
    case may reduce load on the server allowing for more clients.

    (d) Probe shape is adapted over time based on channel knowledge to
    further reduce the delay incurred by probing.


    References:
    [1] BBR: Congestion-Based Congestion Control
    http://queue.acm.org/detail.cfm?id=3022184
    [2] TCP Congestion Control Beyond Bandwidth-Delay Product for Mobile Cellular Networks
    https://www.comp.nus.edu.sg/~bleong/publications/conext17-proprate.pdf
    [3] The Probe Gap Model can Underestimate the Available Bandwidth of Multihop Paths
    https://www.cc.gatech.edu/fac/Constantinos.Dovrolis/Papers/spruce-ccr.pdf
    [4] pathChirp: Efficient Available Bandwidth Estimation for Network Paths
    http://www.slac.stanford.edu/pubs/slacpubs/9500/slac-pub-9732.pdf
    [5] Ten Fallacies and Pitfalls on End-to-End Available Bandwidth Estimation
    https://www.cc.gatech.edu/fac/Constantinos.Dovrolis/Papers/p120-jain.pdf
    [6] Mitigating Egregious ACK Delays in Cellular Data Networks by Eliminating TCP ACK Clocking
    https://www.comp.nus.edu.sg/~bleong/publications/icnp13-tcprre.pdf
    [7] An End-to-End Measurement Study of Modern Cellular Data Networks
    https://dl.acm.org/citation.cfm?id=2722270
*/

/**
    Tonk Bandwidth Control: FEC Recovery Packet Scheduling

    Like Acknowledgement messages, FEC recovery messages are allowed to bypass
    the bandwidth limit as they are part of the channel recovery method.

    To be effective, FEC recovery packets must be sent at some minimum interval,
    which for Tonk is tuned to be the one-way delay of the channel, provided
    by the time synchronization code, and not faster than 10 milliseconds, set
    by kMinimumRecoveryIntervalUsec.

    The Siamese FEC library performs retransmissions (RTO) of original data
    after max(RTT) * 1.5 = OWD * 3 or higher.  The goal of FEC is to recover
    faster, so it must send recovery data at some interval that is smaller than
    this retransmission timeout.

    We could send FEC recovery data at a massive rate to reduce latency more,
    but this would be expensive.  Tonk's goal is to modestly recover from single
    losses within a single one-way delay (OWD) interval.  But it can clearly
    be amped up a lot more to recover faster in trade for bandwidth.  The amount
    of extra bandwidth used by FEC is based on the data rate, and the worked
    examples below highlight the trade-offs.


    Worked example behavior (low rate):

    + One-way delay is 100 milliseconds (common on the Internet).
    + Packet loss rate = 1% (also common).
    + Original data flow = 10 KB/s with a packet size of 1000 bytes.

    Traditional TCP-style ARQ would have RTO = Max(RTT) * 1.5, which takes at
    least OWD * 3 to notice a loss, and another OWD to deliver a retransmission,
    assuming the retransmission is not also lost.  This means at least 400 msec
    to deliver in the case of a loss, which is a huge user-noticable delay for
    interactive applications.

    So, one packet is sent every 100 msec, and if the Tonk receiver decides to
    request FEC recovery packets at 3 times the loss rate, then it would only
    send one FEC recovery packet every 3.333 seconds.  Instead, Tonk sends one
    FEC recovery packet per OWD on a timer, enabling one chance to recover
    each OWD.  That is sending 10 FEC recovery packets per second.

    FEC overhead = 10 KB/s for 1000 byte packets, 13 KB/s for max size packets,
    as the size of the FEC recovery packets is similar to the original packets.
    So in this case FEC recovery doubles the overall bandwidth used, though the
    bandwidth used overall is still fairly small just 20 KB/s.

    This means that there will be about 3 times to recover faster than ARQ
    retransmission before ARQ will act as a safety net, and it may take one
    extra OWD = 100 ms (worst case) to recover from a single loss, where ARQ
    would take 400 ms (best case).


    Worked example behavior (medium rate):

    + One-way delay is 100 milliseconds (common on the Internet).
    + Packet loss rate = 1% (also common).
    + Original data flow = 1 MB/s with a packet size of 1000 bytes.

    So, one packet is sent each millisecond, and if the Tonk receiver decides to
    request FEC recovery packets at 3 times the loss rate, then it will send
    three packets per OWD, enabling three chances to recover each OWD.  That is
    sending 30 FEC recovery packets per second.

    FEC overhead = 30 KB/s for 1000 byte packets, 39 KB/s for max size packets,
    as the size of the FEC recovery packets is similar to the original packets.
    So in this case FEC only takes 3% additional bandwidth over the original
    data, but reduces the latency significantly.

    This means that at 3 times the loss rate there will be about 9 times to
    recover faster than ARQ retransmission before ARQ will act as a safety net,
    and it may OWD / 3 = 33 ms (worst case) to recover from a single loss,
    where ARQ would take at least 400 ms (best case), a 10x improvement!


    So why not just use FEC recovery for every situation?

    FEC recovery works best for a small number of losses and it adds overhead.
    FEC recovery at higher data rates starts becoming probablistic rather than
    100% reliable (Siamese has a 99.7% recovery success rate).  Siamese FEC
    takes larger CPU overhead for higher loss rates as O(N^2), which does not
    matter on desktop-class CPUs but will affect mobile devices.  For this
    reason Siamese FEC is limited to correcting up to 255 losses at a time.

    For a large burst of losses, it is much more efficient to just resend the
    original data.  For applications that do not have real-time requirements,
    only sending the data that was missing after some delay is much more
    bandwidth and CPU efficient.

    For variably-sized data, FEC recovery is much less bandwidth efficient than
    retransmission because FEC recovery packets are at least as large as the
    largest of the original sent data.

    In my testing, sending FEC recovery packets for retransmission  does not
    help with latency, when FEC data is already being sent periodically.
*/

namespace tonk {


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Sender Side

/// BandwidthShape : Bandwidth probe shape updated by receiver
struct BandwidthShape
{
    /// Is the sender in slow start?
    bool InSlowStart = false;

    /// Bytes that can be sent since slow start began
    unsigned SlowStartBytes = 0; ///< Bytes

    /// Bytes per second target send rate
    unsigned AppBPS = 0; ///< Bytes/Second

    /// Rate 0.01 - 0.51 of FEC requested for the next interval.
    /// This is used to pad out the app traffic up to a high send rate to probe
    /// for available bandwidth
    float FECRate = 0.f; ///< Rate


    /// Bytes used in compressed representation
    static const unsigned kCompressBytes = 1 + 2;

    /// Compresses the shape into a byte buffer.
    /// Precondition: Buffer is at least kCompressBytes in size
    void Compress(uint8_t* buffer) const;

    /// Decompresses the shape from the given buffer.
    /// Precondition: Buffer is at least kCompressBytes in size
    void Decompress(const uint8_t* buffer);
};

/// Overhead bytes added to Siamese library Acknowledgement message.
/// 3 bytes for PeerNextExpectedNonce and bytes for BandwidthShape
static const unsigned kAckOverheadBytes = 3 + BandwidthShape::kCompressBytes;

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

    /// Set UDP/IP overhead bytes
    TONK_FORCE_INLINE void SetUDPIPHeaderBytes(unsigned bytes)
    {
        UDPIPHeaderBytes = bytes;
    }

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
    TONK_FORCE_INLINE void OnSendDatagram(int bytes, bool isReliable)
    {
        // Increment the slow-start sent byte counter
        SlowStartSentBytes += (unsigned)bytes;
        TONK_DEBUG_ASSERT(SlowStartSentBytes >= (unsigned)bytes); // Overflow

        // Include the UDP/IP header for bandwidth metering
        bytes += UDPIPHeaderBytes;

        // Subtract the bytes used from the available token counter
        AvailableTokens -= bytes;

        // If this message was reliable:
        if (isReliable) {
            // Credit some tokens to the recovery send counter.
            // This means that we send more recovery datagrams proportional to
            // the number of packets to protect at the chosen FEC rate.
            RecoverySendTokens += kReliableRewardTokens;
        }
    }

    /// Number of bytes that can be sent right now
    TONK_FORCE_INLINE unsigned GetAvailableBytes() const
    {
        // If there are no available tokens:
        if (AvailableTokens <= 0) {
            // Do not return negative numbers
            return 0;
        }
        return static_cast<unsigned>(AvailableTokens);
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

    /// Target BPS implied by the shape
    unsigned TargetBPS = 0;

    /// Number of FEC recovery send tokens available so far
    int RecoverySendTokens = 0;

    /// FEC decrease count
    int RecoverySendCost = 0;

    /// Datagram UDP/IP header overhead in bytes, based on IPv4/6
    int UDPIPHeaderBytes = 0;

    /// Count of bytes sent in slow start.
    /// This does not (need to) include UDP/IP headers
    unsigned SlowStartSentBytes = 0;
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

    /// Timestamp right after processing the datagram.
    /// This is useful for checking if the application is falling behind in
    /// processing incoming data, allowing the congestion control to double
    /// as flow control for the connection.
    uint64_t PostProcessUsec = 0; ///< Microseconds

    /// Number of data bytes in the datagram (including headers).
    /// This is used for calculating send and receive bandwidth.
    unsigned Bytes = 0; ///< Bytes
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
// FastLossIndicator

/**
    FastLossIndicator

    For low/mid-rate streams, waiting for the statistics collection
    interval to end would take a while.  For example on WiFi there are
    large 100 ms lag spikes even though the delay may be 5 milliseconds.
    The BW statistics need to wait for those spikes, so it takes much
    longer to react to changes in the network, as a result.

    To react faster to congestion signaled by packetloss, a fast loss
    indicator used:

    (1) For low-rate data it checks if 2 datagrams have been lost or
    reordered within a span of 20 datagrams.  Or 3 datagrams in 10.
    This handles the general case of low-rate data where statistics
    are too slow to react.

    (2) For high-rate data it checks if more than 10% loss is detected
    within a span of 22 milliseconds.  This handles the case of links
    with high delay variance like WiFi, allowing Tonk to reduce its
    send rate faster than the statistics collection interval, which
    can be a quarter of a second in that case.
*/
class FastLossIndicator
{
public:
    FastLossIndicator();

    /// Update on datagram received.
    /// Returns true if datagram is out of sequence (reordered)
    bool UpdateIsReordered(const DatagramInfo& datagram);

    /// Reset the stats
    void ResetStats();

    /// Double loss event seen since the last Reset()?
    bool DoubleLoss = false;

    /// Set to the highest loss rate seen since reset
    float HighestLossRate = 0.f;

protected:
    /// Next nonce expected from peer
    NonceT NextExpectedNonce = 0;

    /// Last loss nonce
    NonceT LastLossNonce = 0;

    /// Two single losses within this threshold distance
    /// will also count as a DoubleLoss.
    static const uint64_t kDoubleLossThreshold = 10;

    /// Minimum interval before reporting a loss rate
    static const unsigned kMinIntervalUsec = 22 * 1000; ///< 22 ms

    /// Minimum count of datagrams received before reporting a loss
    static const unsigned kMinCount = 20;

    /// Start of interval in microseconds
    uint64_t IntervalStartUsec = 0;

    /// Number of bins to average over
    static const unsigned kBinCount = 2;

    /// Received count for each bin
    unsigned GotCount[kBinCount];

    /// Expected count of each bin
    unsigned ExpectedCount[kBinCount];
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
    At low rates we wait at least 250 ms or at least the OWD (whichever is
    smaller) and for at least two packets.
*/
class BandwidthEstimator
{
public:
    /// Update on datagram received.
    /// Returns true if statistics were updated
    bool UpdateOnDatagram(bool reordered, const DatagramInfo& datagram);

    /// Returns BPS that we suspect the peer was sending in the last interval
    unsigned GetAttemptedBPS() const
    {
        return LatestGoodputBPS + GuessedLossBPS;
    }

    /// Smoothed estimate of goodput
    unsigned SmoothedGoodputBPS = 0;

    /// Pretty accurate measure of BPS the sender is achieving over a small
    /// recent time interval.  If there is packetloss, this is a smaller rate
    /// than the sender actually sent.  GetAttemptedBPS() can be used to
    /// estimate the original send rate during loss.
    unsigned LatestGoodputBPS = 0;

    /// Guessed BW before packetloss
    unsigned GuessedLossBPS = 0;

    /// Smoothed estimate of maximum trip time
    unsigned SmoothedMaxTripUsec = 0;

    /// Smoothed estimate of minimum trip time
    unsigned SmoothedMinTripUsec = 0;

    /// PLR
    float LatestPLR = 0.f;

    /// Smoothed PLR
    float SmoothedPLR = 0.f;

    /// Get maximum recently seen PLR
    float GetMaxPLR() const
    {
        return LatestPLR > SmoothedPLR ? LatestPLR : SmoothedPLR;
    }

protected:
    /// Interval start
    DatagramInfo Start;

    /// Seeking a minimum value
    bool SeekingMinimum = true;

    /// Previous in-order datagram info
    DatagramInfo Previous;

    /// Bytes before Minimum
    unsigned IntervalBytes = 0;

    /// Datagram count in interval
    unsigned IntervalDatagramCount = 0;

    /// Count before Minimum
    unsigned IntervalInSeqCount = 0;

    /// Maximum seen so far in this interval
    unsigned IntervalMaxTripUsec = 0;

    /// Statistics tracking inter-packet gap (IPG)
    InterPacketGapEstimator InterPacketGap;


    /// Called when interval ends
    void OnInterval(unsigned intervalUsec);
};


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Receiver Side

/**
    There are several advantages to bandwidth control at the receiver:

    (1) All the data is available at the receiver.  The sender only sees
    acknowledgement messages coming back from the receiver, which are sent
    at a 2:1 ratio (at least) back through a lossy and unreliable channel.

    (2) Flow Control is directly incorporated into Bandwidth Control rather
    than being a separate algorithm with its own protocol.

    (3) Reactions to network changes can happen faster.  The receiver can
    inform the sender how much bandwidth to use with every single ACK, rather
    than the sender having to wait for a statistically significant change over
    multiple ACK messages before making a choice to change its bandwidth.

    (4) In the common case where the sender is a server hosting many clients,
    the computational load on the server is lighter since it does not need to
    do any data analysis; the sender only needs to run a datagram scheduler.


    Discussion:

    Having the congestion control algorithm run at the receiver is an unusual
    approach, which carries with it the two risks of a receiver misbehaving
    in order to either (a) DoS the sender or to (b) hog a channel.  The fact
    is that neither of these concerns have been addressed by any existing CC
    approaches to date.  For example (b) can be achieved with UDT, QUIC-BBR,
    TCP, etc by simply running 10+ parallel connections with a server and
    making parallel requests in order to take most of the available bandwidth
    -- a well known trick leveraged by download accelerators.

    Mitigating (a) is a matter of imposing bandwidth limits and/or
    incorporating schemes such as rolling hashes to prove to the sender that
    the receiver is getting data.  We do not attempt to solve (b).
*/

/// Receiver's bandwidth control algorithm
class ReceiverBandwidthControl
{
public:
    struct Dependencies
    {
        TimeSynchronizer* TimeSync;
        logger::Channel* Logger;
    };

    struct Parameters
    {
        /// Maximum delay allowed by application
        unsigned MaximumDelayMsec = 0;

        /// Is FEC enabled?
        bool EnableFEC = false;
    };

    /// Initialize bandwidth control
    void Initialize(const Dependencies& deps, const Parameters& params);

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

    /// Provides realtime feedback from receiver
    TONK_FORCE_INLINE BandwidthShape GetSenderShape() const
    {
        return Shape;
    }

    /// Get statistics for infrequent reporting
    ReceiverStatistics GetStatistics() const;

    /// Reset channel probe from 0 BPS
    void BeginSlowStart();

    /// Reset channel probe from mid-BPS
    void BeginQuickRateProbe();

protected:
    Dependencies Deps;
    Parameters Params;

    /// Estimator for bandwidth (BW)
    BandwidthEstimator Bandwidth;

    /// Fast loss indicator
    FastLossIndicator FastLoss;

    /// Bandwidth shape to return
    BandwidthShape Shape;

    /// Count of bytes sent in slow start.
    /// This does not (need to) include UDP/IP headers
    unsigned SlowStartSentBytes = 0;

    /// Time when slow-start began
    uint64_t SlowStartStartUsec = 0;

    /// Use statistics to probe for rate
    bool FastRateBaseProbe = true;

    /// Last probe BPS chosen by Update* functions
    unsigned LastProbeBPS = 0;

    /// Did BW estimate change since last update?
    bool UpdatedBW = false;

    //--------------------------------------------------------------------------
    // Update Function State
    //--------------------------------------------------------------------------

    /// Window to keep max BW samples.
    /// After this the network conditions may have changed
    static const uint64_t kMaxBWWindowUsec = 1000 * 1000; ///< 1 second

    /// Windowed maximum uncongested maximum goodput (in bytes per second).
    /// Uncongested meaning that this is only measured when PLR is lower,
    /// and it gets reset when PLR exceeds a threshold
    WinMaxUnsigned MaxUncongestedGoodput;

    void UpdateMaxUncongestedGoodput(uint64_t nowUsec)
    {
        MaxUncongestedGoodput.Update(
            Bandwidth.LatestGoodputBPS,
            nowUsec,
            kMaxBWWindowUsec);
    }

    /// Should the receiver send an ack immediately
    /// or wait for the next ack timer to expire?
    bool ShouldAckFast = false;

    /// Probing down to lower BW?
    bool ProbingDown = false;

    /// Time when rate change probe started
    uint64_t ProbeStartUsec = 0;


    /// Update bandwidth during slow-start
    void UpdateSlowStart(
        uint64_t nowUsec,
        unsigned& appBPS,
        unsigned& probeBPS);

    /// Update bandwidth during fast rate-based probe
    void UpdateFastRateProbe(
        uint64_t nowUsec,
        unsigned smoothedSendDelayUsec,
        unsigned& appBPS,
        unsigned& probeBPS);

    /// Update bandwidth in steady-state
    void UpdateSteadyState(
        uint64_t nowUsec,
        unsigned smoothedSendDelayUsec,
        unsigned& appBPS,
        unsigned& probeBPS);

    /// Update bandwidth in steady-state: While probing down
    void UpdateSteadyState_ProbeDown(
        uint64_t nowUsec,
        unsigned smoothedSendDelayUsec,
        unsigned& appBPS,
        unsigned& probeBPS);
};


//------------------------------------------------------------------------------
// Serializers

/// Represent a 32-bit integer with 16 bits using fixed point.
/// 5 bits of exponent, representing offsets of 0..20.
/// 11 bits of mantissa, providing a precision of 1/2048 = 0.048828125%.
/// The return value will decompress within 0.1% of the input word
TONK_FORCE_INLINE uint16_t FixedPointCompress(uint32_t word)
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

TONK_FORCE_INLINE uint32_t FixedPointDecompress(uint16_t fpword)
{
    return ((uint32_t)fpword & 2047) << ((uint32_t)fpword >> 11);
}


} // namespace tonk
