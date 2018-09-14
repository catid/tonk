/** \file
    \brief Siamese FEC Implementation: Encoder
    \copyright Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Siamese nor the names of its contributors may be
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

#include "SiameseEncoder.h"
#include "SiameseSerializers.h"

namespace siamese {

#ifdef SIAMESE_ENCODER_DUMP_VERBOSE
    static logger::Channel Logger("Encoder", logger::Level::Debug);
#else
    static logger::Channel Logger("Encoder", logger::Level::Silent);
#endif


//------------------------------------------------------------------------------
// EncoderStats

EncoderStats::EncoderStats()
{
    for (unsigned i = 0; i < SiameseEncoderStats_Count; ++i) {
        Counts[i] = 0;
    }
}


//------------------------------------------------------------------------------
// EncoderPacketWindow

EncoderPacketWindow::EncoderPacketWindow()
{
    NextColumn  = 0;
    ColumnStart = 0;

    ClearWindow();
}

void EncoderPacketWindow::ClearWindow()
{
    FirstUnremovedElement = 0;
    Count                 = 0;
    LongestPacket         = 0;
    SumStartElement       = 0;
    SumEndElement         = 0;

    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        EncoderColumnLane& lane = Lanes[laneIndex];

        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            lane.Sum[sumIndex].Bytes   = 0;
            lane.NextElement[sumIndex] = laneIndex;
        }
        lane.LongestPacket = 0;
    }
}

SiameseResult EncoderPacketWindow::Add(SiameseOriginalPacket& packet)
{
    if (EmergencyDisabled) {
        return Siamese_Disabled;
    }

    if (GetRemainingSlots() <= 0) {
        // This is not invalid input - The application often does not have
        // control over how much data it tries to send.  Instead it must
        // listen for our feedback return code to decide when to slow down.
        return Siamese_MaxPacketsReached;
    }

    const unsigned column         = NextColumn;
    const unsigned subwindowCount = Subwindows.GetSize();
    unsigned element              = Count;

    // Assign packet number
    packet.PacketNum = column;

    // If there is not enough room for this new element:
    // Note: Adding a buffer of kColumnLaneCount to create space ahead for
    // snapshots as a subwindow is filled and we need to store its snapshot
    if (element + kColumnLaneCount >= subwindowCount * kSubwindowSize)
    {
        EncoderSubwindow* subwindow = TheAllocator->Construct<EncoderSubwindow>();
        if (!subwindow || !Subwindows.Append(subwindow))
        {
            EmergencyDisabled = true;
            Logger.Error("WindowAdd.Construct OOM");
            SIAMESE_DEBUG_BREAK();
            return Siamese_Disabled;
        }
    }

    if (Count > 0) {
        ++Count;
    }
    else
    {
        // Start a new window:
        element = column % kColumnLaneCount;
        StartNewWindow(column);
    }

    // Initialize original packet with received data
    OriginalPacket* original = GetWindowElement(element);
    if (0 == original->Initialize(TheAllocator, packet))
    {
        EmergencyDisabled = true;
        Logger.Error("WindowAdd.Initialize OOM");
        SIAMESE_DEBUG_BREAK();
        return Siamese_Disabled;
    }
    SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == element % kColumnLaneCount);

    // Attach original send time in milliseconds
    *GetWindowElementTimestampPtr(element) = static_cast<uint32_t>(GetTimeMsec());

    // Roll next column to assign
    NextColumn = IncrementColumn1(NextColumn);

    // Update longest packet
    const unsigned originalBytes = original->Buffer.Bytes;
    const unsigned laneIndex     = column % kColumnLaneCount;
    EncoderColumnLane& lane      = Lanes[laneIndex];
    if (lane.LongestPacket < originalBytes) {
        lane.LongestPacket = originalBytes;
    }
    if (LongestPacket < originalBytes) {
        LongestPacket = originalBytes;
    }

    Stats->Counts[SiameseEncoderStats_OriginalCount]++;
    Stats->Counts[SiameseEncoderStats_OriginalBytes] += packet.DataBytes;
    return Siamese_Success;
}

void EncoderPacketWindow::StartNewWindow(unsigned column)
{
    // Maintain the invariant that element % 8 == column % 8 by skipping some
    const unsigned element = column % kColumnLaneCount;
    ColumnStart            = column - element;
    SIAMESE_DEBUG_ASSERT(column >= element && ColumnStart < kColumnPeriod);
    SumStartElement        = element;
    SumEndElement          = element;
    FirstUnremovedElement  = element;
    Count                  = element + 1;

    // Reset longest packet
    LongestPacket = 0;
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex) {
        Lanes[laneIndex].LongestPacket = 0;
    }

    Logger.Info(">>> Starting a new window from column ", ColumnStart);
}

void EncoderPacketWindow::RemoveBefore(unsigned firstKeptColumn)
{
    if (EmergencyDisabled) {
        return;
    }

    // Convert column to element, handling wrap-around:
    const unsigned firstKeptElement = ColumnToElement(firstKeptColumn);

    // If the column is outside of the window:
    if (InvalidElement(firstKeptElement))
    {
        // If the element was before the window:
        if (IsColumnDeltaNegative(firstKeptElement)) {
            Logger.Info("Remove before column ", firstKeptColumn, " - Ignored before window");
        }
        else
        {
            // Removed everything
            Count = 0;

            Logger.Info("Remove before column ", firstKeptColumn, " - Removed everything");
        }
    }
    else
    {
        Logger.Info("Remove before column ", firstKeptColumn, " element ", firstKeptElement);

        // Mark these elements for removal next time we generate output
        if (FirstUnremovedElement < firstKeptElement) {
            FirstUnremovedElement = firstKeptElement;
        }
    }
}

void EncoderPacketWindow::ResetSums(unsigned elementStart)
{
    // Recreate all the sums from scratch after this:
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        // Calculate first element to accumulate for this lane
        const unsigned nextElement = GetNextLaneElement(elementStart, laneIndex);

        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            Lanes[laneIndex].NextElement[sumIndex] = nextElement;
            Lanes[laneIndex].Sum[sumIndex].Bytes   = 0;
        }
    }

    SumStartElement = elementStart;
    SumEndElement   = elementStart;
    SumColumnStart  = ElementToColumn(elementStart);
    SumErasedCount  = 0;
}

void EncoderPacketWindow::RemoveElements()
{
    const unsigned firstKeptSubwindow  = FirstUnremovedElement / kSubwindowSize;
    const unsigned removedElementCount = firstKeptSubwindow * kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(firstKeptSubwindow >= 1);
    SIAMESE_DEBUG_ASSERT(firstKeptSubwindow < Subwindows.GetSize());
    SIAMESE_DEBUG_ASSERT(removedElementCount % kColumnLaneCount == 0);
    SIAMESE_DEBUG_ASSERT(removedElementCount <= FirstUnremovedElement);

    Logger.Info("******** Removing up to ", FirstUnremovedElement, " and startColumn=", ColumnStart);

    // If there are running sums:
    if (SumEndElement > SumStartElement)
    {
        // Roll up the sums past the removal point
        for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
        {
            for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
            {
                GetSum(laneIndex, sumIndex, removedElementCount);

                SIAMESE_DEBUG_ASSERT(Lanes[laneIndex].NextElement[sumIndex] >= removedElementCount);
                Lanes[laneIndex].NextElement[sumIndex] -= removedElementCount;
            }
        }

        if (removedElementCount > SumStartElement) {
            SumErasedCount += removedElementCount - SumStartElement;
        }

        if (SumEndElement > removedElementCount) {
            SumEndElement -= removedElementCount;
        }
        else {
            SumEndElement = 0;
        }

        if (SumStartElement > removedElementCount) {
            SumStartElement -= removedElementCount;
        }
        else {
            SumStartElement = 0;
        }
    }

    // Shift kept subwindows to the front of the vector:

    // Resize a temporary buffer for removed subwindows
    if (!SubwindowsShift.SetSize_NoCopy(firstKeptSubwindow))
    {
        SIAMESE_DEBUG_BREAK(); // OOM
        EmergencyDisabled = true;
        return;
    }

    // Store removed subwindows temporarily
    memcpy(
        SubwindowsShift.GetPtr(0),
        Subwindows.GetPtr(0),
        firstKeptSubwindow * sizeof(EncoderSubwindow*)
    );

    const unsigned subwindowsShifted = Subwindows.GetSize() - firstKeptSubwindow;

    // Shift subwindows to front that are being kept
    memmove(
        Subwindows.GetPtr(0),
        Subwindows.GetPtr(firstKeptSubwindow),
        subwindowsShifted * sizeof(EncoderSubwindow*)
    );

    // Removed subwindows are moved to the end (unordered) for later reuse
    memcpy(
        Subwindows.GetPtr(subwindowsShifted),
        SubwindowsShift.GetPtr(0),
        firstKeptSubwindow * sizeof(EncoderSubwindow*)
    );

    // Update the count of elements in the window
    SIAMESE_DEBUG_ASSERT(Count >= removedElementCount);
    Count -= removedElementCount;

    // Roll up the ColumnStart member
    ColumnStart = ElementToColumn(removedElementCount);
    SIAMESE_DEBUG_ASSERT(ColumnStart == Subwindows.GetRef(0)->Originals[0].Column);

    // Roll up the FirstUnremovedElement member
    SIAMESE_DEBUG_ASSERT(FirstUnremovedElement % kSubwindowSize == FirstUnremovedElement - removedElementCount);
    SIAMESE_DEBUG_ASSERT(FirstUnremovedElement >= removedElementCount);
    FirstUnremovedElement -= removedElementCount;

    // Determine the new longest packets
    unsigned longestPacket = 0;
    unsigned laneLongest[kColumnLaneCount] = { 0 };
    for (unsigned i = FirstUnremovedElement, count = Count; i < count; ++i)
    {
        OriginalPacket* original     = GetWindowElement(i);
        const unsigned originalBytes = original->Buffer.Bytes;
        if (longestPacket < originalBytes) {
            longestPacket = originalBytes;
        }
        SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == i % kColumnLaneCount);
        const unsigned laneIndex = i % kColumnLaneCount;
        if (laneLongest[laneIndex] < originalBytes) {
            laneLongest[laneIndex] = originalBytes;
        }
    }

    // Update longest packet fields
    LongestPacket = longestPacket;
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex) {
        Lanes[laneIndex].LongestPacket = laneLongest[laneIndex];
    }

    // If there are no running sums:
    if (SumEndElement <= SumStartElement) {
        ResetSums(FirstUnremovedElement);
    }
}

const GrowingAlignedDataBuffer* EncoderPacketWindow::GetSum(unsigned laneIndex, unsigned sumIndex, unsigned elementEnd)
{
    EncoderColumnLane& lane = Lanes[laneIndex];
    unsigned element = lane.NextElement[sumIndex];
    SIAMESE_DEBUG_ASSERT(element % kColumnLaneCount == laneIndex);
    SIAMESE_DEBUG_ASSERT(element < Count + kColumnLaneCount);

    if (element < elementEnd)
    {
        GrowingAlignedDataBuffer& sum = lane.Sum[sumIndex];

        // Grow this sum for this lane to fit new (larger) data if needed
        if (lane.LongestPacket > 0 &&
            !sum.GrowZeroPadded(TheAllocator, lane.LongestPacket))
        {
            EmergencyDisabled = true;
            goto ExitSum;
        }

        do
        {
            Logger.Info("Lane ", laneIndex, " sum ", sumIndex, " accumulating column: ", ColumnStart + element);

            OriginalPacket* original = GetWindowElement(element);
            const unsigned column    = original->Column;
            unsigned addBytes        = original->Buffer.Bytes;

            if (!sum.GrowZeroPadded(TheAllocator, addBytes))
            {
                EmergencyDisabled = true;
                goto ExitSum;
            }

            SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes <= sum.Bytes || element < FirstUnremovedElement);

            // Sum += PacketData
            if (sumIndex == 0) {
                gf256_add_mem(sum.Data, original->Buffer.Data, addBytes);
            }
            else
            {
                // Sum += CX[2] * PacketData
                uint8_t CX = GetColumnValue(column);
                if (sumIndex == 2) {
                    CX = gf256_sqr(CX);
                }
                gf256_muladd_mem(sum.Data, CX, original->Buffer.Data, addBytes);
            }

            SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == laneIndex);
            element += kColumnLaneCount;
        } while (element < elementEnd);

        // Store next element to accumulate
        lane.NextElement[sumIndex] = element;
    }

ExitSum:
    return &lane.Sum[sumIndex];
}


//------------------------------------------------------------------------------
// EncoderAcknowledgementState

/**
    Siamese Retransmission Timeout (RTO) calculation

    TL;DR:  RTO = Max(RTT) * 1.5


    Why calculate RTO?

    There are three times when retransmission timeouts must be decided:
    (1) When the receiver receives a datagram out of order.
    (2) When the sender does not get any acknowledgement back for a datagram.
    (3) When the sender retransmits a datagram due to (1) or (2) and does not
    get any acknowledgement back for that datagram.

    For case (1) the receiver has full data on reorder variance and can improve
    on the RTO for sequence number holes that it notices.  Sending a NACK right
    away once a hole is noticed might enable the sender to retransmit faster.

    For case (2) and (3) the encoder must decide what the RTO should be without
    any feedback from the decoder except prior acknowledgements.  These are the
    cases where Siamese must determine the appropriate RTO.


    How does Siamese calculate RTO?

    The RTO is used to decide when to retransmit data if no acknowledgements
    have been received, so we need to estimate the maximum time it takes to get
    back an acknowledgement for sent data.

    The delay between when siamese_encoder_add() indicates a packet was sent
    and when siamese_encoder_ack() indicates that packet was received is the
    data available in Encoder::Acknowledge().  This delay is called the Round-
    Trip Time (RTT).

    The Siamese encoder sends (a) original data, (b) FEC recovery packets, and
    (c) retransmissions of original data, each of which can cause the Siamese
    decoder to acknowledge the receipt of the data.  In the acknowledgement
    timing data, the RTT for (a) is mixed in with the RTTs in cases (b)
    and (c) and there is no way to tell those cases apart.

    Fortunately, the maximum RTT in each set acknowledged data is a good
    guide for the RTO value.  For case (a) it helps put an upper bound on
    the time it takes to acknowledge original data.  For case (b) the delay
    values will be unexpectedly small so will be mostly eliminated from the
    data.  For case (c) the delay will either be useful or too small if the
    RTO was too small and the original was just taking too long to arrive.

    The only remaining issue is if any set of acknowledged data is entirely
    made up of values that are too small.  To resolve that problem, a
    windowed maximum is updated over time.

    An advantage of basing the RTO on the maximum RTT is that it can react
    to the leading edge of increasing delays due to cross-traffic, self-
    congestion, and other network changes.  As soon as the delays start
    increasing, the maximum value will be updated immediately.

    The RTO is chosen to be 50% higher than the largest RTT seen lately,
    avoiding unnecessary retransmissions when the delay starts to increase.
    This rough value may overestimate the proper RTO, but it does not need
    to be particularly accurate (see below).


    What other approaches exist?

    For ARQ schemes that need to estimate RTT in addition to RTO, it is more
    important to sort cases (a) from (b) and (c).  Google's QUIC assigns
    each packet a different nonce instead of reusing the same reliable packet
    sequence number, enabling it to differentiate these cases.  For Siamese,
    only RTO is needed so that would be overkill.


    Does RTO need to be very accurate?  No.

    For delay-sensitive flows, FEC recovery is clearly preferred over any of
    these options.  By the time RTT * 2 has elapsed, a few recovery packets
    must have been received, and it is likely that the missing data is
    already recovered.  Basically in case (3) FEC does almost all the heavy
    lifting already, and a larger RTO should be used.  And even in case (2) it
    can play a large role in reducing delay.

    With careful estimation on the receiver side, a NACK can be sent sooner for
    reordered data to cut the overall retransmission time to RTT * 1.5 at best.
    This would require a lot more complexity at the receiver and it would also
    provide no practical benefit over recovering with FEC.

    For high-speed flows, FEC recovery is not desireable because there is no
    delay requirement on the data.  So instead a long RTO can be selected to
    safely avoid retransmitting data unnecessarily, optimizing for bandwidth.
*/

