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

/*
    Realtime congestion control approaches from RMCAT working group:

    Blog overview: https://blog.mozilla.org/webrtc/what-is-rmcat-congestion-control/
    Google: https://c3lab.poliba.it/images/6/65/Gcc-analysis.pdf
    Google: https://tools.ietf.org/pdf/draft-ietf-rmcat-gcc-02.pdf
    NADA: https://tools.ietf.org/pdf/draft-ietf-rmcat-nada-06.pdf
    SCReAM: https://www.rfc-editor.org/rfc/pdfrfc/rfc8298.txt.pdf
*/

/*
    Ideas for packetloss prediction for better FEC scheduling:

    ARIMA: http://www.jatit.org/volumes/Vol66No1/30Vol66No1.pdf
    HDAX: http://crpit.com/confpapers/CRPITV101Homayounfard.pdf
    PARSE BASIS MODEL for PLR for FEC: http://alumnus.caltech.edu/~amir/PLR.pdf
*/

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

void BandwidthShape::Compress(uint8_t* buffer) const
{
    uint8_t fecRateByte = 0;
    if (FECRate > 0.f)
    {
        const int FECRate_int = static_cast<int>((FECRate - 0.01f) * 512.f + 0.5f);
        if (FECRate_int > 0)
        {
            if (FECRate_int >= 255) {
                fecRateByte = 255;
            }
            else {
                fecRateByte = static_cast<uint8_t>(FECRate_int);
            }
        }
    }

    unsigned word;
    if (InSlowStart)
    {
        fecRateByte |= 1;
        word = SlowStartBytes;
        TONK_DEBUG_ASSERT(word >= protocol::kBandwidthBurstTokenLimit);
    }
    else
    {
        fecRateByte &= 0xfe;
        word = AppBPS;
        TONK_DEBUG_ASSERT(word >= protocol::kMinimumBytesPerSecond);
    }

    buffer[0] = fecRateByte;
    siamese::WriteU16_LE(buffer + 1, FixedPointCompress(word));
}

void BandwidthShape::Decompress(const uint8_t* buffer)
{
    const uint8_t fecRateByte = buffer[0];

    InSlowStart = (fecRateByte & 1) != 0;
    FECRate = static_cast<int>(fecRateByte & 0xfe) / 512.f + 0.01f;

    const uint32_t word = FixedPointDecompress(siamese::ReadU16_LE(buffer + 1));
    if (InSlowStart)
    {
        // We cannot let WindowBytes go negative because it fills AvailableTokens
        TONK_DEBUG_ASSERT(word < (unsigned)0x80000000);
        SlowStartBytes = word & 0x7fffffff;
        AppBPS = protocol::kSlowStartBPS;
    }
    else
    {
        SlowStartBytes = 0;
        AppBPS = word;
    }
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

    siamese::WriteU16_LE(buffer + 1, FixedPointCompress(GoodputBPS));
    siamese::WriteU16_LE(buffer + 3, FixedPointCompress(TripUsec));
}

void ReceiverStatistics::Decompress(const uint8_t* buffer)
{
    LossRate = static_cast<int>(buffer[0]) / 256.f;
    GoodputBPS = FixedPointDecompress(siamese::ReadU16_LE(buffer + 1));
    TripUsec = FixedPointDecompress(siamese::ReadU16_LE(buffer + 3));
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

    // Set default datagram header overhead to IPv4
    UDPIPHeaderBytes = protocol::kUDPIPV4_HeaderBytes;

    // Set initial recovery timer
    const uint64_t nowUsec = siamese::GetTimeUsec();
    RecoveryTimer.SetTimeoutUsec(protocol::kInitialOWDUsec, Timer::Behavior::Restart);
    RecoveryTimer.Start(nowUsec);
}

void SenderBandwidthControl::OnReceiverFeedback(BandwidthShape shape)
{
    // Update recovery send cost
    RecoverySendCost = static_cast<int>(kReliableRewardTokens / shape.FECRate);

    // Bound and store shape
    if (shape.AppBPS < protocol::kMinimumBytesPerSecond) {
        shape.AppBPS = protocol::kMinimumBytesPerSecond;
    }
    else if (shape.AppBPS > Params.MaximumBPS && Params.MaximumBPS > 0) {
        shape.AppBPS = Params.MaximumBPS;
    }
    Shape = shape;

    // Set target rate > app rate
    TargetBPS = (unsigned)(shape.AppBPS * (1.f + shape.FECRate));
    if (TargetBPS < protocol::kMinimumBytesPerSecond) {
        TargetBPS = protocol::kMinimumBytesPerSecond;
    }
    else if (TargetBPS > Params.MaximumBPS && Params.MaximumBPS > 0) {
        TargetBPS = Params.MaximumBPS;
    }

    // DO NOT CHECK THIS IN - Limit c2s data to focus on s2c WIP
    //TargetBPS = Shape.AppBPS = protocol::kMinimumBytesPerSecond;
}

void SenderBandwidthControl::Squelch()
{
    if (Shape.AppBPS != protocol::kMinimumBytesPerSecond) {
        Deps.Logger->Debug("Squelch: Setting BPS to minimum");
    }
    Shape.AppBPS = protocol::kMinimumBytesPerSecond;
    TargetBPS = protocol::kMinimumBytesPerSecond;
    Shape.FECRate = 0.01f;
}

