/** \file
    \brief Siamese FEC Implementation: Decoder
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

#include "SiameseDecoder.h"
#include "SiameseSerializers.h"

namespace siamese {

#ifdef SIAMESE_DECODER_DUMP_VERBOSE
    static logger::Channel Logger("Decoder", logger::Level::Debug);
#else
    static logger::Channel Logger("Decoder", logger::Level::Silent);
#endif


//------------------------------------------------------------------------------
// DecoderStats

DecoderStats::DecoderStats()
{
    for (unsigned i = 0; i < SiameseDecoderStats_Count; ++i) {
        Counts[i] = 0;
    }
}


//------------------------------------------------------------------------------
// Decoder

Decoder::Decoder()
{
    RecoveryPackets.TheAllocator  = &TheAllocator;
    RecoveryPackets.CheckedRegion = &CheckedRegion;
    Window.TheAllocator           = &TheAllocator;
    Window.Stats                  = &Stats;
    Window.CheckedRegion          = &CheckedRegion;
    Window.RecoveryPackets        = &RecoveryPackets;
    Window.RecoveryMatrix         = &RecoveryMatrix;
    RecoveryMatrix.TheAllocator   = &TheAllocator;
    RecoveryMatrix.Window         = &Window;
    RecoveryMatrix.CheckedRegion  = &CheckedRegion;
    CheckedRegion.RecoveryMatrix  = &RecoveryMatrix;
}

SiameseResult Decoder::Get(SiameseOriginalPacket& packetOut)
{
    // Note: Keep this in sync with Encoder::Get

    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    // Note: This also works when Count == 0
    const unsigned element = Window.ColumnToElement(packetOut.PacketNum);
    if (Window.InvalidElement(element))
    {
        // Set default return value
        packetOut.Data      = nullptr;
        packetOut.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    // Return the packet data
    OriginalPacket* original = Window.GetWindowElement(element);
    if (original->Buffer.Bytes <= 0)
    {
        packetOut.Data      = nullptr;
        packetOut.DataBytes = 0;
        return Siamese_NeedMoreData;
    }

    const unsigned headerBytes = original->HeaderBytes;
    SIAMESE_DEBUG_ASSERT(headerBytes > 0 && original->Buffer.Bytes > headerBytes);
    const unsigned length = original->Buffer.Bytes - headerBytes;

#ifdef SIAMESE_DEBUG
    // Check: Deserialize length from the front
    unsigned lengthCheck;
    int headerBytesCheck = DeserializeHeader_PacketLength(
        original->Buffer.Data,
        original->Buffer.Bytes,
        lengthCheck);

    if (lengthCheck != length || (int)headerBytes != headerBytesCheck ||
        headerBytesCheck < 1 || lengthCheck == 0 ||
        lengthCheck + headerBytesCheck != original->Buffer.Bytes)
    {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        Window.EmergencyDisabled = true;
        return Siamese_Disabled;
    }
#endif // SIAMESE_DEBUG

    packetOut.Data      = original->Buffer.Data + headerBytes;
    packetOut.DataBytes = length;
    return Siamese_Success;
}

SiameseResult Decoder::GenerateAcknowledgement(
    uint8_t* buffer,
    unsigned byteLimit,
    unsigned& usedBytesOut)
{
    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    SIAMESE_DEBUG_ASSERT(byteLimit >= SIAMESE_ACK_MIN_BYTES);

    std::ostringstream* pDebugMsg = nullptr;

    // If we have no data yet:
    const unsigned windowCount = Window.Count;
    if (windowCount == 0)
    {
        // This should only happen before we receive any data at all.
        // After we receive some data we keep a window of data around to decode FEC packets
        SIAMESE_DEBUG_ASSERT(Window.ColumnStart == 0);
        usedBytesOut = 0;
        return Siamese_NeedMoreData;
    }

    const uint8_t* bufferStart = buffer;

    // Calculate next column we expect to receive
    const unsigned nextElementExpected = Window.NextExpectedElement;
    SIAMESE_DEBUG_ASSERT(nextElementExpected <= windowCount);
    const unsigned nextColumnExpected = Window.ElementToColumn(nextElementExpected);
    unsigned headerBytes = SerializeHeader_PacketNum(nextColumnExpected, buffer);
    buffer += headerBytes, byteLimit -= headerBytes;

    // If there is no missing data:
    if (Window.InvalidElement(nextElementExpected))
    {
        // Write used bytes
        usedBytesOut = (unsigned)(uintptr_t)(buffer - bufferStart);

        Stats.Counts[SiameseDecoderStats_AckCount]++;
        Stats.Counts[SiameseDecoderStats_AckBytes] += usedBytesOut;

        return Siamese_Success;
    }

    SIAMESE_DEBUG_ASSERT(Window.GetWindowElement(nextElementExpected)->Buffer.Bytes == 0);

    // Start searching for the next set bit at the next after the next expected element
    unsigned rangeOffset = nextElementExpected;

    if (Logger.ShouldLog(logger::Level::Debug))
    {
        delete pDebugMsg;
        pDebugMsg = new std::ostringstream();
        *pDebugMsg << "Building ack from nextExpectedColumn=" << nextColumnExpected << " : NACKs = {";
    }

    // While there is room for another maximum-length loss range:
    while (byteLimit >= kMaxLossRangeFieldBytes)
    {
        const unsigned rangeStart = Window.FindNextLostElement(rangeOffset);
        if (rangeStart >= windowCount)
        {
            SIAMESE_DEBUG_ASSERT(rangeStart == windowCount);
            if (pDebugMsg)
                *pDebugMsg << " next:" << AddColumns(Window.ColumnStart, rangeStart);

            // Noticed this can happen somehow
            if (windowCount >= rangeOffset)
            {
                // Take range start relative to the range offset
                const unsigned relativeStart = windowCount - rangeOffset;

                // Serialize this NACK loss range into the buffer
                const unsigned encodedBytes = SerializeHeader_NACKLossRange(relativeStart, 0, buffer);
                buffer += encodedBytes;
            }

            break;
        }
        SIAMESE_DEBUG_ASSERT(rangeStart >= rangeOffset);

        unsigned rangeEnd = Window.FindNextGotElement(rangeStart + 1);
        SIAMESE_DEBUG_ASSERT(rangeEnd > rangeStart);
        SIAMESE_DEBUG_ASSERT(rangeEnd <= windowCount);
        unsigned lossCountM1 = rangeEnd - rangeStart - 1; // Loss count minus 1

        if (pDebugMsg)
        {
            if (lossCountM1 > 0) {
                *pDebugMsg << " " << AddColumns(Window.ColumnStart, rangeStart)
                    << "-" << Window.ElementToColumn(rangeEnd - 1);
            }
            else {
                *pDebugMsg << " " << AddColumns(Window.ColumnStart, rangeStart);
            }
        }

        // Take range start relative to the range offset
        SIAMESE_DEBUG_ASSERT(rangeStart >= rangeOffset);
        const unsigned relativeStart = rangeStart - rangeOffset;

        // Serialize this NACK loss range into the buffer
        const unsigned encodedBytes = SerializeHeader_NACKLossRange(relativeStart, lossCountM1, buffer);

        // Range end is one beyond the end of the loss region.
        // The next loss cannot be before one after the range end, since we
        // either found a received packet id there, or we hit end of range.
        // This is also where we should start searching for losses again
        rangeOffset = rangeEnd + 1;

        // Advance buffer write pointer
        buffer += encodedBytes;
        byteLimit -= encodedBytes;
    }
    // Note that the loss range list may have been truncated due to the buffer space constraint

    if (pDebugMsg)
    {
        *pDebugMsg << " }";
        Logger.Debug(pDebugMsg->str());
    }

    // Write used bytes
    usedBytesOut = (unsigned)(uintptr_t)(buffer - bufferStart);

    Stats.Counts[SiameseDecoderStats_AckCount]++;
    Stats.Counts[SiameseDecoderStats_AckBytes] += usedBytesOut;

    return Siamese_Success;
}

SiameseResult Decoder::AddRecovery(const SiameseRecoveryPacket& packet)
{
    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    // Deserialize the recovery metadata from the front of the packet
    RecoveryMetadata metadata;
    int footerSize = DeserializeFooter_RecoveryMetadata(packet.Data, packet.DataBytes, metadata);
    if (footerSize < 0)
    {
        Window.EmergencyDisabled = true;
        Logger.Error("AddRecovery: Corrupt recovery metadata");
        SIAMESE_DEBUG_BREAK(); // Should never happen
        return Siamese_Disabled;
    }

    Stats.Counts[SiameseDecoderStats_RecoveryCount]++;
    Stats.Counts[SiameseDecoderStats_RecoveryBytes] += packet.DataBytes;

    // Check if recovery packet was received out of order
    bool outOfOrder = IsColumnDeltaNegative(metadata.ColumnStart + metadata.SumCount - LatestColumn);
    if (!outOfOrder) {
        // Update the latest received column
        LatestColumn = (metadata.ColumnStart + metadata.SumCount) % kColumnPeriod;
    }

#if 0
    if (outOfOrder)
    {
        Logger.Warning("Ignoring recovery packet received out of order");
        Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
        return Siamese_Success; // This packet cannot be used for recovery
    }
#endif

    unsigned elementStart, elementEnd;

    // Check if we need this recovery packet:
    if (Window.Count <= 0)
    {
        if (outOfOrder)
        {
            Logger.Warning("Recovery packet cannot be used because it was received by an empty window out of order");
            Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
            return Siamese_Success; // This packet cannot be used for recovery
        }

        Logger.Info("Got first recovery packet: ColumnStart=", metadata.ColumnStart, " SumCount=", metadata.SumCount, " LDPC_Count=", metadata.LDPCCount, " Row=", metadata.Row);

        Window.ColumnStart = metadata.ColumnStart;

        if (!Window.GrowWindow(metadata.SumCount))
        {
            Window.EmergencyDisabled = true;
            Logger.Error("AddRecovery.GrowWindow: OOM");
            return Siamese_Disabled;
        }

        elementEnd = metadata.SumCount;
        elementStart = elementEnd - metadata.LDPCCount;

        // This should only happen at the start if we get recovery first before data
        SIAMESE_DEBUG_ASSERT(Window.NextExpectedElement == 0);
    }
    else
    {
        Logger.Info("Got recovery packet: ColumnStart=", metadata.ColumnStart, " SumCount=", metadata.SumCount, " LDPC_Count=", metadata.LDPCCount, " Row=", metadata.Row);

        elementEnd = Window.ColumnToElement(metadata.ColumnStart + metadata.SumCount);

        // Ignore data from too long ago
        if (IsColumnDeltaNegative(elementEnd))
        {
            Logger.Info("Packet cannot be used because it ends before the window starts");
            Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
            return Siamese_Success;
        }

        // If we clipped the LDPC region already out of the window:
        if (elementEnd < metadata.LDPCCount)
        {
            Logger.Warning("Recovery packet cannot be used because we clipped its LDPC region already: Received too far out of order?");
            Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
            return Siamese_Success; // This packet cannot be used for recovery
        }
        elementStart = elementEnd - metadata.LDPCCount;

        // Ignore data we already have
        if (elementEnd <= Window.NextExpectedElement)
        {
            if (outOfOrder)
            {
                Logger.Warning("Recovery packet does not contain new data and is out of order, so ignoring it");
                Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
                return Siamese_Success; // This packet cannot be used for recovery
            }

            Logger.Debug("Ignoring unnecessary recovery packet for data we received successfully");
            if (elementStart >= kDecoderRemoveThreshold)
            {
                const unsigned recoveryBytes = packet.DataBytes - footerSize;

                // Update the last received recovery metadata
                RecoveryPackets.LastRecoveryMetadata = metadata;
                RecoveryPackets.LastRecoveryBytes = recoveryBytes;

                Window.RemoveElements();
            }
            Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
            return Siamese_Success;
        }

        // Ignore sums that include data we have removed already
#ifdef SIAMESE_ENABLE_CAUCHY
        // If it is a Siamese sum row:
        if (metadata.SumCount > SIAMESE_CAUCHY_THRESHOLD)
#endif
        {
            // If there is no running sum or it does not match the new one:
            if (Window.SumColumnCount == 0 || Window.SumColumnStart != metadata.ColumnStart)
            {
                // Then we need to have all the data in the sum at hand or it is useless.
                const unsigned elementSumStart = Window.ColumnToElement(metadata.ColumnStart);
                if (Window.InvalidElement(elementSumStart))
                {
                    Logger.Info("Recovery packet cannot be used because we clipped its Sum region already : " \
                        "Received too far out of order: Window.SumColumnStart = ",
                        Window.SumColumnStart, ", Window.SumColumnCount = ", Window.SumColumnCount,
                        ", metadata.ColumnStart = ", metadata.ColumnStart);

                    Stats.Counts[SiameseDecoderStats_DupedRecoveryCount]++;
                    return Siamese_Success;
                }
            }
        }

        // Grow the original packet window to cover all the packets this one protects
        if (!Window.GrowWindow(elementEnd))
        {
            Window.EmergencyDisabled = true;
            Logger.Error("AddRecovery.GrowWindow2: OOM");
            return Siamese_Disabled;
        }
    }

    // If this is a single (duplicate) packet:
    if (metadata.SumCount == 1)
    {
        if (!AddSingleRecovery(packet, metadata, footerSize))
        {
            Window.EmergencyDisabled = true;
            Logger.Error("AddRecovery.AddSingleRecovery failed");
            return Siamese_Disabled;
        }
        return Siamese_Success;
    }

    // Allocate a packet object
    RecoveryPacket* recovery = (RecoveryPacket*)TheAllocator.Construct<RecoveryPacket>();
    if (!recovery)
    {
        Window.EmergencyDisabled = true;
        Logger.Error("AddRecovery.Construct OOM");
        return Siamese_Disabled;
    }

    SIAMESE_DEBUG_ASSERT((unsigned)footerSize < packet.DataBytes);
    const unsigned recoveryBytes = packet.DataBytes - footerSize;
    SIAMESE_DEBUG_ASSERT(recoveryBytes > 0);

    if (!recovery->Buffer.Initialize(&TheAllocator, recoveryBytes))
    {
        TheAllocator.Destruct(recovery);
        Window.EmergencyDisabled = true;
        Logger.Error("AddRecovery.Initialize OOM");
        return Siamese_Disabled;
    }

    // Fill in the packet object
    memcpy(recovery->Buffer.Data, packet.Data, recoveryBytes);
    recovery->Metadata     = metadata;
    recovery->ElementStart = elementStart;
    recovery->ElementEnd   = elementEnd;

    // Insert it into the sorted packet list
    RecoveryPackets.Insert(recovery, outOfOrder);

    // Remove elements from the front if possible
    if (elementStart >= kDecoderRemoveThreshold) {
        Window.RemoveElements();
    }

    return Siamese_Success;
}

bool Decoder::AddSingleRecovery(const SiameseRecoveryPacket& packet, const RecoveryMetadata& metadata, int footerSize)
{
    const unsigned element = Window.ColumnToElement(metadata.ColumnStart);
    if (Window.InvalidElement(element)) {
        SIAMESE_DEBUG_BREAK(); // Should never happen
        return false;
    }

    // Note: In this case the length is already prefixed to the data
    SIAMESE_DEBUG_ASSERT(metadata.LDPCCount == 1 && metadata.Row == 0);
    OriginalPacket* windowOriginal = Window.GetWindowElement(element);

    // Ignore duplicate data
    if (windowOriginal->Buffer.Bytes != 0) {
        return true;
    }

    // Check: Deserialize length from the front
    SIAMESE_DEBUG_ASSERT(packet.DataBytes > (unsigned)footerSize);
    const unsigned lengthPlusDataBytes = packet.DataBytes - footerSize;
    unsigned lengthCheck;
    int headerBytes = DeserializeHeader_PacketLength(packet.Data, lengthPlusDataBytes, lengthCheck);
    if (headerBytes < 1 || lengthCheck == 0 ||
        lengthCheck + headerBytes != lengthPlusDataBytes)
    {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        return false;
    }

    SiameseOriginalPacket original;
    SIAMESE_DEBUG_ASSERT((int)packet.DataBytes > footerSize + headerBytes);
    original.DataBytes = packet.DataBytes - footerSize - headerBytes;
    original.Data      = packet.Data + headerBytes;
    original.PacketNum = metadata.ColumnStart;

    unsigned newHeaderBytes = windowOriginal->Initialize(&TheAllocator, original);
    SIAMESE_DEBUG_ASSERT(newHeaderBytes == (unsigned)headerBytes);
    if (0 == newHeaderBytes) {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        return false;
    }
    SIAMESE_DEBUG_ASSERT(windowOriginal->Buffer.Bytes > 1);

    if (!Window.HasRecoveredPackets)
    {
        Window.HasRecoveredPackets = true;
        Window.RecoveredPackets.Clear();
    }

    original.Data = windowOriginal->Buffer.Data + newHeaderBytes;
    SIAMESE_DEBUG_ASSERT(original.DataBytes == windowOriginal->Buffer.Bytes - headerBytes);

    if (!Window.RecoveredPackets.Append(original)) {
        SIAMESE_DEBUG_BREAK(); // OOM
        return false;
    }
    if (!Window.RecoveredColumns.Append(metadata.ColumnStart)) {
        SIAMESE_DEBUG_BREAK(); // OOM
        return false;
    }

    // If the added element is somewhere inside the previously checked region:
    if (element >= CheckedRegion.ElementStart &&
        element < CheckedRegion.NextCheckStart)
    {
        CheckedRegion.Reset();
    }

    // If this was the next expected element:
    if (Window.MarkGotColumn(metadata.ColumnStart))
    {
        SIAMESE_DEBUG_ASSERT(element == Window.NextExpectedElement);

        // Iterate the next expected element beyond the recovery region
        Window.IterateNextExpectedElement(element + 1);

        Logger.Debug("AddSingleRecovery: Deleting recovery packets before element ", Window.NextExpectedElement, " column = ", (Window.NextExpectedElement + Window.ColumnStart));

        RecoveryPackets.DeletePacketsBefore(Window.NextExpectedElement);

        if (CheckedRegion.NextCheckStart >= kDecoderRemoveThreshold) {
            Window.RemoveElements();
        }
    }

    return true;
}

bool Decoder::CheckRecoveryPossible()
{
    if (Window.EmergencyDisabled) {
        return false;
    }

    RecoveryPacket* recovery;
    unsigned nextCheckStart, recoveryCount, lostCount;

    // If we just started checking again:
    if (!CheckedRegion.LastRecovery)
    {
        recovery = RecoveryPackets.Head;
        if (!recovery) {
            return false; // No recovery data
        }

        CheckedRegion.FirstRecovery = recovery;
        CheckedRegion.ElementStart  = recovery->ElementStart;
#ifdef SIAMESE_DEBUG
        const unsigned lostPacketsBeforeLDPC = Window.RangeLostPackets(0, recovery->ElementStart);
        SIAMESE_DEBUG_ASSERT(0 == lostPacketsBeforeLDPC);
#endif
        recoveryCount  = 1;
        nextCheckStart = recovery->ElementEnd;
        lostCount      = Window.RangeLostPackets(recovery->ElementStart, nextCheckStart);
        CheckedRegion.SolveFailed = false;

        // Keep track of how many losses this recovery packet is facing
        recovery->LostCount = lostCount;
    }
    else
    {
        recoveryCount = CheckedRegion.RecoveryCount;
        lostCount     = CheckedRegion.LostCount;
        if (recoveryCount >= lostCount && !CheckedRegion.SolveFailed)
        {
            // If maximum loss recovery count is exceeded:
            if (lostCount > kMaximumLossRecoveryCount) {
                return false; // Limit was hit
            }
            return true; // It is already possible
        }

        recovery       = CheckedRegion.LastRecovery;
        nextCheckStart = CheckedRegion.NextCheckStart;
    }
    SIAMESE_DEBUG_ASSERT(lostCount > 0);

    // While we do not have enough recovery data:
    while ((recoveryCount < lostCount || CheckedRegion.SolveFailed) &&
           (recovery->Next != nullptr))
    {
        recovery = recovery->Next;
        ++recoveryCount;

        // Accumulate losses within the range of this recovery packet, skipping
        // losses we've already accumulated into the checked region
        unsigned elementEnd = recovery->ElementEnd;
        if (elementEnd < nextCheckStart) {
            elementEnd = nextCheckStart; // This can happen when interleaved with Cauchy packets
        }
        Logger.Debug("RecoveryPossible? Searching between ", nextCheckStart, " and ", elementEnd);
        lostCount += Window.RangeLostPackets(nextCheckStart, elementEnd);
        SIAMESE_DEBUG_ASSERT(lostCount > 0);
        nextCheckStart = elementEnd;

        // Keep track of how many losses this recovery packet is facing
        recovery->LostCount = lostCount;

        CheckedRegion.SolveFailed = false;
    }

    // Remember state for the next time around
    CheckedRegion.LastRecovery   = recovery;
    CheckedRegion.RecoveryCount  = recoveryCount;
    CheckedRegion.LostCount      = lostCount;
    CheckedRegion.NextCheckStart = nextCheckStart;

    Logger.Debug("RecoveryPossible? LostCount=", CheckedRegion.LostCount, " RecoveryCount=", CheckedRegion.RecoveryCount);

    // If maximum loss recovery count is exceeded:
    if (lostCount > kMaximumLossRecoveryCount) {
        return false; // Limit was hit
    }

    return recoveryCount >= lostCount && !CheckedRegion.SolveFailed;
}

SiameseResult Decoder::Decode(SiameseOriginalPacket** packetsPtrOut, unsigned* countOut)
{
    if (Window.EmergencyDisabled) {
        return Siamese_Disabled;
    }

    // If there are already recovered packets to report:
    if (Window.HasRecoveredPackets)
    {
        Window.HasRecoveredPackets = false;
        SIAMESE_DEBUG_ASSERT(Window.RecoveredPackets.GetSize() != 0);
        if (packetsPtrOut)
        {
            *packetsPtrOut = Window.RecoveredPackets.GetPtr(0);
            *countOut      = Window.RecoveredPackets.GetSize();
        }
        return Siamese_Success;
    }

    // Default return on failure
    if (packetsPtrOut)
    {
        *packetsPtrOut = nullptr;
        *countOut      = 0;
    }

    /*
        The goal of this routine is to determine if solutions to the matrix inverse
        problem exists with minimal work, and to keep track of the data operations
        required to arrive at the solution.  Given the row and column descriptions,
        we generate the square matrix to invert.  Then we use Gaussian Elimination
        while keeping track of what values are multiplied in place of the original
        matrix elements.  If successful, the resulting matrix can be multiplied by
        the recovery data in the solution order to recover the lost data.
    */

    // Advance the checked region to the first possible solution
    if (!CheckRecoveryPossible()) {
        return Siamese_NeedMoreData;
    }

    RecoveryPacket* recovery = CheckedRegion.LastRecovery;
    unsigned nextCheckStart  = CheckedRegion.NextCheckStart;
    unsigned recoveryCount   = CheckedRegion.RecoveryCount;
    unsigned lostCount       = CheckedRegion.LostCount;

    SIAMESE_DEBUG_ASSERT(recovery != nullptr && nextCheckStart > CheckedRegion.ElementStart);
    SIAMESE_DEBUG_ASSERT(lostCount > 0 && lostCount <= recoveryCount);

    for (;;)
    {
        if (recoveryCount >= lostCount)
        {
            SiameseResult result = DecodeCheckedRegion();

            // Pass error or success up; continue on decode failure
            if (result == Siamese_Success)
            {
                if (packetsPtrOut)
                {
                    *packetsPtrOut = Window.RecoveredPackets.GetPtr(0);
                    *countOut      = Window.RecoveredPackets.GetSize();
                }
                return Siamese_Success;
            }

            if (result != Siamese_NeedMoreData) {
                return result;
            }
        }

        if (recovery->Next == nullptr) {
            break;
        }
        recovery = recovery->Next;
        ++recoveryCount;

        // Accumulate losses within the range of this recovery packet, skipping
        // losses we've already accumulated into the checked region
        unsigned elementEnd = recovery->ElementEnd;
        if (elementEnd < nextCheckStart) {
            elementEnd = nextCheckStart; // This can happen when interleaved with Cauchy packets
        }
        lostCount += Window.RangeLostPackets(nextCheckStart, elementEnd);

        // Keep track of how many lost packets this recovery packet is facing
        recovery->LostCount = lostCount;

        nextCheckStart = elementEnd;
    }

    // Remember state for the next time around
    CheckedRegion.LastRecovery   = recovery;
    CheckedRegion.NextCheckStart = nextCheckStart;
    CheckedRegion.RecoveryCount  = recoveryCount;
    CheckedRegion.LostCount      = lostCount;
    return Siamese_NeedMoreData;
}