bool EncoderAcknowledgementState::OnAcknowledgementData(const uint8_t* data, unsigned bytes)
{
    unsigned nextColumnExpected = 0;
    int headerBytes = DeserializeHeader_PacketNum(data, bytes, nextColumnExpected);
    if (headerBytes < 1) {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        return false;
    }
    data += headerBytes, bytes -= headerBytes;

    // If we received ack data out of order:
    unsigned columnDelta = SubtractColumns(nextColumnExpected, NextColumnExpected);
    if (IsColumnDeltaNegative(columnDelta)) {
        // Ignore any older data since it would lose information.
        return true;
    }

    /*
        Ignore duplicate data.

        Note if we get the same packet number twice and the data bytes changed
        we will accept the new data.  This means that if datagrams are received
        out of order, the old one might win.

        This can be avoided by including the latest acknowledged datagram nonce
        at the protocol level with each acknowledgement.  Then assuming that
        data is being received regularly it will allow a quick reject for old
        acknowledgement data.
    */
    if (NextColumnExpected == nextColumnExpected &&
        Data && bytes == DataBytes &&
        0 == memcmp(data, Data, bytes))
    {
        return true;
    }

    // Update next expected column
    NextColumnExpected = nextColumnExpected;

    // Reset message decoder state
    Offset     = 0;
    LossColumn = NextColumnExpected;
    LossCount  = 0;
    DataBytes  = bytes;

    // If there are NACK ranges:
    if (bytes > 0)
    {
        // Copy the new data into place with some padding at the end
        Data = TheAllocator->Reallocate(Data, bytes + kPaddingBytes,
            pktalloc::Realloc::Uninitialized);
        memcpy(Data, data, bytes);
        memset(Data + bytes, 0, kPaddingBytes); // Zero guard bytes

        // Returns false if decoding the first loss range fails
        if (!DecodeNextRange()) {
            return false;
        }
        SIAMESE_DEBUG_ASSERT(IsIteratorAtFront()); // Invalid data
    }

    // Update RTO estimation based on the latest loss ranges and rollup
    UpdateRTO();

    // Remove data the given column
    TheWindow->RemoveBefore(NextColumnExpected);

    return true;
}

