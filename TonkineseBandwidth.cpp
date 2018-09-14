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

#include "TonkineseBandwidth.h"

#include <math.h>

namespace tonk {

#ifdef TONK_ENABLE_VERBOSE_BANDWIDTH
    static logger::Channel ModuleLogger("BandwidthControl", MinimumLogLevel);
    #define TONK_VERBOSE_BANDWIDTH_LOG(...) ModuleLogger.Info(__VA_ARGS__);
#else
    static logger::Channel ModuleLogger("BandwidthControl", logger::Level::Silent);
    #define TONK_VERBOSE_BANDWIDTH_LOG(...) ;
#endif


//------------------------------------------------------------------------------
// BandwidthShape

// Convert FEC rate to byte
// Assumes input rate of 0.01 .. 0.51 (clamps output)
static uint8_t FECRateToByte(float rate)
{
    if (rate <= 0.01f) {
        return 0;
    }
    const int rateInt = static_cast<int>((rate - 0.01f) * 512.f + 0.5f);
    if (rateInt <= 0) {
        return 0;
    }
    else if (rateInt >= 255) {
        return 255;
    }
    else {
        return static_cast<uint8_t>(rateInt);
    }
}

// Convert byte to FEC rate
static float ByteToFECRate(unsigned byte)
{
    return 0.01f + (byte / 512.f);
}

void BandwidthShape::Compress(uint8_t* buffer) const
{
    uint8_t fecRateByte = FECRateToByte(FECRate);

    const uint32_t dwordAppBPS = (uint32_t)AppBPS;
    TONK_DEBUG_ASSERT(dwordAppBPS >= protocol::kMinimumBytesPerSecond);

    const uint16_t wordAppBPS = FixedPointCompress32to16(dwordAppBPS);

    siamese::WriteU16_LE(buffer, wordAppBPS);
    buffer[2] = fecRateByte;
}

void BandwidthShape::Decompress(const uint8_t* buffer)
{
    AppBPS = FixedPointDecompress16to32(siamese::ReadU16_LE(buffer));

    const uint8_t fecRateByte = buffer[2];
    FECRate = ByteToFECRate(fecRateByte & 0xfe);
}


//------------------------------------------------------------------------------
// ReceiverStatistics

void ReceiverStatistics::Compress(uint8_t* buffer) const
{
    uint8_t lossRateByte = 0;
    if (LossRate > 0.f)
    {
        const int lossRateInt = static_cast<int>(LossRate * 256.f + 0.5f);
        if (lossRateInt > 0)
        {
            if (lossRateInt >= 255) {
                lossRateByte = 255;
            }
            else {
                lossRateByte = static_cast<uint8_t>(lossRateInt);
            }
        }
    }
    buffer[0] = lossRateByte;

    siamese::WriteU16_LE(buffer + 1, FixedPointCompress32to16(GoodputBPS));
    siamese::WriteU16_LE(buffer + 3, FixedPointCompress32to16(TripUsec));
}

void ReceiverStatistics::Decompress(const uint8_t* buffer)
{
    LossRate = static_cast<int>(buffer[0]) / 256.f;
    GoodputBPS = FixedPointDecompress16to32(siamese::ReadU16_LE(buffer + 1));
    TripUsec = FixedPointDecompress16to32(siamese::ReadU16_LE(buffer + 3));
}


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Sender Side

void SenderBandwidthControl::Initialize(
    const Dependencies& deps,
    const Parameters& params)
{
    Deps = deps;
    Params = params;

    // Set initial FEC rate
    static const float kInitialFECRate = 0.01f;
    RecoverySendCost = static_cast<int>(kReliableRewardTokens / kInitialFECRate);

    Shape.AppBPS = protocol::kBandwidthBurstTokenLimit;

    // Set initial recovery timer
    const uint64_t nowUsec = siamese::GetTimeUsec();
    RecoveryTimer.SetTimeoutUsec(protocol::kInitialOWDUsec, Timer::Behavior::Restart);
    RecoveryTimer.Start(nowUsec);
}

void SenderBandwidthControl::OnReceiverFeedback(BandwidthShape shape)
{
    // Update recovery send cost
    RecoverySendCost = static_cast<int>(kReliableRewardTokens / shape.FECRate);

    // Bound shape
    if (shape.AppBPS < protocol::kMinimumBytesPerSecond) {
        shape.AppBPS = protocol::kMinimumBytesPerSecond;
    }
    else if (shape.AppBPS > Params.MaximumBPS && Params.MaximumBPS > 0) {
        shape.AppBPS = Params.MaximumBPS;
    }

    Shape = shape;
}

void SenderBandwidthControl::Squelch()
{
    //Deps.Logger->Debug("Squelch: Setting BPS to minimum");

    Shape.AppBPS = protocol::kMinimumBytesPerSecond;
}

void SenderBandwidthControl::RecalculateAvailableBytes(uint64_t nowUsec)
{
    TONK_DEBUG_ASSERT(Shape.AppBPS < 1 * 1000 * 1000 * 1000); // Will not fit in integer

    // Calculate the time since the last tick
    uint64_t tickIntervalUsec = nowUsec - LastIntervalStartUsec;
    LastIntervalStartUsec = nowUsec;

    // Calculate deficit from last tick
    int availableTokens = AvailableTokens;

    unsigned bps = Shape.AppBPS;

    // If we skipped a lot of time, take away all the tokens.
    // This also handles the first tick where the interval is undefined
    static const uint64_t kMaxTickIntervalUsec = 500 * 1000; // 500 ms
    if (tickIntervalUsec >= kMaxTickIntervalUsec) {
        availableTokens = 0;
    }

    // Limit the maximum number of skipped ticks to reward in one tick
    if (tickIntervalUsec > protocol::kSendTimerTickIntervalUsec * 2) {
        tickIntervalUsec = protocol::kSendTimerTickIntervalUsec * 2;
    }

    // Calculate how many bucket tokens were generated since the last tick
    const unsigned addedTokens = (unsigned)(((uint64_t)bps * tickIntervalUsec) / 1000000);

    availableTokens += addedTokens;

    // Limit available tokens to the bucket size plus some burst amount.
    // The burst amount matches the slow start burst size of CWND = 10 in TCP
    const int tokenLimit = static_cast<int>(addedTokens) + protocol::kBandwidthBurstTokenLimit;

    if (availableTokens > tokenLimit)
    {
        // Do not allow bursts that are too large.
        // If the application decides to send no data,
        // then avoid over-rewarding.  This prevents huge bursts of
        // data from being sent
        availableTokens = tokenLimit;
    }
    else if (availableTokens < -tokenLimit)
    {
        // Do not allow deficits that are too negative.
        // If the application decides to disregard the bandwidth limit,
        // then avoid over-penalizing.  This will allow sending data
        // within about two ticks after a burst at the longest
        availableTokens = -tokenLimit;
    }

    // Commit the available tokens
    AvailableTokens = availableTokens;

    // Limit the number of probe send tokens so they do not accumulate to a huge
    // number while the application is sending data, and later would lead to a
    // sustained probe while the application is not sending data.  This choice
    // is designed to allow for four ticks of probing beyond the application
    // data rate before reducing the rate
    if (ProbeSendTokens > tokenLimit * 4) {
        ProbeSendTokens = tokenLimit * 4;
    }
}

unsigned SenderBandwidthControl::CalculateRecoverySendCount(uint64_t nowUsec)
{
    TONK_DEBUG_ASSERT(RecoverySendCost > 0);
    if (RecoverySendTokens < RecoverySendCost)
    {
        // Check if we should send a recovery packet now
        if (!RecoveryTimer.IsExpired(nowUsec)) {
            return 0;
        }
        RecoverySendTokens = RecoverySendCost;
    }

    // Spend tokens for a FEC recovery packet x1
    unsigned sendCount = 1;
    RecoverySendTokens -= RecoverySendCost;
    if (RecoverySendTokens < RecoverySendCost) {
        goto SendRecovery;
    }

    // Spend tokens for a FEC recovery packet x2
    ++sendCount;
    RecoverySendTokens -= RecoverySendCost;
    if (RecoverySendTokens < RecoverySendCost) {
        goto SendRecovery;
    }

    // Spend all remaining tokens and limit to a maximum
    RecoverySendTokens = 0;
    sendCount += RecoverySendTokens / RecoverySendCost;
    if (sendCount > protocol::kRecoveryLimit) {
        sendCount = protocol::kRecoveryLimit;
    }

SendRecovery:
    // Reset the recovery timer for low rate streams e.g. < 1 MB/s with OWD=100 ms
    uint64_t nextTimeoutUsec = protocol::kInitialOWDUsec;
    if (Deps.TimeSync->IsSynchronized()) {
        nextTimeoutUsec = Deps.TimeSync->GetMinimumOneWayDelayUsec();
    }
    if (nextTimeoutUsec < protocol::kMinimumRecoveryIntervalUsec) {
        nextTimeoutUsec = protocol::kMinimumRecoveryIntervalUsec;
    }
    RecoveryTimer.SetTimeoutUsec(nextTimeoutUsec, Timer::Behavior::Restart);
    RecoveryTimer.Start(nowUsec);
    return sendCount;
}

unsigned SenderBandwidthControl::CalculateSendTimeMsec(unsigned bytes)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    const int availableTokens = AvailableTokens;