SiameseResult Decoder::DecodeCheckedRegion()
{
    Logger.Debug("Attempting decode...");

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    bool skipLog = CheckedRegion.LostCount <= 1;
    if (!skipLog)
        Logger.Debug("For ", CheckedRegion.LostCount, " losses:");

    uint64_t t0 = GetTimeUsec();
#endif

    // Generate updated recovery matrix
    if (!RecoveryMatrix.GenerateMatrix())
    {
        Window.EmergencyDisabled = true;
        Logger.Error("DecodeCheckedRegion.GenerateMatrix failed");
        return Siamese_Disabled;
    }

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    uint64_t t1 = GetTimeUsec();
#endif

    // Attempt to solve the linear system
    if (!RecoveryMatrix.GaussianElimination())
    {
        CheckedRegion.SolveFailed = true;
        Stats.Counts[SiameseDecoderStats_SolveFailCount]++;
        return Siamese_NeedMoreData;
    }

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    uint64_t t2 = GetTimeUsec();
#endif

    if (!EliminateOriginalData())
    {
        Window.EmergencyDisabled = true;
        Logger.Error("DecodeCheckedRegion.EliminateOriginalData failed");
        return Siamese_Disabled;
    }

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    uint64_t t3 = GetTimeUsec();
#endif

    if (!MultiplyLowerTriangle())
    {
        Window.EmergencyDisabled = true;
        Logger.Error("DecodeCheckedRegion.MultiplyLowerTriangle failed");
        return Siamese_Disabled;
    }

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    uint64_t t4 = GetTimeUsec();
#endif

    SiameseResult solveResult = BackSubstitution();

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    uint64_t t5 = GetTimeUsec();
#endif

    CheckedRegion.Reset();

#ifdef SIAMESE_DECODER_DUMP_SOLVER_PERF
    uint64_t t6 = GetTimeUsec();

    if (!skipLog)
    {
        Logger.Info("RecoveryMatrix.GenerateMatrix: ", (t1 - t0), " usec");
        Logger.Info("RecoveryMatrix.GaussianElimination: ", (t2 - t1), " usec");
        Logger.Info("EliminateOriginalData: ", (t3 - t2), " usec");
        Logger.Info("MultiplyLowerTriangle: ", (t4 - t3), " usec");
        Logger.Info("BackSubstitution: ", (t5 - t4), " usec");
        Logger.Info("Cleanup: ", (t6 - t5), " usec");
    }
#endif

    return solveResult;
}