void EncoderAcknowledgementState::UpdateRTO()
{
    const unsigned windowCount = TheWindow->Count;

    // If there is no unacknowledged data:
    unsigned firstLoss = TheWindow->ColumnToElement(NextColumnExpected);
    if (firstLoss >= windowCount) {
        return;
    }

    // Get current time
    const uint64_t nowMsec64 = GetTimeMsec();
    const uint32_t nowMsec = static_cast<uint32_t>(nowMsec64);
    unsigned longestAckDelayMsec = 0;

    // Locate first unacknowledged element
    unsigned element = TheWindow->ColumnToElement(NextRTOColumn);
    if (element >= windowCount)
    {
        Logger.Warning("NextRTOColumn was invalid, so rolling up to FirstUnremovedElement = ", TheWindow->FirstUnremovedElement);
        element = TheWindow->FirstUnremovedElement;
    }

    // First walk through all the rollups:
    for (; element < firstLoss; ++element)
    {
        // Get last time this was sent (maybe the second time)
        const uint32_t* sendMsecPtr = TheWindow->GetWindowElementTimestampPtr(element);
        SIAMESE_DEBUG_ASSERT(*sendMsecPtr != 0);

        // Calculate delay since it was sent
        const int32_t delayMsec = nowMsec - *sendMsecPtr;
        if ((unsigned)delayMsec > longestAckDelayMsec && delayMsec > 0)
        {
            longestAckDelayMsec = delayMsec;

            Logger.Info("ACKED TS = ", *sendMsecPtr, " for ID=", element + TheWindow->ColumnStart, " <- new max delay: ", delayMsec);
        }
        else
        {
            Logger.Info("acked ts = ", *sendMsecPtr, " for ID=", element + TheWindow->ColumnStart, " - not max delay - ", delayMsec);
        }
    }

    // Scan the selective acknowledgements implied by the NACK ranges:
    unsigned remaining = DataBytes;
    const uint8_t* data = Data;
    while (remaining > 0)
    {
        // Decode NACK loss range
        unsigned relativeStart, lossCountM1;
        const int lossRangeBytes = DeserializeHeader_NACKLossRange(
            data, remaining + kPaddingBytes, relativeStart, lossCountM1);

        if (lossRangeBytes < 0 ||
            lossRangeBytes > (int)remaining)
        {
            SIAMESE_DEBUG_BREAK(); // Invalid input
            return;
        }
        data += lossRangeBytes, remaining -= lossRangeBytes;

        // If element has not rolled up to the first acknowledged packet:
        if (element + 1 < firstLoss) {
            element = firstLoss - 1; // Roll it up
        }

        // Calculat the first loss in this NACK range
        firstLoss += relativeStart;

        // Note that element may already be beyond this NACK range because we
        // may have seen this NACK range in a prior Acknowledgement message.
        // For each element up to the first loss:
        for (; element < firstLoss; ++element)
        {
            if (element >= windowCount) {
                SIAMESE_DEBUG_BREAK(); // Invalid input
                return;
            }

            // Get last time this was sent (maybe the second time)
            const uint32_t* sendMsecPtr = TheWindow->GetWindowElementTimestampPtr(element);
            SIAMESE_DEBUG_ASSERT(*sendMsecPtr != 0);

            // Calculate delay since it was sent
            const int32_t delayMsec = nowMsec - *sendMsecPtr;
            if ((unsigned)delayMsec > longestAckDelayMsec && delayMsec > 0)
            {
                longestAckDelayMsec = delayMsec;

                Logger.Info("NACKED TS = ", *sendMsecPtr, " for ID=", element + TheWindow->ColumnStart, " <- new max delay: ", delayMsec);
            }
            else
            {
                Logger.Info("nacked ts = ", *sendMsecPtr, " for ID=", element + TheWindow->ColumnStart, " - not max delay - ", delayMsec);
            }
        }

        // Update loss offset to keep in sync with NACK range encoder
        firstLoss += lossCountM1 + 2;
    }

    // Update next expected column for RTT calculation
    NextRTOColumn = TheWindow->ElementToColumn(element);

    // If there was no delay measurement:
    if (longestAckDelayMsec <= 0) {
        return;
    }

    // Recalculate the window size based on current timeout
    static const uint64_t kMaxWindowMsec = 4000;
    static const uint64_t kMinWindowMsec = 100;
    uint64_t windowMsec = RetransmitTimeoutMsec * 2;

    if (windowMsec < kMinWindowMsec) {
        windowMsec = kMinWindowMsec;
    }
    else if (windowMsec > kMaxWindowMsec) {
        windowMsec = kMaxWindowMsec;
    }

    // Update estimate of maximum RTT
    MaxWindowedRTT.Update(
        longestAckDelayMsec,
        nowMsec64,
        windowMsec);
    const unsigned RTT_max = MaxWindowedRTT.GetBest();

    // Pick an RTO that is 50% higher than the windowed maximum
    RetransmitTimeoutMsec = (RTT_max * 3) / 2;

    // Do not allow the RTO to get so small that OS scheduling delays might cause timeouts.
    static const unsigned kMinimumRTOMsec = 20;
    if (RetransmitTimeoutMsec < kMinimumRTOMsec) {
        RetransmitTimeoutMsec = kMinimumRTOMsec;
    }

    Logger.Info("Calculated windowMsec = ", windowMsec, " and RTT_max = ", RTT_max, " and RetransmitTimeoutMsec = ", RetransmitTimeoutMsec, " from longestAckDelayMsec = ", longestAckDelayMsec, " NextRTOColumn=", NextRTOColumn);
}