    // If there are already enough tokens:
    if (availableTokens >= static_cast<int>(bytes)) {
        return 0; // It would send immediately
    }

    // Accumulate available tokens:
    // If tokens is negative, this will add to the bytes request.
    // If tokens is positive, this will eliminate the data that would send now.
    bytes -= availableTokens;

    const unsigned bps = Shape.AppBPS;

    // If BPS is invalid:
    if (bps < protocol::kMinimumBytesPerSecond) {
        TONK_DEBUG_BREAK(); // Should never happen
        return protocol::kMinimumBytesPerSecond;
    }

    return static_cast<unsigned>((static_cast<uint64_t>(bytes) * 1000) / bps);
}


//------------------------------------------------------------------------------
// InterPacketGapEstimator

void InterPacketGapEstimator::OnDatagram(uint64_t receiveUsec)
{
    // Calculate time since last datagram was received (processed)
    const int64_t delta = (int64_t)(receiveUsec - LastReceiveUsec);

    // If time is rolling backwards:
    if (delta <= 0) {
        TONK_DEBUG_BREAK(); // Should not happen on any modern PC
        return;
    }

    // If there was not a huge period of silence:
    if (delta < (int64_t)protocol::kBWMaxIntervalUsec &&
        LastReceiveUsec != 0)
    {
        const unsigned ipgUsec = (unsigned)delta;

        // Update maximum seen in interval
        if (MaxUsec < ipgUsec) {
            MaxUsec = ipgUsec;
        }
    }

    LastReceiveUsec = receiveUsec;
}