bool Decoder::EliminateOriginalData()
{
    SIAMESE_DEBUG_ASSERT(CheckedRegion.LostCount == RecoveryMatrix.Columns.GetSize());

    std::ostringstream* pDebugMsg = nullptr;

    // Note: This is done because the Siamese sums need to be accumulated from
    // left to right in the same order that the encoder generated them.
    // This step tends to be slow because there is a lot of data that was
    // successfully received that we need to eliminate from the recovery sums

    const unsigned rows = CheckedRegion.RecoveryCount;
    SIAMESE_DEBUG_ASSERT(CheckedRegion.RecoveryCount == RecoveryMatrix.Rows.GetSize());

    // Eliminate data in sorted row order regardless of pivot order:
    for (unsigned matrixRowIndex = 0; matrixRowIndex < rows; ++matrixRowIndex)
    {
        if (!RecoveryMatrix.Rows.GetRef(matrixRowIndex).UsedForSolution) {
            continue;
        }

        RecoveryPacket* recovery        = RecoveryMatrix.Rows.GetRef(matrixRowIndex).Recovery;
        const RecoveryMetadata metadata = recovery->Metadata;
        const unsigned elementStart     = recovery->ElementStart;
        const unsigned elementEnd       = recovery->ElementEnd;
        GrowingAlignedDataBuffer& recoveryBuffer = recovery->Buffer;
        SIAMESE_DEBUG_ASSERT(recoveryBuffer.Data && recoveryBuffer.Bytes > 0);

#ifdef SIAMESE_ENABLE_CAUCHY
        // If it is a Cauchy or parity row:
        if (metadata.SumCount <= SIAMESE_CAUCHY_THRESHOLD)
        {
            // If this is a parity row:
            if (metadata.Row == 0)
            {
                // Fill columns from left for new rows:
                for (unsigned j = elementStart; j < elementEnd; ++j)
                {
                    OriginalPacket* original = Window.GetWindowElement(j);
                    unsigned addBytes = original->Buffer.Bytes;
                    if (addBytes > 0)
                    {
                        if (addBytes > recoveryBuffer.Bytes) {
                            SIAMESE_DEBUG_BREAK(); // Should never happen
                            addBytes = recoveryBuffer.Bytes;
                        }
                        gf256_add_mem(recoveryBuffer.Data, original->Buffer.Data, addBytes);
                    }
                }
            }
            else // This is a Cauchy row:
            {
                // Fill columns from left for new rows:
                for (unsigned j = elementStart; j < elementEnd; ++j)
                {
                    OriginalPacket* original = Window.GetWindowElement(j);
                    unsigned addBytes = original->Buffer.Bytes;
                    if (addBytes > 0)
                    {
                        const uint8_t y = CauchyElement(metadata.Row - 1, original->Column % kCauchyMaxColumns);
                        if (addBytes > recoveryBuffer.Bytes) {
                            SIAMESE_DEBUG_BREAK(); // Should never happen
                            addBytes = recoveryBuffer.Bytes;
                        }
                        gf256_muladd_mem(recoveryBuffer.Data, y, original->Buffer.Data, addBytes);
                    }
                }
            }

            continue;
        }
#endif // SIAMESE_ENABLE_CAUCHY

        // Zero the product sum
        const unsigned recoveryBytes = recoveryBuffer.Bytes;
        if (!ProductSum.Initialize(&TheAllocator, recoveryBytes)) {
            return false;
        }
        memset(ProductSum.Data, 0, recoveryBytes);

        Logger.Debug("Starting sums for row=", recovery->Metadata.Row, " start=", recovery->Metadata.ColumnStart, " count=", recovery->Metadata.SumCount);

        // Convert column start to window element.
        // If some of the summed elements have fallen out of the window,
        // then start it at the first element in the window (0).
        unsigned sumElementStart = Window.ColumnToElement(recovery->Metadata.ColumnStart);

        /*
            If the recovery packet indicates a different siamese sum, then we
            will need to clear the running sums and recreate them from scratch.

            Sums need to be restarted if the start point changed, which should
            be a jump of over 128 packets that were acknowledged.  They would
            also need to be restarted if the number of summed columns has
            reduced instead of increased.  In both cases, data needs to be
            removed from the running sum.  Instead of being clever about how to
            remove that data, we just start over from scratch to avoid a huge
            amount of extra complexity.

            TBD: Collect real data on how much it would help to checkpoint on
            the decoder side.  I suspect it would not help much because multi-
            packet losses probably do not often straddle these checkpoints.
            For example if start point changes 1/128 packets, then the benefit
            of checkpoints is only felt in about 1% of the multi-loss cases,
            which are already much less common than single losses.

            Due to the way we order recovery packets in the list, and therefore
            how they get ordered as matrix rows for the matrix we are solving,
            often times sums will only roll forward or skip ahead.
        */
        if (metadata.ColumnStart != Window.SumColumnStart ||
            metadata.SumCount < Window.SumColumnCount)
        {
            // If we have to restart the sums but the data is not available:
            if (Window.InvalidElement(sumElementStart)) {
                // This should never happen.  The code that decides when to
                // remove data from the window should have kept this data.
                SIAMESE_DEBUG_BREAK();
                return false;
            }

            Window.ResetSums(sumElementStart);
            Window.SumColumnStart = metadata.ColumnStart;
        }
        else
        {
            if (Window.InvalidElement(sumElementStart)) {
                sumElementStart = 0;
            }

            // Prepare any lane sums that have not accumulated data yet to
            // receive data, since we are about to start accumulaing into
            // some of these running sums.
            if (!Window.StartSums(sumElementStart, recoveryBytes)) {
                return false;
            }
        }
        Window.SumColumnCount = metadata.SumCount;

        // Eliminate dense recovery data outside of matrix:
        for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
        {
            const unsigned opcode = GetRowOpcode(laneIndex, metadata.Row);

            // For summations into the RecoveryPacket buffer:
            unsigned mask = 1;
            for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
            {
                if (opcode & mask)
                {
                    const GrowingAlignedDataBuffer* sum = Window.GetSum(laneIndex, sumIndex, elementEnd);
                    SIAMESE_DEBUG_ASSERT(elementEnd + kColumnLaneCount >= Window.Lanes[laneIndex].Sums[sumIndex].ElementEnd);
                    unsigned addBytes = sum->Bytes;
                    if (addBytes > 0)
                    {
                        if (addBytes > recoveryBytes) {
                            addBytes = recoveryBytes;
                        }
                        gf256_add_mem(recoveryBuffer.Data, sum->Data, addBytes);
                    }
                }
                mask <<= 1;
            }

            // For summations into the ProductWorkspace buffer:
            for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
            {
                if (opcode & mask)
                {
                    const GrowingAlignedDataBuffer* sum = Window.GetSum(laneIndex, sumIndex, elementEnd);
                    SIAMESE_DEBUG_ASSERT(elementEnd + kColumnLaneCount >= Window.Lanes[laneIndex].Sums[sumIndex].ElementEnd);
                    unsigned addBytes = sum->Bytes;
                    if (addBytes > 0)
                    {
                        if (addBytes > recoveryBytes)
                            addBytes = recoveryBytes;
                        gf256_add_mem(ProductSum.Data, sum->Data, addBytes);
                    }
                }
                mask <<= 1;
            }
        }

        // Eliminate light recovery data outside of matrix:
        PCGRandom prng;
        prng.Seed(metadata.Row, metadata.LDPCCount);
        SIAMESE_DEBUG_ASSERT(metadata.SumCount >= metadata.LDPCCount);

        if (Logger.ShouldLog(logger::Level::Debug))
        {
            delete pDebugMsg;
            pDebugMsg = new std::ostringstream();
            *pDebugMsg << "(Eliminate originals) LDPC columns (*=missing): ";
        }

        const unsigned pairCount = (metadata.LDPCCount + kPairAddRate - 1) / kPairAddRate;
        for (unsigned i = 0; i < pairCount; ++i)
        {
            const unsigned element1   = elementStart + (prng.Next() % metadata.LDPCCount);
            OriginalPacket* original1 = Window.GetWindowElement(element1);
            unsigned addBytes1 = original1->Buffer.Bytes;
            if (addBytes1 > 0)
            {
                if (addBytes1 > recoveryBytes)
                {
                    SIAMESE_DEBUG_BREAK(); // Should never happen
                    addBytes1 = recoveryBytes;
                }
                gf256_add_mem(recoveryBuffer.Data, original1->Buffer.Data, addBytes1);

                if (pDebugMsg)
                    *pDebugMsg << element1 << " ";
            }
            else
            {
                if (pDebugMsg)
                    *pDebugMsg << element1 << "* ";
            }

            const unsigned elementRX   = elementStart + (prng.Next() % metadata.LDPCCount);
            OriginalPacket* originalRX = Window.GetWindowElement(elementRX);
            unsigned addBytesRX = originalRX->Buffer.Bytes;
            if (addBytesRX > 0)
            {
                if (addBytesRX > recoveryBytes)
                {
                    SIAMESE_DEBUG_BREAK(); // Should never happen
                    addBytesRX = recoveryBytes;
                }
                gf256_add_mem(ProductSum.Data, originalRX->Buffer.Data, addBytesRX);

                if (pDebugMsg)
                    *pDebugMsg << elementRX << " ";
            }
            else
            {
                if (pDebugMsg)
                    *pDebugMsg << elementRX << "* ";
            }
        }

        if (pDebugMsg)
            Logger.Debug(pDebugMsg->str());

        SIAMESE_DEBUG_ASSERT(recoveryBuffer.Bytes == ProductSum.Bytes);
        const uint8_t RX = GetRowValue(metadata.Row);
        gf256_muladd_mem(recoveryBuffer.Data, RX, ProductSum.Data, ProductSum.Bytes);
    }

    // Return false if GetSum() ran out of memory
    return !Window.EmergencyDisabled;
}