bool EncoderAcknowledgementState::DecodeNextRange()
{
    // If there is no more loss range data to process:
    if (Offset >= DataBytes) {
        return false;
    }

    // Decode loss range format:

    SIAMESE_DEBUG_ASSERT(Data && DataBytes >= 1);

    unsigned relativeStart, lossCountM1;
    const int lossRangeBytes = DeserializeHeader_NACKLossRange(
        Data + Offset, DataBytes + kPaddingBytes - Offset, relativeStart, lossCountM1);
    if (lossRangeBytes < 0) {
        return false;
    }

    Offset += lossRangeBytes;
    if (Offset > DataBytes) {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        // TBD: Disable codec here?
        return false;
    }

    // Move ahead the loss column
    LossColumn = AddColumns(LossColumn, relativeStart);
    LossCount  = lossCountM1 + 1;

    return true;
}

bool EncoderAcknowledgementState::GetNextLossColumn(unsigned& columnOut)
{
    if (LossCount <= 0)
    {
        // Note: LossColumn is used as the offset for the next loss range, so
        // we should increment it to one beyond the end of the current region
        // when we get to the end of the region.
        LossColumn = IncrementColumn1(LossColumn);

        if (!DecodeNextRange()) {
            return false;
        }
    }

    columnOut = LossColumn;

    LossColumn = IncrementColumn1(LossColumn);
    --LossCount;

    return true;
}