void InterPacketGapEstimator::OnIntervalEnd()
{
    // If no interval was started:
    if (MaxUsec == 0) {
        return;
    }

    // Smooth the maximum values seen with EWMA
    SmoothedMaxUsec = (SmoothedMaxUsec * 7 + MaxUsec) / 8;

    // Indicate that no interval has started yet
    MaxUsec = 0;
}


//------------------------------------------------------------------------------
// BandwidthEstimator

bool BandwidthEstimator::UpdateOnDatagram(const DatagramInfo& datagram)
{
    // If we have not received any data yet:
    if (Start.Nonce == 0)
    {
        // Initialize the start to this datagram - We ignore its size because
        // there is no previous packet timestamp to use to measure bandwidth.
        Start = datagram;
        Previous = datagram;
        return false;
    }

    // Update IPG
    InterPacketGap.OnDatagram(datagram.ReceiveUsec);

    // Check if datagram is re-ordered
    const int64_t deltaNonce = (int64_t)(datagram.Nonce - NextExpectedNonce);
    const bool reordered = (deltaNonce < 0);

    // If datagram was received out of order:
    if (reordered)
    {
        // Count it for this interval but do not use it to end an interval
        IntervalDatagramBytes += datagram.DatagramBytes;
        //++IntervalDatagramCount;
        return false;
    }
    NextExpectedNonce = datagram.Nonce + 1;

    bool resultUpdated = false;

    // Find minimum trip time points:
    if (SeekingMinimum)
    {
        // If trip time increased, the Previous datagram was a local minimum:
        if (datagram.ReceiveSendDeltaTS24 > Previous.ReceiveSendDeltaTS24)
        {
            // Calculate receive interval
            const unsigned intervalUsec = (unsigned)(Previous.ReceiveUsec - Start.ReceiveUsec);
            TONK_DEBUG_ASSERT((int64_t)(Previous.ReceiveUsec - Start.ReceiveUsec) >= 0);

            // Check that the interval is long enough to avoid OS jitter.
            // Check that it exceeds the smoothed running maximum trip time,
            // which accounts for regular latency spikes experienced on WiFi.
            if ((IntervalInSeqCount >= protocol::kMinBWIntervalDatagrams &&
                 intervalUsec >= protocol::kMinBWIntervalUsec &&
                 intervalUsec >= InterPacketGap.Get() * 2) ||
                (intervalUsec >= protocol::kBWMaxIntervalUsec &&
                 Previous.Nonce > Start.Nonce + 2)) // Or timeout
            {
#ifdef TONK_ENABLE_VERBOSE_BANDWIDTH
                ModuleLogger.Info("BW Interval done: intervalUsec = ", intervalUsec, " IPG*2=", InterPacketGap.Get()*2, " IntervalInSeqCount=", IntervalInSeqCount);
#endif

                OnInterval(intervalUsec);
                resultUpdated = true; // Updated!
            }

            // Found the minimum, now start seeking a maximum
            SeekingMinimum = false;
            IntervalMaxTripUsec = datagram.NetworkTripUsec;
        }
    }
    // Else if trip time decreased:
    else if (datagram.ReceiveSendDeltaTS24 < Previous.ReceiveSendDeltaTS24) {
        SeekingMinimum = true;
    }
    // Else if this is the smallest trip time so far:
    else if (IntervalMaxTripUsec < datagram.NetworkTripUsec) {
        IntervalMaxTripUsec = datagram.NetworkTripUsec;
    }

    Previous = datagram;

    // Accumulate bytes following the first packet in the interval.
    // The packet that starts an interval belongs to the previous interval
    IntervalDatagramBytes += datagram.DatagramBytes;
    ++IntervalInSeqCount;
    //++IntervalDatagramCount;

    return resultUpdated;
}