bool Decoder::MultiplyLowerTriangle()
{
    // Note: This step tends to be slow because it is a dense triangular
    // matrix-vector product

    const unsigned columns = CheckedRegion.LostCount;

    // Multiply lower triangle following solution order from left to right:
    for (unsigned col_i = 0; col_i < columns - 1; ++col_i)
    {
        const unsigned matrixRowIndex_i = RecoveryMatrix.Pivots.GetRef(col_i);
        const GrowingAlignedDataBuffer& recovery_i = RecoveryMatrix.Rows.GetRef(matrixRowIndex_i).Recovery->Buffer;
        const uint8_t* srcData = recovery_i.Data;
        const unsigned srcBytes = recovery_i.Bytes;
        SIAMESE_DEBUG_ASSERT(srcData && srcBytes > 0);

        for (unsigned col_j = col_i + 1; col_j < columns; ++col_j)
        {
            const unsigned matrixRowIndex_j = RecoveryMatrix.Pivots.GetRef(col_j);
            const uint8_t y = RecoveryMatrix.Matrix.Get(matrixRowIndex_j, col_i);

            // If this row does not reference this column:
            if (y == 0) {
                continue;
            }

            GrowingAlignedDataBuffer& recovery_j = RecoveryMatrix.Rows.GetRef(matrixRowIndex_j).Recovery->Buffer;
            SIAMESE_DEBUG_ASSERT(recovery_j.Data && recovery_j.Bytes > 0);

            // Make room for the summation
            if (!recovery_j.GrowZeroPadded(&TheAllocator, srcBytes)) {
                return false;
            }

            gf256_muladd_mem(recovery_j.Data, y, srcData, srcBytes);
        }
    }

    return true;
}

SiameseResult Decoder::BackSubstitution()
{
    // Note: This step tends to be fast because the upper-right of the matrix
    // while streaming is mostly zero

    const unsigned columns = CheckedRegion.LostCount;
    Window.RecoveredPackets.SetSize_NoCopy(columns);

    bool iterateNextExpected = false;

    // For each column starting with the right-most column:
    for (int col_i = columns - 1; col_i >= 0; --col_i)
    {
        const unsigned matrixRowIndex = RecoveryMatrix.Pivots.GetRef(col_i);
        OriginalPacket* original = RecoveryMatrix.Columns.GetRef(col_i).Original;
        RecoveryPacket* recovery = RecoveryMatrix.Rows.GetRef(matrixRowIndex).Recovery;
        SIAMESE_DEBUG_ASSERT(original->Column == (unsigned)col_i && original->Buffer.Bytes == 0);

        uint8_t* buffer = recovery->Buffer.Data;
        const uint8_t y = RecoveryMatrix.Matrix.Get(matrixRowIndex, col_i);

        SIAMESE_DEBUG_ASSERT(buffer && recovery->Buffer.Bytes > 0);
        SIAMESE_DEBUG_ASSERT(y != 0);
        const uint8_t inv_y = gf256_inv(y);

        // Reveal the first chunk of bytes of data
        unsigned bufferBytes = recovery->Buffer.Bytes;
        unsigned lengthCheckBytes = pktalloc::kAlignmentBytes;
        if (lengthCheckBytes > bufferBytes) {
            lengthCheckBytes = bufferBytes;
        }
        gf256_mul_mem(buffer, buffer, inv_y, lengthCheckBytes);

        // Check the embedded length field
        unsigned length;
        int headerBytes = DeserializeHeader_PacketLength(buffer, lengthCheckBytes, length);
        if (headerBytes < 0 || length == 0 || headerBytes + length > bufferBytes)
        {
            //------------------------------------------------------------------
            // This error means that the Siamese FEC recovery has failed.
            // Common causes:
            // + Packet Numbers provided by application are incorrect.
            // + Or some software bug in this library I need to fix.
            //------------------------------------------------------------------
            Window.EmergencyDisabled = true;
            Logger.Error("BackSubstitution corrupted recovered data len");
            SIAMESE_DEBUG_BREAK(); // Should never happen
            return Siamese_Disabled;
        }

        // Reduce buffer bytes to only cover the original packet data
        bufferBytes = headerBytes + length;
        if (bufferBytes > lengthCheckBytes) {
            gf256_mul_mem(
                buffer + lengthCheckBytes,
                buffer + lengthCheckBytes,
                inv_y,
                bufferBytes - lengthCheckBytes);
        }

        // Swap original and recovery buffers
        uint8_t* oldOriginalData = original->Buffer.Data;
        original->Buffer.Data    = buffer;
        original->Buffer.Bytes   = bufferBytes;
        original->Column         = RecoveryMatrix.Columns.GetRef(col_i).Column;
        original->HeaderBytes    = (unsigned)headerBytes;
        recovery->Buffer.Data    = oldOriginalData;
        recovery->Buffer.Bytes   = 0;

        // Write recovered packet data
        SiameseOriginalPacket* recoveredPtr = Window.RecoveredPackets.GetPtr(col_i);
        recoveredPtr->Data      = buffer + headerBytes;
        recoveredPtr->DataBytes = length;
        recoveredPtr->PacketNum = original->Column;

        if (!Window.RecoveredColumns.Append(original->Column))
        {
            Window.EmergencyDisabled = true;
            SIAMESE_DEBUG_BREAK(); // OOM
            return Siamese_Disabled;
        }

        Logger.Trace("GE Decoded: Column=", original->Column, " Row=", recovery->Metadata.Row);

        iterateNextExpected |= Window.MarkGotColumn(original->Column);

        // Eliminate from all other pivot rows above it:
        for (unsigned col_j = 0; col_j < (unsigned)col_i; ++col_j)
        {
            unsigned pivot_j = RecoveryMatrix.Pivots.GetRef(col_j);
            const uint8_t x  = RecoveryMatrix.Matrix.Get(pivot_j, col_i);

            if (x == 0) {
                continue;
            }

            const GrowingAlignedDataBuffer& buffer_j = RecoveryMatrix.Rows.GetRef(pivot_j).Recovery->Buffer;
            SIAMESE_DEBUG_ASSERT(buffer_j.Data && buffer_j.Bytes > 0);

            unsigned addBytes = bufferBytes;
            if (addBytes > buffer_j.Bytes) {
                SIAMESE_DEBUG_BREAK(); // This should never happen
                addBytes = buffer_j.Bytes;
            }

            gf256_muladd_mem(buffer_j.Data, x, buffer, addBytes);
        }
    }

    // We always expect to have recovered the next expected packet
    if (!iterateNextExpected)
    {
        Window.EmergencyDisabled = true;
        Logger.Error("BackSubstitution.iterateNextExpected failed");
        SIAMESE_DEBUG_BREAK(); // Should never happen
        return Siamese_Disabled;
    }

    // Iterate the next expected element beyond the recovery region
    Window.IterateNextExpectedElement(CheckedRegion.NextCheckStart);

    Logger.Debug("BackSubstitution: Deleting recovery packets before element ", Window.NextExpectedElement, " column = ", (Window.NextExpectedElement + Window.ColumnStart));

    RecoveryPackets.DeletePacketsBefore(Window.NextExpectedElement);

    if (CheckedRegion.NextCheckStart >= kDecoderRemoveThreshold) {
        Window.RemoveElements();
    }

    Stats.Counts[SiameseDecoderStats_SolveSuccessCount]++;

    return Siamese_Success;
}

SiameseResult Decoder::GetStatistics(uint64_t* statsOut, unsigned statsCount)
{
    if (statsCount > SiameseDecoderStats_Count) {
        statsCount = SiameseDecoderStats_Count;
    }

    // Fill in memory allocated
    Stats.Counts[SiameseDecoderStats_MemoryUsed] = TheAllocator.GetMemoryAllocatedBytes();

    for (unsigned i = 0; i < statsCount; ++i) {
        statsOut[i] = Stats.Counts[i];
    }

    return Siamese_Success;
}


//------------------------------------------------------------------------------
// DecoderPacketWindow

bool DecoderPacketWindow::MarkGotColumn(unsigned column)
{
    // Convert to window element
    const unsigned element = ColumnToElement(column);
    if (InvalidElement(element))
    {
        EmergencyDisabled = true;
        Logger.Error("MarkGotColumn failed");
        SIAMESE_DEBUG_BREAK(); // Should never happen
        return false;
    }

    DecoderSubwindow* subwindow = Subwindows.GetRef(element / kSubwindowSize);
    subwindow->GotCount++;
    subwindow->Got.Set(element % kSubwindowSize);

    return (element == NextExpectedElement);
}

unsigned DecoderPacketWindow::RangeLostPackets(unsigned elementStart, unsigned elementEnd)
{
    if (elementStart >= elementEnd) {
        return 0;
    }

    unsigned lostCount = 0;

    // Accumulate first partial subwindow (if any)
    unsigned subwindowStart = elementStart / kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(subwindowStart < Subwindows.GetSize());
    const unsigned bitStart = elementStart % kSubwindowSize;
    if (bitStart > 0)
    {
        unsigned bitEnd = bitStart + elementEnd - elementStart;
        if (bitEnd > kSubwindowSize) {
            bitEnd = kSubwindowSize;
        }
        const unsigned bitMaxSet = bitEnd - bitStart; // Bit count in range
        lostCount += bitMaxSet - Subwindows.GetRef(subwindowStart)->Got.RangePopcount(bitStart, bitEnd);
        ++subwindowStart;
    }

    // Accumulate whole subwindows of losses
    const unsigned subwindowEnd = elementEnd / kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(subwindowEnd <= Subwindows.GetSize());
    for (unsigned i = subwindowStart; i < subwindowEnd; ++i) {
        lostCount += kSubwindowSize - Subwindows.GetRef(i)->GotCount;
    }

    // Accumulate last partial subwindow (if any, common case)
    if (subwindowEnd >= subwindowStart)
    {
        const unsigned lastSubwindowBits = elementEnd - subwindowEnd * kSubwindowSize;

        if (lastSubwindowBits > 0)
        {
            const unsigned gotCount = Subwindows.GetRef(subwindowEnd)->Got.RangePopcount(0, lastSubwindowBits);

            lostCount += lastSubwindowBits - gotCount;
        }
    }

    return lostCount;
}