void SenderBandwidthControl::RecalculateAvailableBytes(uint64_t nowUsec)
{
    // Handle self-clocked/slow-start/windowed-mode:
    if (Shape.InSlowStart)
    {
        // Accumulate sent bytes so far
        if (Shape.SlowStartBytes > SlowStartSentBytes) {
            AvailableTokens = Shape.SlowStartBytes - SlowStartSentBytes;
        }
        else {
            AvailableTokens = 0;
        }

        return;
    }

    TONK_DEBUG_ASSERT(TargetBPS < 1 * 1000 * 1000 * 1000); // Will not fit in integer

    // Calculate the time since the last tick
    uint64_t tickIntervalUsec = nowUsec - LastIntervalStartUsec;
    LastIntervalStartUsec = nowUsec;

    // Calculate deficit from last tick
    int availableTokens = AvailableTokens;

    // If we skipped a lot of time, take away all the tokens.
    // This also handles the first tick where the interval is undefined
    static const uint64_t kMaxTickIntervalUsec = 500 * 1000; // 500 ms
    if (tickIntervalUsec >= kMaxTickIntervalUsec) {
        availableTokens = 0;
    }

    // Calculate how many bucket tokens were generated since the last tick
    if (tickIntervalUsec > protocol::kSendTimerTickIntervalUsec * 4) {
        tickIntervalUsec = protocol::kSendTimerTickIntervalUsec;
    }
    const unsigned addedTokens = (unsigned)(((uint64_t)TargetBPS * tickIntervalUsec) / 1000000);

    // Limit available tokens to the bucket size plus some burst amount.
    // The burst amount matches the slow start burst size of CWND = 10 in TCP
    availableTokens += addedTokens;
    if (availableTokens > (int)addedTokens + protocol::kBandwidthBurstTokenLimit) {
        availableTokens = (int)addedTokens + protocol::kBandwidthBurstTokenLimit;
    }

    // Commit the available tokens
    AvailableTokens = availableTokens;
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

    // Some optimized unrolling for 1, 2 packet common cases:
    unsigned sendCount = 1;

    // Spend tokens for a FEC recovery packet
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
    if (availableTokens >= (int)bytes) {
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

    return (unsigned)(((uint64_t)bytes * 1000) / bps);
}


//------------------------------------------------------------------------------
// FastLossIndicator

FastLossIndicator::FastLossIndicator()
{
    for (unsigned i = 0; i < kBinCount; ++i)
    {
        GotCount[i] = 0;
        ExpectedCount[i] = 0;
    }
}

bool FastLossIndicator::UpdateIsReordered(const DatagramInfo& datagram)
{
    const int64_t deltaNonce = (int64_t)(datagram.Nonce - NextExpectedNonce);

    // If datagram is reordered:
    if (deltaNonce < 0) {
        return true;
    }

    NextExpectedNonce = datagram.Nonce + 1;

    // If at least one loss occurred:
    if (deltaNonce >= 1)
    {
        // If two losses occurred:
        if (deltaNonce >= 2) {
            DoubleLoss = true;
        }
        else
        {
            // Negative values will wrap high here
            const uint64_t deltaLastLoss = (uint64_t)(datagram.Nonce - LastLossNonce);
            LastLossNonce = datagram.Nonce;

            // Count two single losses in a small span as a double loss
            if (deltaLastLoss < kDoubleLossThreshold) {
                DoubleLoss = true;
            }
        }
    }

    // Accumulate data
    GotCount[0]++;
    ExpectedCount[0] += (unsigned)deltaNonce + 1;

    // If interval has ended:
    if (GotCount[0] >= kMinCount &&
        (int64_t)(datagram.ReceiveUsec - IntervalStartUsec) >= kMinIntervalUsec)
    {
        IntervalStartUsec = datagram.ReceiveUsec;

        // Average over the last kBinCount bins
        unsigned got = GotCount[0], expected = ExpectedCount[0];
        for (unsigned i = kBinCount - 1; i > 0; --i)
        {
            got += GotCount[i];
            expected += ExpectedCount[i];

            GotCount[i] = GotCount[i - 1];
            ExpectedCount[i] = ExpectedCount[i - 1];
        }
        GotCount[0] = 0;
        ExpectedCount[0] = 0;

        // Update loss rate
        TONK_DEBUG_ASSERT(got <= expected);
        const float lossRate = (expected - got) / (float)expected;
        if (HighestLossRate < lossRate) {
            HighestLossRate = lossRate;
        }
    }

    return false;
}

void FastLossIndicator::ResetStats()
{
    DoubleLoss = false;
    LastLossNonce = 0;
    HighestLossRate = 0.f;
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

bool BandwidthEstimator::UpdateOnDatagram(bool reordered, const DatagramInfo& datagram)
{
    // If we have not received any data yet:
    if (Start.Nonce == 0)
    {
        // Initialize the start to this datagram - We ignore its size because
        // there is no previous packet timestamp to use to measure bandwidth.
        Start = datagram;
        Previous = datagram;
        return true;
    }

    // Update IPG
    InterPacketGap.OnDatagram(datagram.ReceiveUsec);

    // If datagram was received out of order:
    if (reordered)
    {
        // Count it for this interval but do not use it to end an interval
        IntervalBytes += datagram.Bytes;
        ++IntervalDatagramCount;
        return false;
    }

    bool resultUpdated = false;

    // Find minimum trip time points:
    const unsigned tripUsec = datagram.NetworkTripUsec;
    if (SeekingMinimum)
    {
        // If trip time increased, the Previous datagram was a local minimum:
        if (tripUsec > Previous.NetworkTripUsec)
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
                OnInterval(intervalUsec);
                resultUpdated = true; // Updated!
            }

            // Found the minimum, now start seeking a maximum
            SeekingMinimum = false;
            IntervalMaxTripUsec = tripUsec;
        }
    }
    else if (tripUsec < Previous.NetworkTripUsec) {
        SeekingMinimum = true;
    }
    else if (tripUsec > IntervalMaxTripUsec) {
        IntervalMaxTripUsec = tripUsec;
    }

    Previous = datagram;

    // Accumulate bytes following the first packet in the interval.
    // The packet that starts an interval belongs to the previous interval
    IntervalBytes += datagram.Bytes;
    ++IntervalInSeqCount;
    ++IntervalDatagramCount;

    return resultUpdated;
}