void BandwidthEstimator::OnInterval(const unsigned intervalUsec)
{
    // Record timestamp for the burst
    Measurement.TimestampUsec = Previous.ReceiveUsec;

    {
        // Calculate receive BPS
        const uint64_t scaledBytes = (uint64_t)IntervalDatagramBytes * 1000000ULL;
        const unsigned receiveBPS = intervalUsec > 0 ? (unsigned)(scaledBytes / intervalUsec) : 0;
        Measurement.LatestProbeBPS = receiveBPS;
    }

    // Calculate send BPS
    //const Counter24 sendIntervalTS24 = Previous.SendTS24 - Start.SendTS24;
    //const unsigned sendIntervalUsec = sendIntervalTS24.ToUnsigned() << kTime23LostBits;
    //Measurement.LatestGoodputBPS = sendIntervalUsec > 0 ? (unsigned)(scaledBytes / sendIntervalUsec) : 0;

    // Calculate PLR
    const int expected = (int)(int64_t)(Previous.Nonce - Start.Nonce);
    const int lossCount = expected - (int)IntervalInSeqCount;
    TONK_DEBUG_ASSERT(lossCount >= 0);

    float plr = 0.f;
    if (expected > 0 && lossCount > 0) {
        plr = (unsigned)lossCount / (float)expected;
    }

    // Smooth the received PLR using EWMA
    Measurement.SmoothedPLR = (Measurement.SmoothedPLR * 7.f + plr) / 8.f;

    // Smooth the received BPS using EWMA
    if (Measurement.SmoothedGoodputBPS == 0) {
        Measurement.SmoothedGoodputBPS = Measurement.LatestProbeBPS;
    }
    else {
        Measurement.SmoothedGoodputBPS = (Measurement.SmoothedGoodputBPS * 7 + Measurement.LatestProbeBPS) / 8;
    }

    // Smooth the minimum trip time estimate
    const unsigned minTripUsec = Previous.NetworkTripUsec;
    if (Measurement.SmoothedMinTripUsec == 0) {
        Measurement.SmoothedMinTripUsec = minTripUsec;
    }
    else {
        Measurement.SmoothedMinTripUsec = (Measurement.SmoothedMinTripUsec * 7 + minTripUsec) / 8;
    }

    // Smooth the maximum trip time estimate
    const unsigned maxTripUsec = IntervalMaxTripUsec;
    if (Measurement.SmoothedMaxTripUsec == 0) {
        Measurement.SmoothedMaxTripUsec = maxTripUsec;
    }
    else {
        Measurement.SmoothedMaxTripUsec = (Measurement.SmoothedMaxTripUsec * 7 + maxTripUsec) / 8;
    }

    // Record the smallest ReceiveSendDeltaTS24
    Measurement.MinReceiveSendDeltaTS24 = Previous.ReceiveSendDeltaTS24;

    // Update statistics
    InterPacketGap.OnIntervalEnd();

    TONK_VERBOSE_BANDWIDTH_LOG(
        "ProbeBPS=", Measurement.LatestProbeBPS,
        " Goodput=", Measurement.SmoothedGoodputBPS,
        " MinTrip=", Previous.NetworkTripUsec,
        " Interval=", intervalUsec,
        " Bytes=", IntervalDatagramBytes,
        " Count=", IntervalInSeqCount,
        " PLR=", Measurement.SmoothedPLR,
        " AvgMin=", Measurement.SmoothedMinTripUsec,
        " AvgMax=", Measurement.SmoothedMaxTripUsec,
        " IPG=", InterPacketGap.Get());

    // Reset interval sampling
    Start = Previous;
    IntervalDatagramBytes = 0;
    IntervalInSeqCount = 0;
}


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Receiver Side