unsigned DecoderPacketWindow::FindNextLostElement(unsigned elementStart)
{
    if (elementStart >= Count) {
        return Count;
    }

    const unsigned subwindowEnd = (Count + kSubwindowSize - 1) / kSubwindowSize;
    unsigned subwindowIndex = elementStart / kSubwindowSize;
    unsigned bitIndex = elementStart % kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(subwindowEnd <= Subwindows.GetSize());
    SIAMESE_DEBUG_ASSERT(subwindowIndex < Subwindows.GetSize());

    while (subwindowIndex < subwindowEnd)
    {
        // If there may be any lost packets in this subwindow:
        if (Subwindows.GetRef(subwindowIndex)->GotCount < kSubwindowSize)
        {
            for (;;)
            {
                // Seek next clear bit
                bitIndex = Subwindows.GetRef(subwindowIndex)->Got.FindFirstClear(bitIndex);

                // If there were none, skip this subwindow
                if (bitIndex >= kSubwindowSize) {
                    break;
                }

                // Calculate element index and stop if we hit the end of the valid data
                unsigned nextElement = subwindowIndex * kSubwindowSize + bitIndex;
                if (nextElement > Count) {
                    nextElement = Count;
                }

                return nextElement;
            }
        }

        // Reset bit index to the front of the next subwindow
        bitIndex = 0;

        // Check next subwindow
        ++subwindowIndex;
    }

    return Count;
}

unsigned DecoderPacketWindow::FindNextGotElement(unsigned elementStart)
{
    if (elementStart >= Count) {
        return Count;
    }

    const unsigned subwindowEnd = (Count + kSubwindowSize - 1) / kSubwindowSize;
    unsigned subwindowIndex = elementStart / kSubwindowSize;
    unsigned bitIndex = elementStart % kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(subwindowEnd <= Subwindows.GetSize());
    SIAMESE_DEBUG_ASSERT(subwindowIndex < Subwindows.GetSize());

    while (subwindowIndex < subwindowEnd)
    {
        // If there may be any got packets in this subwindow:
        if (Subwindows.GetRef(subwindowIndex)->GotCount > 0)
        {
            for (;;)
            {
                // Seek next set bit
                bitIndex = Subwindows.GetRef(subwindowIndex)->Got.FindFirstSet(bitIndex);

                // If there were none, skip this subwindow
                if (bitIndex >= kSubwindowSize) {
                    break;
                }

                // Calculate element index and stop if we hit the end of the valid data
                unsigned nextElement = subwindowIndex * kSubwindowSize + bitIndex;
                if (nextElement > Count) {
                    nextElement = Count;
                }

                return nextElement;
            }
        }

        // Reset bit index to the front of the next subwindow
        bitIndex = 0;

        // Check next subwindow
        ++subwindowIndex;
    }

    return Count;
}

void DecoderPacketWindow::IterateNextExpectedElement(unsigned elementStart)
{
    SIAMESE_DEBUG_ASSERT(elementStart > NextExpectedElement);
    if (NextExpectedElement >= Count) {
        return;
    }

    SIAMESE_DEBUG_ASSERT(RangeLostPackets(0, NextExpectedElement) == 0);
    SIAMESE_DEBUG_ASSERT(RangeLostPackets(NextExpectedElement, elementStart) == 0);

    const unsigned nextLostElement = FindNextLostElement(elementStart);

    SIAMESE_DEBUG_ASSERT(RangeLostPackets(elementStart, nextLostElement) == 0);

    NextExpectedElement = nextLostElement;
}

bool DecoderPacketWindow::GrowWindow(const unsigned windowElementEnd)
{
    // Note: Adding a buffer of lane count to create space ahead for snapshots
    // as a subwindow is filled and we need to store its snapshot
    const unsigned subwindowCount   = Subwindows.GetSize();
    const unsigned subwindowsNeeded = (windowElementEnd + kColumnLaneCount + kSubwindowSize - 1) / kSubwindowSize;

    if (subwindowsNeeded > subwindowCount)
    {
        // Note resizing larger will keep old data in the vector
        if (!Subwindows.SetSize_Copy(subwindowsNeeded))
            return false;

        // For each subwindow to initialize:
        for (unsigned i = subwindowCount; i < subwindowsNeeded; ++i)
        {
            DecoderSubwindow* subwindow = TheAllocator->Construct<DecoderSubwindow>();
            if (!subwindow)
                return false; // Out of memory

            Subwindows.GetRef(i) = subwindow;
        }
    }

    // If this element expands the window:
    if (windowElementEnd > Count)
        Count = windowElementEnd;

    return true;
}

SiameseResult DecoderPacketWindow::AddOriginal(const SiameseOriginalPacket& packet)
{
    if (EmergencyDisabled)
        return Siamese_Disabled;

    SIAMESE_DEBUG_ASSERT(packet.Data && packet.DataBytes > 0);
    const unsigned element = ColumnToElement(packet.PacketNum);

    // If we just received an old element before our window:
    if (IsColumnDeltaNegative(element))
    {
        Logger.Debug("Ignored an old packet before window start: ", packet.PacketNum);
        Stats->Counts[SiameseDecoderStats_DupedOriginalCount]++;
        return Siamese_DuplicateData;
    }

    if (!GrowWindow(element + 1))
    {
        EmergencyDisabled = true;
        Logger.Error("AddOriginal.GrowWindow OOM");
        return Siamese_Disabled;
    }

    // Grab the window element for this packet
    DecoderSubwindow* subwindowPtr  = Subwindows.GetRef(element / kSubwindowSize);
    const unsigned subwindowElement = element % kSubwindowSize;

    OriginalPacket* original = &subwindowPtr->Originals[subwindowElement];
    if (original->Buffer.Bytes > 0)
    {
        Logger.Debug("Ignored a packet already received: ", packet.PacketNum);
        Stats->Counts[SiameseDecoderStats_DupedOriginalCount]++;
        return Siamese_DuplicateData;
    }

    // Make space for the packet data
    if (0 == original->Initialize(TheAllocator, packet))
    {
        EmergencyDisabled = true;
        Logger.Error("AddOriginal.Initialize OOM");
        return Siamese_Disabled;
    }
    SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes > 1);

    // Increment the number of packets filled in for this subwindow
    subwindowPtr->GotCount++;
    subwindowPtr->Got.Set(subwindowElement);

    // If this was the next expected element:
    if (element == NextExpectedElement)
    {
        IterateNextExpectedElement(element + 1);

        Logger.Debug("AddOriginal: Deleting recovery packets before element ", NextExpectedElement, " column = ", (NextExpectedElement + ColumnStart));

        RecoveryPackets->DeletePacketsBefore(NextExpectedElement);
    }

    // If the added element is somewhere inside the previously checked region:
    if (element >= CheckedRegion->ElementStart &&
        element < CheckedRegion->NextCheckStart)
    {
        CheckedRegion->Reset();
    }

    Stats->Counts[SiameseDecoderStats_OriginalCount]++;
    Stats->Counts[SiameseDecoderStats_OriginalBytes] += packet.DataBytes;

    return Siamese_Success;
}

bool DecoderPacketWindow::PlugSumHoles(unsigned elementStart)
{
    const unsigned recoveredCount = RecoveredColumns.GetSize();
    SIAMESE_DEBUG_ASSERT(recoveredCount > 0);

    // Use previously recovered packets to plug holes in the sums:
    for (unsigned i = 0; i < recoveredCount; ++i)
    {
        const unsigned column  = RecoveredColumns.GetRef(i);
        const unsigned element = ColumnToElement(column);

        // If recovered data was far in the past:
        if (InvalidElement(element))
            continue;

        const unsigned laneIndex        = column % kColumnLaneCount;
        const unsigned laneElementStart = GetNextLaneElement(elementStart, laneIndex);

        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            DecoderSum& sum = Lanes[laneIndex].Sums[sumIndex];

            // If this element fills in a hole in the new sum:
            if (element >= laneElementStart && element < sum.ElementEnd)
            {
                const OriginalPacket* original = GetWindowElement(element);
                unsigned originalBytes         = original->Buffer.Bytes;
                SIAMESE_DEBUG_ASSERT(original->Column == column);

                if (originalBytes <= 0)
                {
                    SIAMESE_DEBUG_BREAK(); // Should never happen
                    return false;
                }

                if (originalBytes > sum.Buffer.Bytes)
                {
                    // Grow sum to encompass the original data
                    if (!sum.Buffer.GrowZeroPadded(TheAllocator, originalBytes))
                        return false;
                }

                // Sum += PacketData
                if (sumIndex == 0)
                    gf256_add_mem(sum.Buffer.Data, original->Buffer.Data, originalBytes);
                else
                {
                    uint8_t CX = GetColumnValue(column);
                    if (sumIndex == 2)
                        CX = gf256_sqr(CX);

                    // Sum += CX * PacketData
                    gf256_muladd_mem(sum.Buffer.Data, CX, original->Buffer.Data, originalBytes);
                }

                Logger.Debug("Filled hole in sum for ", laneIndex, " sum ", sumIndex, " at column ", element + ColumnStart);
            }
        }
    }

    // Clear recovered packets to avoid double-plugging holes in the sums
    RecoveredColumns.Clear();

    return true;
}

void DecoderPacketWindow::ResetSums(unsigned elementStart)
{
    Logger.Info("ResetSums at ", elementStart);

    // For each lane:
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        // Find the next window element that is in this lane beginning at `elementStart`
        const unsigned laneElementStart = GetNextLaneElement(elementStart, laneIndex);

        // For each of the sums in the lane:
        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            // Clear the buffer and element start/end range
            DecoderSum& sum  = Lanes[laneIndex].Sums[sumIndex];
            sum.ElementStart = laneElementStart;
            sum.ElementEnd   = laneElementStart;
            sum.Buffer.Bytes = 0;
        }
    }

    // Clear recovered packets to avoid double-plugging holes in the sums
    RecoveredColumns.Clear();
}

bool DecoderPacketWindow::StartSums(unsigned elementStart, unsigned bufferBytes)
{
    for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
    {
        // Get the next window element in this lane beginning with `elementStart`
        const unsigned laneElementStart = GetNextLaneElement(elementStart, laneIndex);

        for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
        {
            DecoderSum& sum = Lanes[laneIndex].Sums[sumIndex];

            // If the sum contains no data or starts in a different place:
            if (sum.Buffer.Bytes == 0)
            {
                Logger.Debug("Re-Restarting sum for ", laneIndex, " sum ", sumIndex, " at column ", laneElementStart + ColumnStart, " current sum bytes = ", sum.Buffer.Bytes);

                sum.ElementEnd = laneElementStart;
            }
            else if (sum.ElementStart != laneElementStart)
            {
                Logger.Debug("Restarting sum for ", laneIndex, " sum ", sumIndex, " at column ", laneElementStart + ColumnStart, " current sum bytes = ", sum.Buffer.Bytes);

                sum.ElementEnd = laneElementStart;
                sum.Buffer.Bytes = 0;
            }

            // Update the start element
            sum.ElementStart = laneElementStart;

            // Grow and zero pad
            if (!sum.Buffer.GrowZeroPadded(TheAllocator, bufferBytes)) {
                return false;
            }

            SIAMESE_DEBUG_ASSERT((sum.ElementStart + ColumnStart) % kColumnLaneCount == laneIndex);
            SIAMESE_DEBUG_ASSERT((sum.ElementEnd   + ColumnStart) % kColumnLaneCount == laneIndex);
        }
    }

    // If we have previously recovered packets, use them to plug holes in the sums:
    if (RecoveredColumns.GetSize() != 0) {
        // If holes cannot be plugged:
        if (!PlugSumHoles(elementStart)) {
            SIAMESE_DEBUG_BREAK(); // Should never happen
            return false;
        }
    }

    return true;
}