void EncoderAcknowledgementState::RestartLossIterator()
{
    // Reset message decoder state
    Offset     = 0;
    LossColumn = NextColumnExpected;
    LossCount  = 0;

    DecodeNextRange();
    // Note: Ignore return value
}

void EncoderAcknowledgementState::Clear()
{
    // Reset message decoder state
    Offset     = 0;
    LossColumn = 0;
    LossCount  = 0;
    DataBytes  = 0;

    // Reset the oldest column
    FoundOldest = false;
}


//------------------------------------------------------------------------------
// Encoder

Encoder::Encoder()
{
    Window.TheAllocator = &TheAllocator;
    Window.Stats        = &Stats;
    Ack.TheAllocator    = &TheAllocator;
    Ack.TheWindow       = &Window;
}

SiameseResult Encoder::Acknowledge(
    const uint8_t* data,
    unsigned bytes,
    unsigned& nextExpectedPacketNumOut)
{
    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    if (!Ack.OnAcknowledgementData(data, bytes)) {
        return Siamese_InvalidInput;
    }

    // Report the next expected packet number
    nextExpectedPacketNumOut = Ack.NextColumnExpected;

    Stats.Counts[SiameseEncoderStats_AckCount]++;
    Stats.Counts[SiameseEncoderStats_AckBytes] += bytes;
    return Siamese_Success;
}

static unsigned LoadOriginal(OriginalPacket* original, SiameseOriginalPacket& originalOut)
{
    const unsigned headerBytes = original->HeaderBytes;
    SIAMESE_DEBUG_ASSERT(headerBytes > 0 && original->Buffer.Bytes > headerBytes);
    const unsigned length = original->Buffer.Bytes - headerBytes;

#ifdef SIAMESE_DEBUG
    // Check: Deserialize length from the front
    unsigned lengthCheck;
    int headerBytesCheck = DeserializeHeader_PacketLength(original->Buffer.Data, original->Buffer.Bytes, lengthCheck);

    if (lengthCheck != length || (int)headerBytes != headerBytesCheck ||
        headerBytesCheck < 1 || lengthCheck == 0 ||
        lengthCheck + headerBytesCheck != original->Buffer.Bytes)
    {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        return 0;
    }
#endif // SIAMESE_DEBUG

    originalOut.PacketNum = original->Column;
    originalOut.Data = original->Buffer.Data + headerBytes;
    originalOut.DataBytes = length;
    return length;
}

SiameseResult Encoder::AttemptRetransmit(
    OriginalPacket* original,
    SiameseOriginalPacket& originalOut)
{
    // Load original data and length
    const unsigned length = LoadOriginal(original, originalOut);
    if (length == 0) {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }

    Stats.Counts[SiameseEncoderStats_RetransmitCount]++;
    Stats.Counts[SiameseEncoderStats_RetransmitBytes] += length;
    return Siamese_Success;
}