void ReceiverBandwidthControl::Initialize(const Dependencies& deps)
{
    Deps = deps;

    NextShape.AppBPS = protocol::kBandwidthBurstTokenLimit;
    NextShape.FECRate = TONK_MIN_FEC_RATE;

    ShouldAckFast = false;

    RecentOWD.Reset();
    RecentOWD_WindowUsec = 100 * 1000;

    MinOWD.Reset();
    MinOWD_WindowUsec = 1000 * 1000;

    RecentMaxBW.Reset();

    InSlowStart = true;
    Momentum = 1;
}

void ReceiverBandwidthControl::OnProcessedDatagram(const DatagramInfo& datagram)
{
    // Update BW estimate
    const bool updated = Bandwidth.UpdateOnDatagram(datagram);

    // Update minimum OWD
    MinOWD.Update(
        Bandwidth.Measurement.MinReceiveSendDeltaTS24,
        Bandwidth.Measurement.TimestampUsec,
        MinOWD_WindowUsec);

    // Update recent OWD
    RecentOWD.Update(
        Bandwidth.Measurement.MinReceiveSendDeltaTS24,
        Bandwidth.Measurement.TimestampUsec,
        RecentOWD_WindowUsec);

    // If bandwidth estimator has not updated yet:
    if (!updated) {
        return;
    }

    // Update recent max bandwidth
    RecentMaxBW.Update(
        Bandwidth.Measurement.LatestProbeBPS,
        Bandwidth.Measurement.TimestampUsec,
        RecentOWD_WindowUsec);

    // The minimum window must include a few timer ticks
    unsigned recent_window_usec = protocol::kSendTimerTickIntervalUsec * 6;

    // Must wait longer than the minimum OWD / 2
    const unsigned min_trip_usec = Bandwidth.Measurement.SmoothedMinTripUsec / 2;
    if (recent_window_usec < min_trip_usec) {
        recent_window_usec = min_trip_usec;
    }

    // Must wait longer than two (longest) inter-packet gaps
    const unsigned min_ipg2_usec = Bandwidth.InterPacketGap.Get() * 2;
    if (recent_window_usec < min_ipg2_usec) {
        recent_window_usec = min_ipg2_usec;
    }

    // Probe time must be longer than the window to be detected
    RecentOWD_WindowUsec = recent_window_usec;
}

ReceiverStatistics ReceiverBandwidthControl::GetStatistics() const
{
    ReceiverStatistics stats;
    stats.GoodputBPS = Bandwidth.Measurement.SmoothedGoodputBPS;

    // This must be the minimum so that the bandwidth controller on the other side uses a
    // minimum OWD to base RTT estimate on when planning to flush the bottleneck router queue.
    // If we over-estimate the RTT, then we may not fully flush before the next probe.
    // If we under-estimate the RTT, then we will not cause any issues.
    stats.TripUsec = Bandwidth.Measurement.SmoothedMinTripUsec;

    stats.LossRate = Bandwidth.Measurement.SmoothedPLR;
    return stats;
}