void BandwidthEstimator::OnInterval(unsigned intervalUsec)
{
    // Calculate receive BPS
    const uint64_t scaledBytes = (uint64_t)IntervalBytes * 1000000;
    const unsigned receiveBPS = intervalUsec > 0 ? (unsigned)(scaledBytes / intervalUsec) : 0;

    // Calculate send BPS
    const Counter24 sendIntervalTS24 = Previous.SendTS24 - Start.SendTS24;
    const unsigned sendIntervalUsec = sendIntervalTS24.ToUnsigned() << kTime23LostBits;
    LatestGoodputBPS = sendIntervalUsec > 0 ? (unsigned)(scaledBytes / sendIntervalUsec) : 0;

    // Calculate PLR
    const int expected = (int)(int64_t)(Previous.Nonce - Start.Nonce);
    const int lossCount = expected - (int)IntervalInSeqCount;
    TONK_DEBUG_ASSERT(lossCount >= 0);

    if (expected > 0 &&
        lossCount > 0)
    {
        LatestPLR = (unsigned)lossCount / (float)expected;

        // Guesstimate how much BW was lost to packetloss
        const uint64_t scaledMaybeBytes = (uint64_t)(scaledBytes * lossCount) / IntervalDatagramCount;
        GuessedLossBPS = sendIntervalUsec > 0 ? (unsigned)(scaledMaybeBytes / sendIntervalUsec) : 0;
    }
    else
    {
        LatestPLR = 0;
        GuessedLossBPS = 0;
    }

    // Smooth the received PLR using EWMA
    SmoothedPLR = (SmoothedPLR * 7.f + LatestPLR) / 8.f;

    // Smooth the receive BPS using EWMA
    if (SmoothedGoodputBPS == 0) {
        SmoothedGoodputBPS = LatestGoodputBPS;
    }
    else {
        SmoothedGoodputBPS = (SmoothedGoodputBPS * 7 + LatestGoodputBPS) / 8;
    }

    // Smooth the minimum trip time estimate - Rough
    if (SmoothedMinTripUsec == 0) {
        SmoothedMinTripUsec = Previous.NetworkTripUsec;
    }
    else {
        SmoothedMinTripUsec = (SmoothedMinTripUsec * 7 + Previous.NetworkTripUsec) / 8;
    }

    // Smooth the maximum trip time estimate - Rough
    if (SmoothedMaxTripUsec == 0) {
        SmoothedMaxTripUsec = IntervalMaxTripUsec;
    }
    else {
        SmoothedMaxTripUsec = (SmoothedMaxTripUsec * 7 + IntervalMaxTripUsec) / 8;
    }

    // Update statistics
    InterPacketGap.OnIntervalEnd();

    TONK_VERBOSE_BANDWIDTH_LOG("RecvBPS=", receiveBPS,
        " Goodput=", LatestGoodputBPS,
        " MinTrip=", Previous.NetworkTripUsec,
        " Interval=", intervalUsec,
        " Bytes=", IntervalBytes,
        " Count=", IntervalInSeqCount,
        " PLR=", SmoothedPLR,
        " AvgMin=", SmoothedMinTripUsec,
        " AvgMax=", SmoothedMaxTripUsec,
        " IPG=", InterPacketGap.Get());

    // Reset interval sampling
    Start = Previous;
    IntervalBytes = 0;
    IntervalInSeqCount = 0;
}


//------------------------------------------------------------------------------
// Tonk Bandwidth Control : Receiver Side

void ReceiverBandwidthControl::Initialize(const Dependencies& deps, const Parameters& params)
{
    Deps = deps;
    Params = params;

    Shape.AppBPS = protocol::kBandwidthBurstTokenLimit;
    Shape.FECRate = TONK_MIN_FEC_RATE;

    // In slow start initially
    Shape.InSlowStart = true;
    Shape.SlowStartBytes = protocol::kBandwidthBurstTokenLimit;
    SlowStartSentBytes = protocol::kBandwidthBurstTokenLimit;
    SlowStartStartUsec = siamese::GetTimeUsec();

    LastProbeBPS = 0;
    ShouldAckFast = false;
    UpdatedBW = false;
    FastRateBaseProbe = false;
    MaxUncongestedGoodput.Reset();
}

void ReceiverBandwidthControl::OnProcessedDatagram(const DatagramInfo& datagram)
{
    // Update fast loss detector
    const bool reordered = FastLoss.UpdateIsReordered(datagram);

    // Update BW estimate
    if (Bandwidth.UpdateOnDatagram(reordered, datagram)) {
        UpdatedBW = true;
    }

    // Accumulate number of bytes sent during slow-start
    SlowStartSentBytes += datagram.Bytes;
}