SiameseResult Encoder::Retransmit(SiameseOriginalPacket& originalOut)
{
    originalOut.Data = nullptr;
    originalOut.DataBytes = 0;

    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    // If there is no unacknowledged data:
    if (Window.GetUnacknowledgedCount() <= 0)
    {
        Ack.FoundOldest = false; // Reset Ack state
        return Siamese_NeedMoreData;
    }

    // Get the window element first the first column expected by the receiver
    const unsigned firstElement = SubtractColumns(Ack.NextColumnExpected, Window.ColumnStart);
    const unsigned count = Window.Count;

    // If the peer's next expected column is outside the window:
    if (IsColumnDeltaNegative(firstElement) ||
        firstElement < Window.FirstUnremovedElement ||
        firstElement >= count)
    {
        return Siamese_NeedMoreData;
    }

    const uint32_t nowMsec = static_cast<uint32_t>(siamese::GetTimeMsec());
    const uint32_t retransmitMsec = Ack.RetransmitTimeoutMsec;

    // If the oldest column is already found:
    if (Ack.FoundOldest)
    {
        // Calculate the oldest element
        const unsigned element = SubtractColumns(Ack.OldestColumn, Window.ColumnStart);

        // If the oldest element is still within the window:
        if (!IsColumnDeltaNegative(element) &&
            element >= firstElement &&
            element < count)
        {
            OriginalPacket* original = Window.GetWindowElement(element);
            SIAMESE_DEBUG_ASSERT(original->Buffer.Data != nullptr);
            SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes > 0);
            uint32_t* lastSendMsecPtr = Window.GetWindowElementTimestampPtr(element);
            const uint32_t lastSendMsec = *lastSendMsecPtr;
            SIAMESE_DEBUG_ASSERT(lastSendMsec != 0);

            // If RTO has not elapsed for the oldest entry:
            if ((uint32_t)(nowMsec - lastSendMsec) < retransmitMsec) {
                // Keep FoundOldest true for next time and stop for now
                return Siamese_NeedMoreData;
            }

            // Update last send time
            *lastSendMsecPtr = nowMsec;

            // Look again for the oldest column next time
            Ack.FoundOldest = false;

            Logger.Debug("Retransmitting oldest in window. RTO = ", retransmitMsec, ", ID=", original->Column);

            return AttemptRetransmit(original, originalOut);
        }

        // Keep searching for the oldest column
        Ack.FoundOldest = false;
    }

    // Find the first column ready to go, or the oldest column in the set:

    // If the first one is already ready to go:
    unsigned nackElement = firstElement;
    OriginalPacket* oldestOriginal = Window.GetWindowElement(nackElement);
    uint32_t* firstSendMsecPtr = Window.GetWindowElementTimestampPtr(nackElement);
    uint32_t oldestSendMsec = *firstSendMsecPtr;

    // If the first packet RTO expired:
    if ((uint32_t)(nowMsec - oldestSendMsec) >= retransmitMsec)
    {
        // Update last send time
        *firstSendMsecPtr = nowMsec;

        Logger.Debug("Retransmitting first in window. RTO = ", retransmitMsec, ", ID=", oldestOriginal->Column);

        return AttemptRetransmit(oldestOriginal, originalOut);
    }

    // If NACK list is available:
    if (Ack.HasNegativeAcknowledgements())
    {
        // Restart loss iterator for next time
        Ack.RestartLossIterator();

        // While there is another NACK column to process:
        unsigned column;
        while (Ack.GetNextLossColumn(column))
        {
            // If the receiver has already seen all the packets we sent:
            nackElement = Window.ColumnToElement(column);
            if (nackElement >= count) {
                break; // Stop here
            }

            // Lookup original packet and send time
            OriginalPacket* original = Window.GetWindowElement(nackElement);
            SIAMESE_DEBUG_ASSERT(original->Buffer.Data != nullptr);
            SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes > 0);
            uint32_t* lastSendMsecPtr = Window.GetWindowElementTimestampPtr(nackElement);
            const uint32_t lastSendMsec = *lastSendMsecPtr;
            SIAMESE_DEBUG_ASSERT(lastSendMsec != 0);

            // If RTO expired:
            if ((uint32_t)(nowMsec - lastSendMsec) >= retransmitMsec)
            {
                // Update last send time
                *lastSendMsecPtr = nowMsec;

                Logger.Debug("Retransmitting NACK: RTO = ", retransmitMsec, ", ID=", original->Column);

                return AttemptRetransmit(original, originalOut);
            }

            // If this is the oldest send time so far:
            if ((int32_t)(oldestSendMsec - lastSendMsec) > 0)
            {
                oldestOriginal = original;
                oldestSendMsec = lastSendMsec;
            }
        }
    }

    // For each remaining unacknowledged element (in flight) in the window:
    for (unsigned element = nackElement + 1; element < count; ++element)
    {
        // If the element needs to be retransmitted:
        OriginalPacket* original = Window.GetWindowElement(element);
        SIAMESE_DEBUG_ASSERT(original->Buffer.Data != nullptr);
        SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes > 0);
        uint32_t* lastSendMsecPtr = Window.GetWindowElementTimestampPtr(element);
        const uint32_t lastSendMsec = *lastSendMsecPtr;
        SIAMESE_DEBUG_ASSERT(lastSendMsec != 0);

        // If RTO has expired:
        if ((uint32_t)(nowMsec - lastSendMsec) >= retransmitMsec)
        {
            // Update last send time
            *lastSendMsecPtr = nowMsec;

            Logger.Debug("Retransmitting post-NACK: RTO = ", retransmitMsec, ", ID=", original->Column);

            return AttemptRetransmit(original, originalOut);
        }

        // If this is the oldest send time so far:
        if ((int32_t)(oldestSendMsec - lastSendMsec) > 0)
        {
            oldestOriginal = original;
            oldestSendMsec = lastSendMsec;
        }
    }

    // Record the oldest column for next time to avoid doing this search again
    Ack.FoundOldest  = true;
    Ack.OldestColumn = oldestOriginal->Column;
    return Siamese_NeedMoreData;
}

void Encoder::AddDenseColumns(unsigned row, uint8_t* productWorkspace)
{
    const unsigned recoveryBytes = Window.LongestPacket;

    // For each lane:
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        // Compute the operations to run for this lane and row
        unsigned opcode = GetRowOpcode(laneIndex, row);

        // For summations into the RecoveryPacket buffer:
        unsigned mask = 1;
        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            if (opcode & mask)
            {
                const GrowingAlignedDataBuffer* sum = Window.GetSum(laneIndex, sumIndex, Window.Count);
                unsigned addBytes = sum->Bytes;

                if (addBytes > 0)
                {
                    if (addBytes > recoveryBytes) {
                        addBytes = recoveryBytes;
                    }
                    gf256_add_mem(RecoveryPacket.Data, sum->Data, addBytes);
                }
            }
            mask <<= 1;
        }

        // For summations into the ProductWorkspace buffer:
        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            if (opcode & mask)
            {
                const GrowingAlignedDataBuffer* sum = Window.GetSum(laneIndex, sumIndex, Window.Count);
                unsigned addBytes = sum->Bytes;

                if (addBytes > 0)
                {
                    if (addBytes > recoveryBytes) {
                        addBytes = recoveryBytes;
                    }
                    gf256_add_mem(productWorkspace, sum->Data, addBytes);
                }
            }
            mask <<= 1;
        }
    }

    // Keep track of where the sum ended
    Window.SumEndElement = Window.Count;
}

void Encoder::AddLightColumns(unsigned row, uint8_t* productWorkspace)
{
    const unsigned startElement = Window.FirstUnremovedElement;
    SIAMESE_DEBUG_ASSERT(Window.SumEndElement >= startElement);
    const unsigned count = Window.SumEndElement - startElement;
    SIAMESE_DEBUG_ASSERT(count >= 2);
    SIAMESE_DEBUG_ASSERT(count <= Window.Count);

    PCGRandom prng;
    prng.Seed(row, count);

    std::ostringstream* pDebugMsg = nullptr;
    if (Logger.ShouldLog(logger::Level::Debug))
    {
        pDebugMsg = new std::ostringstream();
        *pDebugMsg << "LDPC columns: ";
    }

    const unsigned pairCount = (count + kPairAddRate - 1) / kPairAddRate;
    for (unsigned i = 0; i < pairCount; ++i)
    {
        const unsigned element1          = startElement + (prng.Next() % count);
        const OriginalPacket* original1  = Window.GetWindowElement(element1);
        const unsigned elementRX         = startElement + (prng.Next() % count);
        const OriginalPacket* originalRX = Window.GetWindowElement(elementRX);

        if (pDebugMsg) {
            *pDebugMsg << element1 << " " << elementRX << " ";
        }

        SIAMESE_DEBUG_ASSERT(original1->Column  == Window.ColumnStart + element1);
        SIAMESE_DEBUG_ASSERT(originalRX->Column == Window.ColumnStart + elementRX);
        SIAMESE_DEBUG_ASSERT(Window.LongestPacket >= original1->Buffer.Bytes);
        SIAMESE_DEBUG_ASSERT(Window.LongestPacket >= originalRX->Buffer.Bytes);

        gf256_add_mem(RecoveryPacket.Data, original1->Buffer.Data,  original1->Buffer.Bytes);
        gf256_add_mem(productWorkspace,    originalRX->Buffer.Data, originalRX->Buffer.Bytes);
    }

    if (pDebugMsg)
    {
        Logger.Debug(pDebugMsg->str());
        delete pDebugMsg;
    }
}