const GrowingAlignedDataBuffer* DecoderPacketWindow::GetSum(unsigned laneIndex, unsigned sumIndex, unsigned elementEnd)
{
    DecoderSum& sum = Lanes[laneIndex].Sums[sumIndex];

    SIAMESE_DEBUG_ASSERT(sum.ElementStart <= sum.ElementEnd);
    SIAMESE_DEBUG_ASSERT((sum.ElementStart + ColumnStart) % kColumnLaneCount == laneIndex);
    SIAMESE_DEBUG_ASSERT((sum.ElementEnd + ColumnStart) % kColumnLaneCount == laneIndex);

    unsigned element = sum.ElementEnd;
    if (element >= elementEnd) {
        return &sum.Buffer;
    }

    // For each element to accumulate in this lane:
    do
    {
        SIAMESE_DEBUG_ASSERT((element + ColumnStart) % kColumnLaneCount == laneIndex);
        OriginalPacket* original     = GetWindowElement(element);
        const unsigned originalBytes = original->Buffer.Bytes;

        Logger.Info("Lane ", laneIndex,  " sum ",  sumIndex,  " accumulating column: ",  element + ColumnStart,  ". Got = ",  (originalBytes > 0));

        if (originalBytes > 0)
        {
            SIAMESE_DEBUG_ASSERT(original->Column % kColumnLaneCount == laneIndex);
            if (originalBytes > sum.Buffer.Bytes)
            {
                // Grow sum to encompass the original data
                if (!sum.Buffer.GrowZeroPadded(TheAllocator, originalBytes))
                {
                    EmergencyDisabled = true;
                    goto ExitSum;
                }
            }

            // Sum += PacketData
            if (sumIndex == 0)
                gf256_add_mem(sum.Buffer.Data, original->Buffer.Data, originalBytes);
            else
            {
                uint8_t CX = GetColumnValue(original->Column);
                if (sumIndex == 2)
                    CX = gf256_sqr(CX);

                // Sum += CX * PacketData
                gf256_muladd_mem(sum.Buffer.Data, CX, original->Buffer.Data, originalBytes);
            }
        }

        SIAMESE_DEBUG_ASSERT(original->Buffer.Bytes == 0 || original->Column % kColumnLaneCount == laneIndex);
        element += kColumnLaneCount;
    } while (element < elementEnd);

    SIAMESE_DEBUG_ASSERT((element + ColumnStart) % kColumnLaneCount == laneIndex);

    sum.ElementEnd = element;

ExitSum:
    return &sum.Buffer;
}

/*
    We need to eventually remove elements from the window to avoid using
    a lot of memory.  One easy way to simplify this would be to wait
    until there are no recovery packets referencing lost data.  But if
    there are continuous packet losses, we may never remove any data from
    the window because there may always be more losses to fix.

    The encoder will keep the sums running for a very long time if data
    is constantly being lost - up to about 16000 packets.

    When removing packets from the window, we may need to accumulate the
    packet data into a running sum so that it can be used later.


    Worked examples:

    The recovery rows may look like this:
    3 4 5 6 7 8 ... 128 129 130
    3 4 5 6 7 8 ... 128 129 130 131

    In this case we can remove up to element 128 and roll up the sums.
    This assumes that we will not receive a recovery row that starts the sums
    over at an element before 128.  We can tell that the encoder will not
    choose to start sums before 128 if the latest LDPC range it picked starts
    after 128.  And in general, any LDPC/Cauchy ranges cannot be clipped.

    The recovery rows may look like this:
    3 4 5 6 7 8 ... 128 129 130
    3 4 5 6 7 8 ... 128 129 130 131
          6 7 8 ... 128 129 130 131 132
          6 7 8 ... 128 129 130 131 132 133

    When solving we will need to restart at 6 to compute the third and the
    fourth row.  This means we must keep 6 and later, even if the LDPC count
    for these rows only includes the last few packets.
*/

void DecoderPacketWindow::RemoveElements()
{
    // Quick sanity check to make sure we keep some elements around
    if (NextExpectedElement < kDecoderRemoveThreshold) {
        return;
    }

    // Calculate the earliest LDPC element
    unsigned firstKeptElement = 0;

    // Calculate the sum start column that we would like to be using
    // based on the recovery data received so far.  This is used to
    // start a running sum if one is currently not in progress
    unsigned targetSumStartColumn = 0;
    unsigned targetSumColumnCount = 0;
    unsigned initialRecoveryBytes = 0;
    bool seenSum = false;

    // If there are no recovery packets in the list:
    const RecoveryPacket* recovery = RecoveryPackets->Head;
    if (!recovery)
    {
        const RecoveryMetadata metadata = RecoveryPackets->LastRecoveryMetadata;
        const unsigned elementEnd = ColumnToElement(metadata.ColumnStart + metadata.SumCount);

        // If the LDPC data was already clipped:
        if (IsColumnDeltaNegative(elementEnd) ||
            elementEnd < metadata.LDPCCount)
        {
            SIAMESE_DEBUG_BREAK(); // Should never happen
            Logger.Error("LastRecoveryMetadata column ", metadata.ColumnStart, " was clipped");
            EmergencyDisabled = true;
            return;
        }

        firstKeptElement = elementEnd - metadata.LDPCCount;
        targetSumStartColumn = metadata.ColumnStart;
        targetSumColumnCount = metadata.SumCount;
        initialRecoveryBytes = RecoveryPackets->LastRecoveryBytes;

#ifdef SIAMESE_ENABLE_CAUCHY
        if (metadata.SumCount > SIAMESE_CAUCHY_THRESHOLD)
#endif
        {
            seenSum = true;
        }
    }
    else
    {
        firstKeptElement = recovery->ElementStart;
        initialRecoveryBytes = recovery->Buffer.Bytes;

        // For each received recovery packet:
        for (;;)
        {
            const unsigned sumCount = recovery->Metadata.SumCount;
            const unsigned columnStart = recovery->Metadata.ColumnStart;
#ifdef SIAMESE_ENABLE_CAUCHY
            if (sumCount > SIAMESE_CAUCHY_THRESHOLD)
#endif
            {
                if (!seenSum)
                {
                    // This should only take the first sum start/count into
                    // account because if we accumulate any data it will be
                    // for the first row in the matrix that is running sums.
                    targetSumStartColumn = columnStart;
                    targetSumColumnCount = sumCount;
                    seenSum = true;
                }
                // If there is a second sum start column:
                else if (columnStart != targetSumStartColumn ||
                         sumCount < targetSumColumnCount)
                {
                    unsigned firstSumElement = ColumnToElement(columnStart);

                    // Check if we clipped the second start column:
                    if (InvalidElement(firstSumElement))
                    {
                        EmergencyDisabled = true;
                        Logger.Error("RemoveElements failed: Two start columns and one was clipped");
                        SIAMESE_DEBUG_BREAK(); // Should never happen
                        return;
                    }

                    // If there are two start columns then we cannot clip the
                    // second one, so keep it in the window.
                    if (firstKeptElement > firstSumElement) {
                        firstKeptElement = firstSumElement;
                    }
                }
            }

            // Try next recovery packet
            recovery = recovery->Next;
            if (!recovery) {
                break;
            }

            // If we found an earlier element, use it:
            if (firstKeptElement > recovery->ElementStart) {
                firstKeptElement = recovery->ElementStart;
            }
            if (initialRecoveryBytes < recovery->Buffer.Bytes) {
                initialRecoveryBytes = recovery->Buffer.Bytes;
            }
        }
    }

    // If we have not hit a threshold yet:
    if (firstKeptElement < kDecoderRemoveThreshold) {
        return;
    }

    const unsigned firstKeptSubwindow  = firstKeptElement / kSubwindowSize;
    const unsigned removedElementCount = firstKeptSubwindow * kSubwindowSize;
    SIAMESE_DEBUG_ASSERT(firstKeptSubwindow >= 1);
    SIAMESE_DEBUG_ASSERT(Subwindows.GetSize() > firstKeptSubwindow);
    SIAMESE_DEBUG_ASSERT(removedElementCount % kColumnLaneCount == 0);
    SIAMESE_DEBUG_ASSERT(removedElementCount <= NextExpectedElement);

    Logger.Info("********* Removing up to ", removedElementCount);

    // If there is a sum we should be maintaining:
    if (seenSum)
    {
        unsigned sumElementStart = ColumnToElement(targetSumStartColumn);

        // If the sum start point is changing:
        if (SumColumnStart != targetSumStartColumn ||
            SumColumnCount > targetSumColumnCount)
        {
            // If the new sum start point is already clipped:
            if (InvalidElement(sumElementStart))
            {
                SIAMESE_DEBUG_BREAK(); // Should never happen
                Logger.Error("RemoveElements failed: Removal point sum start is clipped! " \
                    "targetSumStartColumn=", targetSumStartColumn, ", ColumnStart=", ColumnStart);
                EmergencyDisabled = true;
                return;
            }

            ResetSums(sumElementStart);

            SumColumnStart = targetSumStartColumn;
            SumColumnCount = targetSumColumnCount;
        }
        else
        {
            // Determine sum start element
            if (InvalidElement(sumElementStart)) {
                sumElementStart = 0;
            }

            // Get ready to accumulate new sums
            if (!StartSums(sumElementStart, initialRecoveryBytes))
            {
                Logger.Error("RemoveElements.StartSums failed. targetSumStartColumn=",
                    targetSumStartColumn, ", sumElementStart=", sumElementStart,
                    ", initialRecoveryBytes=", initialRecoveryBytes);
                SIAMESE_DEBUG_BREAK();
                EmergencyDisabled = true;
                return;
            }
        }

        // Roll up all the sums past the point of removal
        for (unsigned laneIndex = 0; laneIndex < kColumnLaneCount; ++laneIndex)
        {
            for (unsigned sumIndex = 0; sumIndex < kColumnSumCount; ++sumIndex)
            {
                // Roll up this lane, sum
                GetSum(laneIndex, sumIndex, removedElementCount);

                // If the start element is getting clipped:
                if (Lanes[laneIndex].Sums[sumIndex].ElementStart >= removedElementCount) {
                    Lanes[laneIndex].Sums[sumIndex].ElementStart -= removedElementCount;
                }
                else {
                    Lanes[laneIndex].Sums[sumIndex].ElementStart = laneIndex;
                }

                SIAMESE_DEBUG_ASSERT(Lanes[laneIndex].Sums[sumIndex].ElementEnd >= removedElementCount);
                Lanes[laneIndex].Sums[sumIndex].ElementEnd -= removedElementCount;
            }
        }
    }
    else
    {
        /*
            Recovery list only contains Cauchy rows:

            Note that if we see Cauchy rows again it means the encoder reset its sums small enough to
            start sending those again, so we can use that as an indicator to reset ours also.

            Reset the sum column count to zero, which will prevent us from rolling up any running sums.
            Furthermore when a sum is encountered it will be reset during recovery.
        */
        SumColumnCount = 0;
    }

    // Reset windows before putting them on the back
    for (unsigned i = 0; i < firstKeptSubwindow; ++i) {
        Subwindows.GetRef(i)->Reset();
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
        firstKeptSubwindow * sizeof(DecoderSubwindow*)
    );

    const unsigned subwindowsShifted = Subwindows.GetSize() - firstKeptSubwindow;

    // Shift subwindows to front that are being kept
    memmove(
        Subwindows.GetPtr(0),
        Subwindows.GetPtr(firstKeptSubwindow),
        subwindowsShifted * sizeof(DecoderSubwindow*)
    );

    // Removed subwindows are moved to the end (unordered) for later reuse
    memcpy(
        Subwindows.GetPtr(subwindowsShifted),
        SubwindowsShift.GetPtr(0),
        firstKeptSubwindow * sizeof(DecoderSubwindow*)
    );

    // Update the count of elements in the window
    SIAMESE_DEBUG_ASSERT(Count >= removedElementCount);
    Count -= removedElementCount;

    // Roll up the ColumnStart member
    ColumnStart = ElementToColumn(removedElementCount);
    SIAMESE_DEBUG_ASSERT(ColumnStart == Subwindows.GetRef(0)->Originals[0].Column || Subwindows.GetRef(0)->Originals[0].Buffer.Bytes == 0);

    // Roll up the FirstUnremovedElement member
    SIAMESE_DEBUG_ASSERT(NextExpectedElement >= removedElementCount);
    NextExpectedElement -= removedElementCount;

    // Decrement element counters
    RecoveryPackets->DecrementElementCounters(removedElementCount);
    CheckedRegion->DecrementElementCounters(removedElementCount);
    RecoveryMatrix->DecrementElementCounters(removedElementCount);
}