void ReceiverBandwidthControl::BeginSlowStart()
{
    // If already in slow start:
    if (Shape.InSlowStart) {
        return;
    }

    Shape.InSlowStart = true;
    Shape.SlowStartBytes = protocol::kBandwidthBurstTokenLimit;
    SlowStartSentBytes = protocol::kBandwidthBurstTokenLimit;
    SlowStartStartUsec = siamese::GetTimeUsec();
    FastRateBaseProbe = false;

    // Reset app BPS to initial rate
    Shape.AppBPS = protocol::kSlowStartBPS;

    ModuleLogger.Info("BW: Beginning slow-start");
}

void ReceiverBandwidthControl::BeginQuickRateProbe()
{
    // If already in a probe:
    if (Shape.InSlowStart || FastRateBaseProbe) {
        return;
    }

    FastRateBaseProbe = true;
    Shape.AppBPS /= 2; // Cut bandwidth in half

    if (Shape.AppBPS < protocol::kMinimumBytesPerSecond) {
        Shape.AppBPS = protocol::kMinimumBytesPerSecond;
    }

    ModuleLogger.Info("BW: Beginning quick rate probe");
}

ReceiverStatistics ReceiverBandwidthControl::GetStatistics() const
{
    ReceiverStatistics stats;
    stats.GoodputBPS = Bandwidth.SmoothedGoodputBPS;
    stats.TripUsec = Bandwidth.SmoothedMaxTripUsec;
    stats.LossRate = Bandwidth.SmoothedPLR;
    return stats;
}

void ReceiverBandwidthControl::UpdateSendShape(
    uint64_t nowUsec,
    unsigned smoothedSendDelayUsec)
{
    ShouldAckFast = false;

    // If delay feedback has not been received, use our own estimate
    if (smoothedSendDelayUsec <= 0) {
        smoothedSendDelayUsec = Bandwidth.SmoothedMaxTripUsec;
    }

    unsigned appBPS = Shape.AppBPS, probeBPS = LastProbeBPS;
    TONK_DEBUG_ASSERT(Shape.InSlowStart || appBPS <= probeBPS);

    // Update based on the current mode
    if (Shape.InSlowStart) {
        UpdateSlowStart(nowUsec, appBPS, probeBPS);
    }
    else if (FastRateBaseProbe) {
        UpdateFastRateProbe(nowUsec, smoothedSendDelayUsec, appBPS, probeBPS);
    }
    else {
        UpdateSteadyState(nowUsec, smoothedSendDelayUsec, appBPS, probeBPS);
    }

    TONK_DEBUG_ASSERT(Shape.InSlowStart || appBPS <= probeBPS);
    TONK_DEBUG_ASSERT(probeBPS < 1 * 1000 * 1000 * 1000); // Will not fit in integer

    /*
        Send (at minimum) twice the detected loss rate in FEC datagrams.

        Based on my simulations for Cauchy Caterpillar, it seems like 2x PLR
        is a good operation region for Sliding Window (Streaming) FEC code.

        In those tests, if the encoder fails to provide enough redundancy,
        then the decoder has a poor chance of recovering and the FEC code does
        not help much.  Cauchy Caterpillar has a short window so what works
        well for it will also work well for Siamese if the goal is low latency.

        Siamese is different because its window can get much longer (up to 16K),
        so it is called an Infinite-Length Sliding Window (Streaming) FEC code.
        If a burst loss occurs, then recovery data will pile up until it
        overcomes the loss.  So recovery data is only rarely wasted.

        Otherwise, the situation is the same as Cauchy Caterpillar, and sending
        2x PLR for FEC rate is a similarly good idea.
    */
    const float minFECRate = Bandwidth.SmoothedPLR * 2.f;

    // If still in slow-start
    if (Shape.InSlowStart)
    {
        // Update the slow start bytes in bandwidth shape
        Shape.SlowStartBytes = SlowStartSentBytes;

        // For slow-start, the FEC rate is exact
        Shape.FECRate = minFECRate;

        // App BPS is unused in slow-start
        Shape.AppBPS = 0;
    }
    else
    {
        // Update BPS
        if (appBPS < protocol::kMinimumBytesPerSecond)
        {
            appBPS = protocol::kMinimumBytesPerSecond;
            probeBPS = protocol::kMinimumBytesPerSecond;
        }

        float fecRate = minFECRate;

        // If FEC is enabled:
        if (Params.EnableFEC)
        {
            // Calculate new FEC rate
            fecRate = probeBPS / (float)appBPS - 1.f;
            if (fecRate < minFECRate) {
                fecRate = minFECRate; // At least enough for FEC
            }
        }

        if (fecRate > 0.5f) {
            fecRate = 0.5f; // Cannot send more than 50%
        }

#if 0
        // If FEC bandwidth is not enough to probe, use app data instead:
        const unsigned fecBPS = static_cast<unsigned>(appBPS * fecRate);
        if (fecBPS < probeBPS) {
            appBPS += probeBPS - fecBPS;
        }
#endif

        Shape.FECRate = fecRate;
        Shape.AppBPS = appBPS;

        // TODO: REMOVE THIS
        ModuleLogger.Info("Shape update: Shape.AppBPS=", Shape.AppBPS, " Shape.FECRate=", Shape.FECRate);

#ifdef TONK_DEBUG
        // Sanity test for shape compression
        uint8_t buffer[BandwidthShape::kCompressBytes];
        Shape.Compress(buffer);
        BandwidthShape testShape;
        testShape.Decompress(buffer);
        unsigned approx = (unsigned)(testShape.AppBPS * (1.f + testShape.FECRate));
        unsigned expected = (unsigned)(Shape.AppBPS * (1.f + Shape.FECRate));
        float err = 1.f - approx / (float)expected;
        TONK_DEBUG_ASSERT(err < 0.005f);
#endif
    }

    UpdatedBW = false;
    LastProbeBPS = probeBPS;
}