SiameseResult Encoder::Encode(SiameseRecoveryPacket& packet)
{
    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    // If there are no packets so far:
    if (Window.Count <= 0)
    {
        packet.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    // Remove any data from the window at this point
    if (Window.FirstUnremovedElement >= kEncoderRemoveThreshold) {
        Window.RemoveElements();
    }

    // Get the number of packets in the window that are in flight (unacked)
    const unsigned unacknowledgedCount = Window.GetUnacknowledgedCount();

    // If there is only a single packet so far:
    if (unacknowledgedCount == 1) {
        return GenerateSinglePacket(packet);
    }

    // Calculate upper bound on width of sum for this recovery packet
    SIAMESE_DEBUG_ASSERT(Window.Count + Window.SumErasedCount >= Window.SumStartElement);
    const unsigned newSumCountUB = Window.Count - Window.SumStartElement + Window.SumErasedCount;

    // If sums should be reset because the range is empty or too large:
    if (Window.SumEndElement <= Window.SumStartElement ||
        newSumCountUB >= SIAMESE_MAX_PACKETS)
    {
#ifdef SIAMESE_ENABLE_CAUCHY
        // If the number of packets in flight is small enough, use Cauchy rows for now:
        if (unacknowledgedCount <= SIAMESE_CAUCHY_THRESHOLD) {
            return GenerateCauchyPacket(packet);
        }
#endif // SIAMESE_ENABLE_CAUCHY

        Logger.Debug("Resetting sums at element ", Window.FirstUnremovedElement);

        Window.ResetSums(Window.FirstUnremovedElement);
    }
#ifdef SIAMESE_ENABLE_CAUCHY
    else
    {
        // If the number of packets in flight may indicate Cauchy is better or we need to use it:
        if (unacknowledgedCount <= SIAMESE_SUM_RESET_THRESHOLD ||
            newSumCountUB <= SIAMESE_CAUCHY_THRESHOLD)
        {
            SIAMESE_DEBUG_ASSERT(newSumCountUB >= unacknowledgedCount);
            static_assert(SIAMESE_SUM_RESET_THRESHOLD <= SIAMESE_CAUCHY_THRESHOLD, "Update this too");

            // Stop using sums
            Window.SumEndElement = Window.SumStartElement;

            return GenerateCauchyPacket(packet);
        }
    }
#endif // SIAMESE_ENABLE_CAUCHY

    // Advance row index
    const unsigned row = NextRow;
    if (++NextRow >= kRowPeriod) {
        NextRow = 0;
    }

    // Reset workspaces
    const unsigned recoveryBytes = Window.LongestPacket;
    const unsigned alignedBytes = pktalloc::NextAlignedOffset(recoveryBytes);
    if (!RecoveryPacket.Initialize(&TheAllocator, 2 * alignedBytes + kMaxRecoveryMetadataBytes))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }
    SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= alignedBytes * 2);
    memset(RecoveryPacket.Data, 0, alignedBytes * 2);
    uint8_t* productWorkspace = RecoveryPacket.Data + alignedBytes;

    // Generate the recovery packet
    AddDenseColumns(row, productWorkspace);
    AddLightColumns(row, productWorkspace);

    // RecoveryPacket += RX * ProductWorkspace
    const uint8_t RX = GetRowValue(row);
    gf256_muladd_mem(RecoveryPacket.Data, RX, productWorkspace, recoveryBytes);

    RecoveryMetadata metadata;
    SIAMESE_DEBUG_ASSERT(Window.SumEndElement + Window.SumErasedCount >= Window.SumStartElement);
    metadata.SumCount    = Window.SumEndElement - Window.SumStartElement + Window.SumErasedCount;
    metadata.LDPCCount   = unacknowledgedCount;
    metadata.ColumnStart = Window.SumColumnStart;
    metadata.Row         = row;

    // Serialize metadata into the last few bytes of the packet
    // Note: This saves an extra copy to move the data around
    const unsigned footerBytes = SerializeFooter_RecoveryMetadata(metadata, RecoveryPacket.Data + recoveryBytes);
    packet.Data      = RecoveryPacket.Data;
    packet.DataBytes = recoveryBytes + footerBytes;

    Stats.Counts[SiameseEncoderStats_RecoveryCount]++;
    Stats.Counts[SiameseEncoderStats_RecoveryBytes] += packet.DataBytes;

    Logger.Info("Generated Siamese sum recovery packet start=", metadata.ColumnStart, " ldpcCount=", metadata.LDPCCount, " sumCount=", metadata.SumCount, " row=", metadata.Row);

    return Siamese_Success;
}

SiameseResult Encoder::Get(SiameseOriginalPacket& originalOut)
{
    // Note: Keep this in sync with Decoder::Get

    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    // Note: This also works when Count == 0
    SIAMESE_DEBUG_ASSERT(originalOut.PacketNum >= Window.ColumnStart);
    const unsigned element = Window.ColumnToElement(originalOut.PacketNum);
    if (Window.InvalidElement(element))
    {
        // Set default return value
        originalOut.Data      = nullptr;
        originalOut.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    // Return the packet data
    OriginalPacket* original = Window.GetWindowElement(element);
    if (original->Buffer.Bytes <= 0)
    {
        originalOut.Data      = nullptr;
        originalOut.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    SIAMESE_DEBUG_ASSERT(original->Column == originalOut.PacketNum);

    // Load original data and length
    if (0 == LoadOriginal(original, originalOut))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }

    return Siamese_Success;
}

SiameseResult Encoder::GenerateSinglePacket(SiameseRecoveryPacket& packet)
{
    OriginalPacket* original     = Window.GetWindowElement(Window.FirstUnremovedElement);
    const unsigned originalBytes = original->Buffer.Bytes;

    // Note: This often does not actually reallocate or move since we overallocate
    if (!original->Buffer.GrowZeroPadded(&TheAllocator, originalBytes + kMaxRecoveryMetadataBytes))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }

    // Set bytes back to original
    original->Buffer.Bytes = originalBytes;

    // Serialize metadata into the last few bytes of the packet
    // Note: This saves an extra copy to move the data around
    RecoveryMetadata metadata;
    metadata.SumCount    = 1;
    metadata.LDPCCount   = 1;
    metadata.ColumnStart = original->Column;
    metadata.Row         = 0;

    const unsigned footerBytes = SerializeFooter_RecoveryMetadata(metadata, original->Buffer.Data + originalBytes);
    packet.Data      = original->Buffer.Data;
    packet.DataBytes = originalBytes + footerBytes;

    Logger.Info("Generated single recovery packet start=", metadata.ColumnStart, " ldpcCount=", metadata.LDPCCount, " sumCount=", metadata.SumCount, " row=", metadata.Row);

    Stats.Counts[SiameseEncoderStats_RecoveryCount]++;
    Stats.Counts[SiameseEncoderStats_RecoveryBytes] += packet.DataBytes;

    return Siamese_Success;
}