ReceiverBandwidthControl::Decision ReceiverBandwidthControl::UpdateCC(
    uint64_t nowUsec,
    unsigned smoothedSendDelayUsec)
{
    bool updated = false;


    // Estimate minimum RTT so we know how long we have to flush the queue we build up
    const unsigned owd_send_usec = smoothedSendDelayUsec;
    const unsigned owd_recv_usec = Bandwidth.Measurement.SmoothedMinTripUsec;
    unsigned min_rtt_usec = owd_send_usec + owd_recv_usec;
    if (min_rtt_usec <= 0) {
        min_rtt_usec = 100000; // Default until we have a full round trip
    }

    // Wait at least 8 round trips to find the minimum OWD
    MinOWD_WindowUsec = min_rtt_usec * 8;

    // Wait at least 2 seconds
    if (MinOWD_WindowUsec < 4000000) {
        MinOWD_WindowUsec = 4000000;
    }


    // Maximum recently seen bandwidth
    const unsigned maxBW = RecentMaxBW.GetBest();


    // In reaction to high PLR we should decrease rate.
    // In reaction to moderate PLR we should not increase rate.
    const float plr = Bandwidth.Measurement.SmoothedPLR;

    if (plr > TONKCC_CONGESTION_PLR)
    {
        // Reduce BW
        updated = (Momentum >= 0);
        Momentum = -1;
    }
    else
    {
        // Update queuing delay for each packet that arrives
        const Counter32 queue_delayTS24 = RecentOWD.GetBest() - MinOWD.GetBest();
        const unsigned queue_delay_usec = queue_delayTS24.ToUnsigned() << kTime23LostBits;

        // If queue delay is detected:
        if (queue_delay_usec > protocol::kDetectQueueDelayUsec)
        {
            // Reduce BW
            updated = (Momentum >= 0);
            Momentum = -1;
        }
        else
        {
            // Increase BW
            updated = (Momentum <= 0);
            Momentum = 1;
        }
    }


    // Time since last tick
    unsigned tickIntervalUsec = static_cast<unsigned>(nowUsec - LastTickUsec);
    if (tickIntervalUsec > protocol::kSendTimerTickIntervalUsec * 8) {
        tickIntervalUsec = protocol::kSendTimerTickIntervalUsec * 8;
    }
    LastTickUsec = nowUsec;

    if (InSlowStart)
    {
        if (Momentum < 0)
        {
            InSlowStart = false;
            Deps.Logger->Debug("Leaving slow-start");

            NextShape.AppBPS = (unsigned)(maxBW * 0.9f);
        }
        else
        {
            // If we have hit the previous target:
            if (maxBW >= (NextShape.AppBPS * 3 / 4)) {
                NextShape.AppBPS *= 2;
            }
        }
    }
    else
    {
        const unsigned bps = Bandwidth.Measurement.SmoothedGoodputBPS;

        // Rate expression selected by manually checking a fixed-rate simulation
        // from 125 KB/s, 250 KB/s, ..., 2 MB/s, 4 MB/s
        uint64_t rate = tickIntervalUsec * 1300;
        rate *= bps;
        rate /= protocol::kSendTimerTickIntervalUsec * 2000000ULL;
        unsigned bpsDelta = static_cast<unsigned>(rate);

        const unsigned owdUsec = Bandwidth.Measurement.SmoothedMinTripUsec;

        // If there is moderate or high latency, then percentage jumps help a lot
        if (owdUsec > 100000)
        {
            // Skip ahead in the right direction
            if (updated) {
                if (Momentum < 0) {
                    NextShape.AppBPS = (unsigned)(NextShape.AppBPS * 0.9f);
                }
                else {
                    NextShape.AppBPS = (unsigned)(NextShape.AppBPS * 1.1f);
                }
            }

            bpsDelta *= 100000;
            bpsDelta /= owdUsec;
        }

        // If in congestion:
        if (Momentum < 0)
        {
            if (NextShape.AppBPS > bpsDelta * 2) {
                NextShape.AppBPS -= bpsDelta * 2;
            }
        }
        // If caution is not raised:
        else if (maxBW >= (NextShape.AppBPS * 3 / 4))
        {
            NextShape.AppBPS += bpsDelta;
        }
    }

    return updated ? Decision::PushUpdateInAck : Decision::NoChange;
}

void ReceiverBandwidthControl::UpdateSendShape(
    uint64_t nowUsec,
    unsigned smoothedSendDelayUsec)
{
    // If delay feedback has not been received, use our own estimate
    if (smoothedSendDelayUsec <= 0) {
        smoothedSendDelayUsec = Bandwidth.Measurement.SmoothedMaxTripUsec;
    }

    // Update CC
    const Decision decision = UpdateCC(nowUsec, smoothedSendDelayUsec);

    // Minimum BPS
    if (NextShape.AppBPS < protocol::kMinimumBytesPerSecond) {
        NextShape.AppBPS = protocol::kMinimumBytesPerSecond;
    }

    // Send FEC datagrams proportional to the loss rate
    NextShape.FECRate = Bandwidth.Measurement.SmoothedPLR * 2.f;

    if (decision == Decision::PushUpdateInAck) {
        // Ask connection timer tick to send an ACK with our new bandwidth shape
        ShouldAckFast = true;
    }
}


} // namespace tonk