/**
    UpdateSteadyState()

    This estimates the amount of bandwidth available to the remote
    application, and how much overhead should be devoted to FEC.

    This function operates in the steady-state of a connection,
    after a loss indicator has been detected, indicating that the
    current send rate is near the channel limit.  The goal is to
    operate near the limit without incurring additional loss or
    delay, while continuously probing for the availability of
    additional bandwidth.
*/

/// Backoff the given BPS in reaction to loss
static void LossBackoffBW(
    unsigned bps, float plr,
    unsigned& appBPS_out,
    unsigned& probeBPS_out)
{
    TONK_DEBUG_ASSERT(plr >= 0.f && plr <= 1.f);

    // Back off the given base BPS value by PLR/2
    const float backoffFactor = 1.f - 0.5f * plr;

    // Back off probe rate proportionally to loss rate
    probeBPS_out = (unsigned)(bps * backoffFactor);

    // Back off app rate even further proactively
    appBPS_out = (unsigned)(probeBPS_out * backoffFactor);

    TONK_DEBUG_ASSERT(probeBPS_out >= appBPS_out);
}

/// Target the given BPS send rate but back off the app by PLR
static void ProbeBackoffAppBW(
    unsigned bps, float plr,
    unsigned& appBPS_out,
    unsigned& probeBPS_out)
{
    TONK_DEBUG_ASSERT(plr >= 0.f && plr <= 1.f);

    // Back off the given base BPS value by PLR/2
    const float backoffFactor = 1.f - 0.5f * plr;

    // Back off app rate based on PLR
    probeBPS_out = bps;
    appBPS_out = (unsigned)(bps * backoffFactor);

    TONK_DEBUG_ASSERT(probeBPS_out >= appBPS_out);
}

/**
    There are a few bandwidth estimates available for congestion control:

    Bandwidth.LatestGoodput:

        This is the latest measurement of goodput from peer, meaning
        the receive data rate after packet losses.  It is not a good
        measure of current channel capacity because it is dependent
        on the app to generate consistently-high data rates.

    Bandwidth.GetAttemptedBPS():

        This is a guesstimate at the data rate sent by the peer before
        packet losses, which assumes the lost packets are all the same
        size as the average sized packet received.

    Bandwidth.SmoothedGoodput:

        This is a low-pass (EWMA) filtered measure of the goodput
        over time, which is a pretty good indicator of what the
        channel has been able to support recently.

    MaxUncongestedGoodput.GetBest():

        This is the largest Bandwidth.LatestGoodput value seen since
        the last high loss event, which is a good indicator of the
        limits of what the channel can provide.
        This may be 0, so it should be checked before use.
*/