#ifdef SIAMESE_ENABLE_CAUCHY

SiameseResult Encoder::GenerateCauchyPacket(SiameseRecoveryPacket& packet)
{
    // Reset recovery packet
    const unsigned firstElement  = Window.FirstUnremovedElement;
    const unsigned recoveryBytes = Window.LongestPacket;
    if (!RecoveryPacket.Initialize(&TheAllocator, recoveryBytes + kMaxRecoveryMetadataBytes))
    {
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }

    const unsigned unacknowledgedCount = Window.GetUnacknowledgedCount();
    RecoveryMetadata metadata;
    metadata.SumCount    = unacknowledgedCount;
    metadata.LDPCCount   = unacknowledgedCount;
    metadata.ColumnStart = Window.ElementToColumn(firstElement);

    // We have to recalculate the number of used bytes since the Cauchy/parity rows may be
    // shorter since they do not need to contain the start of the window which may be acked.
    unsigned usedBytes = 0;

    // If it is time to generate a new parity row:
    const unsigned nextParityElement = Window.ColumnToElement(NextParityColumn);
    if (nextParityElement <= firstElement || IsColumnDeltaNegative(nextParityElement))
    {
        // Set next time we write a parity row
        NextParityColumn = AddColumns(metadata.ColumnStart, unacknowledgedCount);

        // Row 0 is a parity row
        metadata.Row = 0;

        // Unroll first column
        OriginalPacket* original = Window.GetWindowElement(firstElement);
        unsigned originalBytes   = original->Buffer.Bytes;

        memcpy(RecoveryPacket.Data, original->Buffer.Data, originalBytes);
        // Pad the rest out with zeros to avoid corruption
        SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);
        memset(RecoveryPacket.Data + originalBytes, 0, recoveryBytes - originalBytes);

        usedBytes = originalBytes;

        // For each remaining column:
        for (unsigned element = firstElement + 1, count = Window.Count; element < count; ++element)
        {
            original      = Window.GetWindowElement(element);
            originalBytes = original->Buffer.Bytes;

            SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);

            gf256_add_mem(RecoveryPacket.Data, original->Buffer.Data, originalBytes);

            if (usedBytes < originalBytes)
                usedBytes = originalBytes;
        }
    }
    else
    {
        // Select Cauchy row number
        const unsigned cauchyRow = NextCauchyRow;
        metadata.Row = cauchyRow + 1;
        if (++NextCauchyRow >= kCauchyMaxRows)
            NextCauchyRow = 0;

        // Unroll first column
        unsigned cauchyColumn    = metadata.ColumnStart % kCauchyMaxColumns;
        OriginalPacket* original = Window.GetWindowElement(firstElement);
        uint8_t y                = CauchyElement(cauchyRow, cauchyColumn);
        unsigned originalBytes   = original->Buffer.Bytes;

        gf256_mul_mem(RecoveryPacket.Data, original->Buffer.Data, y, originalBytes);
        // Pad the rest out with zeros to avoid corruption
        SIAMESE_DEBUG_ASSERT(recoveryBytes >= originalBytes);
        SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);
        memset(RecoveryPacket.Data + originalBytes, 0, recoveryBytes - originalBytes);

        usedBytes = originalBytes;

        // For each remaining column:
        for (unsigned element = firstElement + 1, count = Window.Count; element < count; ++element)
        {
            cauchyColumn  = (cauchyColumn + 1) % kCauchyMaxColumns;
            original      = Window.GetWindowElement(element);
            originalBytes = original->Buffer.Bytes;
            y             = CauchyElement(cauchyRow, cauchyColumn);

            SIAMESE_DEBUG_ASSERT(RecoveryPacket.Bytes >= originalBytes);

            gf256_muladd_mem(RecoveryPacket.Data, y, original->Buffer.Data, originalBytes);

            if (usedBytes < originalBytes)
                usedBytes = originalBytes;
        }
    }

    // Slap metadata footer on the end
    const unsigned footerBytes = SerializeFooter_RecoveryMetadata(metadata, RecoveryPacket.Data + usedBytes);

    packet.Data      = RecoveryPacket.Data;
    packet.DataBytes = usedBytes + footerBytes;

    Logger.Info("Generated Cauchy/parity recovery packet start=", metadata.ColumnStart, " ldpcCount=", metadata.LDPCCount, " sumCount=", metadata.SumCount, " row=", metadata.Row);

    Stats.Counts[SiameseEncoderStats_RecoveryCount]++;
    Stats.Counts[SiameseEncoderStats_RecoveryBytes] += packet.DataBytes;

    return Siamese_Success;
}

#endif // SIAMESE_ENABLE_CAUCHY

SiameseResult Encoder::GetStatistics(uint64_t* statsOut, unsigned statsCount)
{
    if (statsCount > SiameseEncoderStats_Count)
        statsCount = SiameseEncoderStats_Count;

    // Fill in memory allocated
    Stats.Counts[SiameseEncoderStats_MemoryUsed] = TheAllocator.GetMemoryAllocatedBytes();

    for (unsigned i = 0; i < statsCount; ++i)
        statsOut[i] = Stats.Counts[i];

    return Siamese_Success;
}


} // namespace siamese