//------------------------------------------------------------------------------
// RecoveryMatrixState

void RecoveryMatrixState::Reset()
{
    Columns.Clear();
    Rows.Clear();
    Pivots.Clear();

    Matrix.Clear();

    PreviousNextCheckStart = 0;
    GEResumePivot = 0;
}

void RecoveryMatrixState::DecrementElementCounters(const unsigned elementCount)
{
    if (PreviousNextCheckStart > elementCount)
        PreviousNextCheckStart -= elementCount;
    else
        PreviousNextCheckStart = 0;
}

void RecoveryMatrixState::PopulateColumns(const unsigned oldColumns, const unsigned newColumns)
{
    if (oldColumns >= newColumns)
        return;

    Columns.SetSize_Copy(newColumns);

    // Resume adding from the last stop point
    unsigned elementStart     = PreviousNextCheckStart;
    PreviousNextCheckStart    = CheckedRegion->NextCheckStart;
    const unsigned elementEnd = CheckedRegion->NextCheckStart;
    if (elementStart < CheckedRegion->ElementStart)
        elementStart = CheckedRegion->ElementStart;

    // The column count increased which means we should have some columns to check
    SIAMESE_DEBUG_ASSERT(elementStart < elementEnd);

    // Check the current subwindow for next lost packet:
    const unsigned subwindowEnd = (elementEnd + kSubwindowSize - 1) / kSubwindowSize;
    unsigned subwindowIndex     = elementStart / kSubwindowSize;

    unsigned bitIndex = elementStart % kSubwindowSize;
    unsigned column   = oldColumns;

    while (subwindowIndex < subwindowEnd)
    {
        SIAMESE_DEBUG_ASSERT(subwindowIndex < Window->Subwindows.GetSize());

        DecoderSubwindow* subwindowPtr = Window->Subwindows.GetRef(subwindowIndex);

        // If there may be any lost packets in this subwindow:
        if (subwindowPtr->GotCount < kSubwindowSize)
        {
            do
            {
                // Seek next clear bit
                bitIndex = subwindowPtr->Got.FindFirstClear(bitIndex);

                // If there were none, skip this subwindow
                if (bitIndex >= kSubwindowSize)
                    break;

                // Calculate element index and stop if we hit the end of the valid data
                const unsigned element = subwindowIndex * kSubwindowSize + bitIndex;
                SIAMESE_DEBUG_ASSERT(element < elementEnd);

                ColumnInfo* columnPtr = Columns.GetPtr(column);
                columnPtr->Column     = Window->ElementToColumn(element);
                columnPtr->Original   = &subwindowPtr->Originals[bitIndex];
                columnPtr->CX         = GetColumnValue(columnPtr->Column);

                // Point lost original packet to recovery matrix column
                SIAMESE_DEBUG_ASSERT(columnPtr->Original->Buffer.Bytes == 0);
                columnPtr->Original->Column = column;

                // If we just added the last column:
                if (++column >= newColumns)
                    return;

            } while (++bitIndex < kSubwindowSize);
        }

        // Reset bit index to the front of the next subwindow
        bitIndex = 0;

        // Check next subwindow
        ++subwindowIndex;
    }

    SIAMESE_DEBUG_BREAK(); // Should never get here
    Window->EmergencyDisabled = true;
}

void RecoveryMatrixState::PopulateRows(const unsigned oldRows, const unsigned newRows)
{
    if (oldRows >= newRows)
        return;

    Rows.SetSize_Copy(newRows);

    RecoveryPacket* recovery;
    if (oldRows > 0)
        recovery = Rows.GetRef(oldRows - 1).Recovery->Next;
    else
        recovery = CheckedRegion->FirstRecovery;
    SIAMESE_DEBUG_ASSERT(recovery);

    for (unsigned rowIndex = oldRows; rowIndex < newRows; ++rowIndex, recovery = recovery->Next)
    {
        RowInfo* rowPtr = Rows.GetPtr(rowIndex);
        rowPtr->Recovery          = recovery;
        rowPtr->UsedForSolution   = false;
        rowPtr->MatrixColumnCount = recovery->LostCount;

        Logger.Info("*** Recovery packet: start=", recovery->Metadata.ColumnStart,  " Sum_Count=",  recovery->Metadata.SumCount,  " LDPC_Count=",  recovery->Metadata.LDPCCount);
    }
}

bool RecoveryMatrixState::GenerateMatrix()
{
    const unsigned columns = CheckedRegion->LostCount;
    const unsigned rows    = CheckedRegion->RecoveryCount;
    SIAMESE_DEBUG_ASSERT(rows >= columns);

    unsigned oldRows    = (unsigned)Rows.GetSize();
    unsigned oldColumns = (unsigned)Columns.GetSize();

    // If we missed a reset somewhere:
    if (rows < oldRows || columns < oldColumns)
    {
        SIAMESE_DEBUG_BREAK(); // Should never happen
        Reset();
        oldRows    = 0;
        oldColumns = 0;
    }

    bool matrixAllocated = false;
    if (oldRows == 0)
        matrixAllocated = Matrix.Initialize(TheAllocator, rows, columns);
    else
        matrixAllocated = Matrix.Resize(TheAllocator, rows, columns);
    if (!matrixAllocated)
    {
        Reset();
        return false;
    }

    PopulateColumns(oldColumns, columns);
    PopulateRows(oldRows, rows);

    const unsigned stride = Matrix.AllocatedColumns;
    uint8_t* rowData      = Matrix.Data;
    unsigned startRow     = 0;

    // If we need to fill to the right, start at the top
    if (columns <= oldColumns)
    {
        startRow = oldRows;
        rowData += startRow * stride;
    }

    std::ostringstream* pDebugMsg = nullptr;

    // For each row to fill:
    for (unsigned i = startRow; i < rows; ++i, rowData += stride)
    {
        RecoveryPacket* recovery = Rows.GetRef(i).Recovery;
        const RecoveryMetadata metadata = recovery->Metadata;

#ifdef SIAMESE_ENABLE_CAUCHY
        // If this is a Cauchy or parity row:
        if (metadata.SumCount <= SIAMESE_CAUCHY_THRESHOLD)
        {
            const unsigned startMatrixColumn = (i < oldRows) ? oldColumns : 0;

            if (Logger.ShouldLog(logger::Level::Debug))
            {
                delete pDebugMsg;
                pDebugMsg = new std::ostringstream();
                *pDebugMsg << "Recovery row (" << (metadata.Row == 0 ? "Parity" : "Cauchy") << "): ";
            }

            // Fill columns from left for new rows:
            for (unsigned j = startMatrixColumn; j < columns; ++j)
            {
                const unsigned column  = Columns.GetRef(j).Column;
                const unsigned element = SubtractColumns(column, metadata.ColumnStart);

                // If we hit the end of the recovery packet data:
                if (element >= metadata.SumCount)
                {
                    for (; j < columns; ++j)
                        rowData[j] = 0;
                    break;
                }

                if (pDebugMsg)
                    *pDebugMsg << column << " ";

                const unsigned value = (metadata.Row == 0) ? 1 : CauchyElement(metadata.Row - 1, column % kCauchyMaxColumns);
                rowData[j] = (uint8_t)value;
            }

            if (pDebugMsg)
                Logger.Debug(pDebugMsg->str());

            continue;
        }
#endif // SIAMESE_ENABLE_CAUCHY

        // Calculate row multiplier RX
        const uint8_t RX = GetRowValue(metadata.Row);

        if (Logger.ShouldLog(logger::Level::Debug))
        {
            delete pDebugMsg;
            pDebugMsg = new std::ostringstream();
            *pDebugMsg << "Recovery row (Siamese): ";
        }

        const unsigned startMatrixColumn = (i < oldRows) ? oldColumns : 0;

        // Fill columns from left for new rows:
        for (unsigned j = startMatrixColumn; j < columns; ++j)
        {
            const unsigned column  = Columns.GetRef(j).Column;
            const unsigned element = SubtractColumns(column, metadata.ColumnStart);

            // If we hit the end of the recovery packet data:
            if (element >= metadata.SumCount)
            {
                for (; j < columns; ++j)
                    rowData[j] = 0;
                break;
            }

            if (pDebugMsg)
                *pDebugMsg << column << " ";

            // Generate opcode and parameters
            const uint8_t CX      = Columns.GetRef(j).CX;
            const uint8_t CX2     = gf256_sqr(CX);
            const unsigned lane   = column % kColumnLaneCount;
            const unsigned opcode = GetRowOpcode(lane, metadata.Row);

            unsigned value = 0;

            // Interpret opcode to calculate matrix row element j
            if (opcode & 1)
                value ^= 1;
            if (opcode & 2)
                value ^= CX;
            if (opcode & 4)
                value ^= CX2;
            if (opcode & 8)
                value ^= RX;
            if (opcode & 16)
                value ^= gf256_mul(CX, RX);
            if (opcode & 32)
                value ^= gf256_mul(CX2, RX);

            rowData[j] = (uint8_t)value;
        }

        if (pDebugMsg)
            Logger.Debug(pDebugMsg->str());

        PCGRandom prng;
        prng.Seed(metadata.Row, metadata.LDPCCount);

        const unsigned elementStart = recovery->ElementStart;
        const unsigned pairCount    = (metadata.LDPCCount + kPairAddRate - 1) / kPairAddRate;
        SIAMESE_DEBUG_ASSERT(metadata.SumCount >= metadata.LDPCCount);

        Logger.Trace("(Generate matrix) LDPC columns: ");

        for (unsigned k = 0; k < pairCount; ++k)
        {
            const unsigned element1   = elementStart + (prng.Next() % metadata.LDPCCount);
            OriginalPacket* original1 = Window->GetWindowElement(element1);
            if (original1->Buffer.Bytes <= 0)
            {
                // Note: packet->Column is set to the recovery matrix column for lost data in PopulateColumns()
                const unsigned matrixColumn = original1->Column;
                SIAMESE_DEBUG_ASSERT(matrixColumn < columns);
                if (matrixColumn >= startMatrixColumn)
                    rowData[matrixColumn] ^= 1;
            }

            const unsigned elementRX   = elementStart + (prng.Next() % metadata.LDPCCount);
            OriginalPacket* originalRX = Window->GetWindowElement(elementRX);
            if (originalRX->Buffer.Bytes <= 0)
            {
                const unsigned matrixColumn = originalRX->Column;
                SIAMESE_DEBUG_ASSERT(matrixColumn < columns);
                if (matrixColumn >= startMatrixColumn)
                    rowData[matrixColumn] ^= RX;
            }

            Logger.Trace(element1, " ", elementRX); // If we use this for testing may want to stringstream it
        } // for each bundle of random columns
    } // for each recovery row

    // Fill in revealed column pivots with their own value
    Pivots.SetSize_Copy(rows);
    for (unsigned i = oldRows; i < rows; ++i)
        Pivots.GetRef(i) = i;

    // If we have already performed some GE, then we need to eliminate new
    // row data and we need to carry on elimination for new columns
    if (GEResumePivot > 0)
    {
#ifdef SIAMESE_DEBUG
        // Check: Verify that newly exposed columns in old rows are zero
        for (unsigned i = 0; i < oldRows; ++i)
        {
            for (unsigned j = oldColumns; j < columns; ++j)
            {
                const uint8_t* ge_row = Matrix.Data + Matrix.AllocatedColumns * i;
                SIAMESE_DEBUG_ASSERT(ge_row[j] == 0);
            }
        }
#endif
        ResumeGE(oldRows, rows);
    }

#ifdef SIAMESE_DEBUG
    // Check: Verify zeros after matrix rows
    for (unsigned i = 0; i < rows; ++i)
    {
        unsigned j;
        for (j = columns; j > 0; --j)
        {
            uint8_t v = Matrix.Get(i, j - 1);
            if (v != 0)
                break;
        }
        const unsigned expectedLossCount = j;
        SIAMESE_DEBUG_ASSERT(Rows.GetRef(i).Recovery->LostCount >= expectedLossCount || GEResumePivot > 0);
        SIAMESE_DEBUG_ASSERT(Rows.GetRef(i).MatrixColumnCount >= expectedLossCount);
    }
#endif

    return true;
}