void ReceiverBandwidthControl::UpdateSteadyState(
    uint64_t nowUsec,
    unsigned smoothedSendDelayUsec,
    unsigned& appBPS,
    unsigned& probeBPS)
{
    // If a fast loss indicator is triggered while not probing down:
    if (!ProbingDown &&
        FastLoss.HighestLossRate > TONK_HIGH_PLR_THRESHOLD)
    {
        // Base BPS off the filtered goodput
        const unsigned bps = Bandwidth.SmoothedGoodputBPS;

        // Back off BW based on loss indicator
        LossBackoffBW(
            bps,
            FastLoss.HighestLossRate,
            appBPS,
            probeBPS);
        ShouldAckFast = true;

        // Now probing down
        ProbingDown = true;
        ProbeStartUsec = nowUsec;

        // Reset goodput since network conditions changed
        MaxUncongestedGoodput.Reset();

        ModuleLogger.Info("BW: FastLoss detected High PLR during steady-state.  Reducing rate and probing down. PLR=",
            FastLoss.HighestLossRate, " appBPS=", appBPS, " probeBPS=", probeBPS);

        return;
    }

    // The rest of these checks only run if there is a new bandwidth estimate
    if (!UpdatedBW) {
        return;
    }

    // If probing down for lower PLR:
    if (ProbingDown)
    {
        UpdateSteadyState_ProbeDown(
            nowUsec,
            smoothedSendDelayUsec,
            appBPS,
            probeBPS);

        return;
    }

    // If high loss was detected during Cruise/ProbeUp:
    const float plr = Bandwidth.SmoothedPLR;
    if (plr > TONK_HIGH_PLR_THRESHOLD)
    {
        // Base BPS off the filtered goodput
        const unsigned bps = Bandwidth.SmoothedGoodputBPS;

        // Reduce rate proportional to overuse
        LossBackoffBW(
            bps,
            plr, // Use aggregate PLR
            appBPS,
            probeBPS);
        ShouldAckFast = true;

        // Start probing down
        ProbingDown = true;
        ProbeStartUsec = nowUsec;

        // After overuse, re-acquire the max uncongested goodput
        MaxUncongestedGoodput.Reset();

        ModuleLogger.Info("BW: High PLR during steady-state. PLR=",
            plr, " appBPS=", appBPS, " SmoothedGoodput=", bps);

        TONK_DEBUG_ASSERT(probeBPS >= appBPS);

        return;
    }

    // Store the received rate as the maximum seen without loss
    UpdateMaxUncongestedGoodput(nowUsec);

    // If attempted BPS is near/above probe rate and PLR is low:
    const unsigned attemptedBPS = Bandwidth.GetAttemptedBPS();
    if (attemptedBPS > (unsigned)(probeBPS * 0.975) &&
        Bandwidth.GetMaxPLR() < TONK_LOW_PLR_THRESHOLD)
    {
        // Note start of probe time
        ProbeStartUsec = nowUsec;

        // Use larger of best goodput seen so far and last probe rate
        unsigned bps = MaxUncongestedGoodput.GetBest();
        if (bps < probeBPS) {
            bps = probeBPS;
        }

        // App can now send at the probe rate
        appBPS = bps;

        // Probe at 5% higher rate with FEC
        probeBPS = (unsigned)(bps * 1.05f);

        // Send rate update in ack immediately
        ShouldAckFast = true;

        ModuleLogger.Info("BW Cruise: Low-PLR; Target Hit!; Probing higher!" \
            " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS);

        return;
    }

    // Note that negative values overflow high
    const uint64_t elapsedUsec = nowUsec - ProbeStartUsec;
    // Check if the peer has failed to adapt within expected time.
    // This usually means the peer is app-limited for available bandwidth
    // but it might also mean that it has been force-sending a lot of data.
    unsigned propagationUsec = smoothedSendDelayUsec + Bandwidth.SmoothedMaxTripUsec * 2;
    if (propagationUsec < protocol::kMinBWIntervalUsec * 2) {
        propagationUsec = protocol::kMinBWIntervalUsec * 2;
    }
    const bool probeComplete = elapsedUsec > propagationUsec;

    // If probe round is complete without the peer generating the probe rate we requested:
    if (probeComplete)
    {
        // Start a new interval
        ProbeStartUsec = nowUsec;

        // Cruise for the next interval to collect more data
        unsigned bps = MaxUncongestedGoodput.GetBest();

        // Do not allow the bps to drop below the probe rate when PLR is low.
        // The application may not be producing enough traffic to probe.
        if (bps < probeBPS) {
            bps = probeBPS;
        }

        // Back off the app bw under the probe rate based on PLR
        ProbeBackoffAppBW(
            bps,
            Bandwidth.SmoothedPLR,
            appBPS,
            probeBPS);
        ShouldAckFast = true;

        ModuleLogger.Info("BW Cruise: Low-PLR; Probe Complete; Timeout -> Cruising..." \
            " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS);

        return;
    }
}

void ReceiverBandwidthControl::UpdateSteadyState_ProbeDown(
    uint64_t nowUsec,
    unsigned smoothedSendDelayUsec,
    unsigned& appBPS,
    unsigned& probeBPS)
{
    // Take the latest PLR rather than smoothed
    const float plr = Bandwidth.SmoothedPLR;

    TONK_DEBUG_ASSERT(plr >= 0.f && plr <= 1.f);

    // Check if PLR is now good (under threshold)
    const bool goodPLR = plr < TONK_HIGH_PLR_THRESHOLD;

    // Note that negative values overflow high
    const uint64_t elapsedUsec = nowUsec - ProbeStartUsec;
    // Check if the peer has failed to adapt within expected time.
    // This usually means the peer is app-limited for available bandwidth
    // but it might also mean that it has been force-sending a lot of data.
    unsigned propagationUsec = smoothedSendDelayUsec + Bandwidth.SmoothedMaxTripUsec * 2;

    // TODO: REMOVE THIS
    ModuleLogger.Info("DEBUG TEST REMOVE: propagationUsec=", propagationUsec,
        " smoothedSendDelayUsec=", smoothedSendDelayUsec,
        " Bandwidth.SmoothedMaxTripUsec=", Bandwidth.SmoothedMaxTripUsec);

    if (propagationUsec < protocol::kMinBWIntervalUsec * 2) {
        propagationUsec = protocol::kMinBWIntervalUsec * 2;
    }

    // Check if the peer has taken too long to adapt its send rate
    const bool adaptTimeout = elapsedUsec > propagationUsec;

    // Use approximation of original send rate for BPS
    const unsigned attemptedBPS = Bandwidth.GetAttemptedBPS();

    // If PLR is reasonable:
    if (goodPLR)
    {
        // If attempted BPS is near probe rate or timeout occurs:
        if (adaptTimeout ||
            (attemptedBPS > (unsigned)(probeBPS * 1.025) &&
             attemptedBPS < (unsigned)(probeBPS * 0.975)) )
        {
            // If a timeout occurs waiting for peer to adjust send rate:
            if (adaptTimeout)
            {
                ModuleLogger.Info("Probe=Down: Good-PLR; BPS>Probe; TimeoutExpired -> Probe=Cruise" \
                    " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS, " attemptedBPS=", attemptedBPS);
            }
            else
            {
                ModuleLogger.Info("Probe=Down: Good-PLR; BPS<Probe; Success! -> Probe=Cruise" \
                    " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS);
            }

            // Transition: Cruise at last good rate
            ProbingDown = false;
            ProbeBackoffAppBW(
                attemptedBPS,
                plr,
                appBPS,
                probeBPS);
            ShouldAckFast = true; // Tell sender immediately
            MaxUncongestedGoodput.Reset(); // Start watching goodput
            ProbeStartUsec = 0; // Probe is expired now
            FastLoss.ResetStats(); // Clear fast loss indicator

            TONK_DEBUG_ASSERT(probeBPS >= appBPS);
            return;
        }

        // Still waiting for peer to adjust send rate
        ModuleLogger.Info("Probe=Down: Good-PLR; BPS>Probe; Wait..." \
            " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS);

        TONK_DEBUG_ASSERT(probeBPS >= appBPS);
        return;
    }

    // If app did not generate enough traffic:
    if (attemptedBPS < (unsigned)(probeBPS * 0.975f))
    {
        // If we timed out waiting for the peer to adapt their rate:
        if (adaptTimeout)
        {
            // This indicates the channel limit was quickly reduced:

            unsigned goodput = Bandwidth.SmoothedGoodputBPS;

            // Take the larger of smoothed and latest seen BPS.
            // The peer may have just been sending less in this snapshot but
            // more overall, so bump up to the average if needed.
            if (goodput < Bandwidth.LatestGoodputBPS) {
                goodput = Bandwidth.LatestGoodputBPS;
            }

            // Do not allow the value to exceed the current ProbeBPS
            // in case the average is too high.
            if (goodput > probeBPS) {
                goodput = probeBPS;
            }

            // Back off from the current goodput because it is still too high
            ProbeBackoffAppBW(
                goodput,
                plr,
                appBPS,
                probeBPS);
            ShouldAckFast = true; // Tell sender immediately
            ProbeStartUsec = nowUsec; // Restart timer
            // Stay in ProbeDown mode - We are just probing lower than expected

            ModuleLogger.Info("Probe=Down: BAD!PLR; BPS<Probe; TimeoutExpired -> Jumping down!" \
                " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS);

            TONK_DEBUG_ASSERT(probeBPS >= appBPS);
            return;
        }

        ModuleLogger.Info("Probe=Down: BAD!PLR; BPS<Probe; TimeoutExpired -> Waiting..." \
            " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS, " attemptedBPS=", attemptedBPS);

        TONK_DEBUG_ASSERT(probeBPS >= appBPS);
        return;
    }

    // App is generating too much traffic:
    if (attemptedBPS > (unsigned)(probeBPS * 1.025f))
    {
        // Just wait longer - The send limiter is pretty accurate,
        // so it's almost certainly due to an app that is sending at
        // its minimum rate and refuses to send slower.
        ModuleLogger.Info("Probe=Down: BAD!PLR; BPS>Probe; Peer is not behaving..." \
            " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS, " attemptedBPS=", attemptedBPS);

        TONK_DEBUG_ASSERT(probeBPS >= appBPS);
        return;
    }

    // App hit the probe target but PLR is still high:

    LossBackoffBW(
        probeBPS,
        plr,
        appBPS,
        probeBPS);
    ShouldAckFast = true; // Tell sender immediately
    ProbeStartUsec = nowUsec; // Restart timer

    ModuleLogger.Info("Probe=Down: BAD!PLR; BPS=Probe; Probing Lower!" \
        " PLR=", plr, " appBPS=", appBPS, " probeBPS=", probeBPS);

    TONK_DEBUG_ASSERT(probeBPS >= appBPS);
}

/**
    UpdateFastRateProbe()

    This estimates the amount of bandwidth available to the remote
    application, and how much overhead should be devoted to FEC.

    This function operates in the fast rate-probe mode of a connection,
    where each received bandwidth estimate is used as a basis for how
    to rapidly scale up usage of the channel.  The goal is to ramp up
    to full speed as quickly as possible, using delay/loss congestion
    indicators.  Once the channel capacity is reached, the state machine
    transitions into the UpdateSteadyState function.
*/

static const float kSlowStartLossRateThreshold = 0.05f;

void ReceiverBandwidthControl::UpdateFastRateProbe(
    uint64_t nowUsec,
    unsigned smoothedSendDelayUsec,
    unsigned& appBPS,
    unsigned& probeBPS)
{
    if (UpdatedBW) {
        UpdateMaxUncongestedGoodput(nowUsec);
    }

    // If high loss was detected:
    if (FastLoss.HighestLossRate > kSlowStartLossRateThreshold)
    {
        // Start out probing down
        ProbingDown = true;

        // Exit fast rate based probe
        FastRateBaseProbe = false;

        // Base off highest BW reached
        const unsigned bps = MaxUncongestedGoodput.GetBest();
        TONK_DEBUG_ASSERT(bps != 0);

        // Back off BW based on PLR
        LossBackoffBW(
            bps,
            FastLoss.HighestLossRate, // Back off based on loss observed
            appBPS,
            probeBPS);
        ShouldAckFast = true;

        // Give the peer some time to react
        ProbeStartUsec = nowUsec;

        ModuleLogger.Info("FastRateProbe: High PLR during fast rate probe. PLR=",
            FastLoss.HighestLossRate, ". Shape.AverageBPS=", appBPS);

        return;
    }

    if (!UpdatedBW) {
        return;
    }

    // If BPS probably sent was within 5% of probe target:
    if ((unsigned)(MaxUncongestedGoodput.GetBest() * 1.05f) >= probeBPS)
    {
        // Lock in successful send rate as new app allowed rate.
        // Use maximum FEC rate (50%) to probe even higher.
        appBPS = MaxUncongestedGoodput.GetBest();
        probeBPS = (3 * appBPS) / 2;
        ShouldAckFast = true;

        // Give the peer some time to react
        ProbeStartUsec = nowUsec;

        ModuleLogger.Info("FastRateProbe: Probing higher. appBPS=", appBPS, " probeBPS=", probeBPS);

        return;
    }

    // Note that negative values overflow high
    const uint64_t elapsedUsec = nowUsec - ProbeStartUsec;
    TONK_DEBUG_ASSERT(ProbeStartUsec != 0); // Should never happen
    // Check if the peer has failed to adapt within expected time.
    // This usually means the peer is app-limited for available bandwidth
    // but it might also mean that it has been force-sending a lot of data.
    unsigned propagationUsec = smoothedSendDelayUsec + Bandwidth.SmoothedMaxTripUsec * 2;

    // TODO: REMOVE THIS
    ModuleLogger.Info("DEBUG TEST REMOVE: propagationUsec=", propagationUsec,
        " smoothedSendDelayUsec=", smoothedSendDelayUsec,
        " Bandwidth.SmoothedMaxTripUsec=", Bandwidth.SmoothedMaxTripUsec);

    if (propagationUsec < protocol::kMinBWIntervalUsec * 2) {
        propagationUsec = protocol::kMinBWIntervalUsec * 2;
    }

    if (elapsedUsec > propagationUsec)
    {
        // Take the best we have seen so far
        const unsigned bps = MaxUncongestedGoodput.GetBest();
        TONK_DEBUG_ASSERT(bps != 0);

        // Use the worst PLR seen recently
        const float plr = Bandwidth.GetMaxPLR();

        // Exit fast rate based probe
        FastRateBaseProbe = false;

        // Exit to steady-state at the best rate we can
        ProbeBackoffAppBW(
            bps,
            plr,
            appBPS,
            probeBPS);
        ShouldAckFast = true;

        // Give the peer some time to react
        ProbeStartUsec = nowUsec;

        ModuleLogger.Info("FastRateProbe: Timeout waiting for target rate. Exiting to Steady-State with best rate so far. appBPS=", appBPS, " probeBPS=", probeBPS);
    }
}

/**
    UpdateSlowStart()

    This estimates the amount of bandwidth available to the remote
    application, and how much overhead should be devoted to FEC.

    This function operates in the slow-start mode of a connection,
    which is identical to the TCP slow-start algorithm.  All Tonk
    connections start in slow-start because there is no faster way
    to ramp up the sender's send rate.  How it works is for each
    byte that is received, we inform the sender via Ack response
    that it can now send two bytes.  This is called a self-clocked
    congestion control method.  For low-rate streams this is
    significantly faster than waiting for enough data to arrive to
    accurately measure the send bandwidth.

    If a congestion indicator is seen then it will transition
    directly into steady-state.  And if enough data is received
    to make an accurate and fast BW estimate without using the
    self-clocked approach, then it will transition to the
    UpdateFastRateProbe mode.
*/

void ReceiverBandwidthControl::UpdateSlowStart(
    uint64_t nowUsec,
    unsigned& appBPS,
    unsigned& probeBPS)
{
    Shape.SlowStartBytes = SlowStartSentBytes;

    // In slow-start we should ack as fast as possible
    ShouldAckFast = true;

    ModuleLogger.Info("BW: SlowStart.SentBytes=", SlowStartSentBytes);

    // If double-loss occurs during slow-start:
    if (FastLoss.DoubleLoss ||
        FastLoss.HighestLossRate >= kSlowStartLossRateThreshold)
    {
        ModuleLogger.Info("BW: DoubleLoss=", FastLoss.DoubleLoss, " HighestLossRate=", FastLoss.HighestLossRate, " seen - Slowstart completed");

        // Give it another shot - Not sure where to go from slow-start since we do not have good stats.
        FastLoss.ResetStats();

        // Exit slow start
        Shape.InSlowStart = false;

        // Skip rate-based probing
        FastRateBaseProbe = false;

        // Give the peer some time to react
        ProbeStartUsec = nowUsec;

        // Approximate how much bandwidth is available
        appBPS = 0;
        const int64_t deltaUsec = (int64_t)(nowUsec - SlowStartStartUsec);
        if (deltaUsec > 0) {
            appBPS = (unsigned)(((uint64_t)SlowStartSentBytes * 1000000) / (unsigned)deltaUsec);
        }
        if (appBPS < Bandwidth.LatestGoodputBPS) {
            appBPS = Bandwidth.LatestGoodputBPS;
        }
        if (appBPS < protocol::kMinimumBytesPerSecond) {
            appBPS = protocol::kMinimumBytesPerSecond;
        }
        probeBPS = appBPS;

        MaxUncongestedGoodput.Reset();
        MaxUncongestedGoodput.Update(appBPS, nowUsec, kMaxBWWindowUsec);

        return;
    }

    // If slow-start has exceeded a certain rate threshold:
    if (UpdatedBW && Bandwidth.LatestGoodputBPS >= protocol::kLeaveSlowStartKBPS)
    {
        MaxUncongestedGoodput.Reset();
        UpdateMaxUncongestedGoodput(nowUsec);

        // Exit slow start
        Shape.InSlowStart = false;

        // Switch to rate-based probing
        FastRateBaseProbe = true;

        // Set up a timeout to catch apps that cannot put out the numbers we need
        ProbeStartUsec = nowUsec;

        // Lock in successful send rate as new app allowed rate.
        // Use maximum FEC rate (50%) to probe even higher.
        appBPS = MaxUncongestedGoodput.GetBest();
        probeBPS = (3 * appBPS) / 2;

        ModuleLogger.Info("BW: Leaving slow-start after BPS reached a moderate rate. probeBPS=", probeBPS);
    }
}


} // namespace tonk