void RecoveryMatrixState::ResumeGE(const unsigned oldRows, const unsigned rows)
{
    // If we did not add any new rows:
    if (oldRows >= rows)
    {
        SIAMESE_DEBUG_ASSERT(oldRows == rows);
        return;
    }

    const unsigned stride = Matrix.AllocatedColumns;

    // For each pivot we have determined already:
    for (unsigned pivot_i = 0; pivot_i < GEResumePivot; ++pivot_i)
    {
        // Get the row for that pivot
        const unsigned matrixRowIndex_i = Pivots.GetRef(pivot_i);
        const uint8_t* ge_row = Matrix.Data + stride * matrixRowIndex_i;
        const uint8_t val_i   = ge_row[pivot_i];
        SIAMESE_DEBUG_ASSERT(val_i != 0);

        const unsigned pivotColumnCount = Rows.GetRef(matrixRowIndex_i).MatrixColumnCount;

        uint8_t* rem_row = Matrix.Data + stride * oldRows;

        // For each new row that was added:
        for (unsigned newRowIndex = oldRows; newRowIndex < rows; ++newRowIndex, rem_row += stride)
        {
            if (EliminateRow(ge_row, rem_row, pivot_i, pivotColumnCount, val_i))
            {
                // Grow the column count of this row if we just filled it in on the right
                if (Rows.GetRef(newRowIndex).MatrixColumnCount < pivotColumnCount)
                    Rows.GetRef(newRowIndex).MatrixColumnCount = pivotColumnCount;
            }
            SIAMESE_DEBUG_ASSERT(Pivots.GetRef(newRowIndex) == newRowIndex);
        }
    }
}

bool RecoveryMatrixState::GaussianElimination()
{
    // Attempt to solve as much of the matrix as possible without using a pivots array
    // since that requires extra memory operations.  Since the matrix will be dense we
    // have a good chance of going pretty far before we hit a zero

    if (GEResumePivot > 0)
        return PivotedGaussianElimination(GEResumePivot);

    const unsigned columns = Matrix.Columns;
    const unsigned stride  = Matrix.AllocatedColumns;
    const unsigned rows    = Matrix.Rows;
    uint8_t* ge_row        = Matrix.Data;

    for (unsigned pivot_i = 0; pivot_i < columns; ++pivot_i, ge_row += stride)
    {
        const uint8_t val_i = ge_row[pivot_i];
        if (val_i == 0)
            return PivotedGaussianElimination(pivot_i);

        RowInfo* rowPtr = Rows.GetPtr(pivot_i);
        rowPtr->UsedForSolution = true;
        const unsigned pivotColumnCount = rowPtr->MatrixColumnCount;

        uint8_t* rem_row = ge_row;

        // For each remaining row:
        for (unsigned pivot_j = pivot_i + 1; pivot_j < rows; ++pivot_j)
        {
            rem_row += stride;
            if (EliminateRow(ge_row, rem_row, pivot_i, pivotColumnCount, val_i))
            {
#ifdef SIAMESE_DECODER_TRACK_ZERO_COLUMNS
                // Grow the column count of this row if we just filled it in on the right
                if (Rows[pivot_j].MatrixColumnCount < pivotColumnCount)
                    Rows[pivot_j].MatrixColumnCount = pivotColumnCount;
#endif
            }
        }
    }

    return true;
}

bool RecoveryMatrixState::PivotedGaussianElimination(unsigned pivot_i)
{
    const unsigned columns = Matrix.Columns;
    const unsigned stride  = Matrix.AllocatedColumns;
    const unsigned rows    = Matrix.Rows;

    // Resume from next row down...
    // Note: This is designed to be called by the non-pivoted version
    unsigned pivot_j = pivot_i + 1;
    goto UsePivoting;

    // For each pivot to determine:
    for (; pivot_i < columns; ++pivot_i)
    {
        pivot_j = pivot_i;
UsePivoting:
        for (; pivot_j < rows; ++pivot_j)
        {
            const unsigned matrixRowIndex_j = Pivots.GetRef(pivot_j);
            const uint8_t* ge_row = Matrix.Data + stride * matrixRowIndex_j;
            const uint8_t val_i = ge_row[pivot_i];
            if (val_i == 0)
                continue;

            // Swap out the pivot index for this one
            if (pivot_i != pivot_j)
            {
                const unsigned temp = Pivots.GetRef(pivot_i);
                Pivots.GetRef(pivot_i) = Pivots.GetRef(pivot_j);
                Pivots.GetRef(pivot_j) = temp;
            }

            RowInfo* rowPtr = Rows.GetPtr(matrixRowIndex_j);
            rowPtr->UsedForSolution = true;
            const unsigned pivotColumnCount = rowPtr->MatrixColumnCount;

            // Skip eliminating extra rows in the case that we just solved the matrix
            if (pivot_i >= columns - 1)
                return true;

            // For each remaining row:
            for (unsigned pivot_k = pivot_i + 1; pivot_k < rows; ++pivot_k)
            {
                const unsigned matrixRowIndex_k = Pivots.GetRef(pivot_k);
                uint8_t* rem_row = Matrix.Data + stride * matrixRowIndex_k;
                if (EliminateRow(ge_row, rem_row, pivot_i, pivotColumnCount, val_i))
                {
                    // Grow the column count of this row if we just filled it in on the right
                    if (Rows.GetRef(matrixRowIndex_k).MatrixColumnCount < pivotColumnCount)
                        Rows.GetRef(matrixRowIndex_k).MatrixColumnCount = pivotColumnCount;
                }
            }

            goto NextPivot;
        }

        // Remember where we failed last time
        GEResumePivot = pivot_i;

        return false;
NextPivot:;
    }

    return true;
}


//------------------------------------------------------------------------------
// CheckedRegionState

void CheckedRegionState::Reset()
{
    ElementStart   = 0;
    NextCheckStart = 0;
    FirstRecovery  = nullptr;
    LastRecovery   = nullptr;
    RecoveryCount  = 0;
    LostCount      = 0;
    SolveFailed    = false;

    RecoveryMatrix->Reset();
}

void CheckedRegionState::DecrementElementCounters(const unsigned elementCount)
{
    if (ElementStart < elementCount || NextCheckStart < elementCount)
    {
        Logger.Warning("Just clipped the checked region state -- reset");
        Reset();
        return;
    }

    ElementStart -= elementCount;
    NextCheckStart -= elementCount;
}


//------------------------------------------------------------------------------
// RecoveryPacketList

void RecoveryPacketList::Insert(RecoveryPacket* recovery, bool outOfOrder)
{
    RecoveryPacket* prev = Tail;
    RecoveryPacket* next = nullptr;

    const unsigned recoveryStart = recovery->Metadata.ColumnStart;
    const unsigned recoveryEnd   = recovery->ElementEnd;

    // Search for insertion point:
    for (; prev; next = prev, prev = prev->Prev)
    {
        const unsigned prevStart = prev->Metadata.ColumnStart;
        const unsigned prevEnd   = prev->ElementEnd;

        /*
            This insertion order guarantees that the left and right side of
            the recovery input ranges are monotonically increasing as in:

                recovery 0: 012345
                recovery 1:   23456 <- Cauchy row
                recovery 2: 01234567
                recovery 3:     45678
                recovery 4:     456789
        */
        if (recoveryEnd >= prevEnd)
        {
            if (recoveryEnd > prevEnd) {
                break;
            }
            if (IsColumnDeltaNegative(SubtractColumns(recoveryStart, prevStart))) {
                break;
            }
        }
    }

    // Insert into linked list
    recovery->Next = next;
    recovery->Prev = prev;
    if (prev) {
        prev->Next = recovery;
    }
    else {
        Head = recovery;
    }
    if (next) {
        next->Prev = recovery;
    }
    else {
        Tail = recovery;
    }

    // If inserting at head or somewhere in the middle:
    // Invalidate the checked region because a smaller solution may be available
    if (!prev || next) {
        CheckedRegion->Reset();
    }
    // Note that for the case where we insert at the end of a non-empty list we do
    // not reset the checked region.  This is the common case where recovery data is
    // received in order.

    ++RecoveryPacketCount;

    if (!outOfOrder)
    {
        // Update the last received recovery metadata
        LastRecoveryMetadata = recovery->Metadata;
        LastRecoveryBytes = recovery->Buffer.Bytes;
    }
}

void RecoveryPacketList::DeletePacketsBefore(const unsigned element)
{
    RecoveryPacket* recovery = Head;
    unsigned deleteCount     = 0;

    // Examine recovery packets starting with the oldest
    for (RecoveryPacket* next; recovery; recovery = next)
    {
        // Stop once we eclipse the element
        if (recovery->ElementEnd > element)
            break;

        next = recovery->Next;
        recovery->Buffer.Free(TheAllocator);
        TheAllocator->Destruct(recovery);
        ++deleteCount;
    }

    Head = recovery;
    if (recovery)
    {
        recovery->Prev = nullptr;
        RecoveryPacketCount -= deleteCount;
    }
    else
    {
        Tail = nullptr;
        RecoveryPacketCount = 0;
    }
}

void RecoveryPacketList::DecrementElementCounters(const unsigned elementCount)
{
    for (RecoveryPacket* recovery = Head; recovery; recovery = recovery->Next)
    {
        SIAMESE_DEBUG_ASSERT(recovery->ElementEnd >= elementCount);
        recovery->ElementEnd -= elementCount;

        SIAMESE_DEBUG_ASSERT(recovery->ElementStart >= elementCount);
        recovery->ElementStart -= elementCount;
    }
}

void RecoveryPacketList::Delete(RecoveryPacket* recovery)
{
    SIAMESE_DEBUG_ASSERT(recovery);
    RecoveryPacket* prev = recovery->Prev;
    RecoveryPacket* next = recovery->Next;

    if (prev)
        prev->Next = next;
    else
        Head = next;

    if (next)
        next->Prev = prev;
    else
        Tail = prev;

    --RecoveryPacketCount;

    recovery->Buffer.Free(TheAllocator);
    TheAllocator->Destruct(recovery);
}


} // namespace siamese
