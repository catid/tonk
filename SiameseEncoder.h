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

#pragma once

/**
    Encoder

    The encoder keeps track of packets that have not yet been acknowledged by
    the decoder, and when asked to encode it will select between a Cauchy matrix
    or a more complicated Siamese matrix row.
*/

#include "SiameseCommon.h"

namespace siamese {


/**
    Terminology:

    + Packet Number = Number assigned to each original packet that is unique
      for a window of data being processed by the codec.  It wraps around to 0.

    + Column Number = Same as Packet Number.

    + Window Element = A packet in the Subwindows array.  0 is the first
      array position in the first subwindow, corresponding to ColumnStart.
*/


//------------------------------------------------------------------------------
// EncoderStats

struct EncoderStats
{
    /// SiameseEncoderStats
    uint64_t Counts[SiameseEncoderStats_Count];

    EncoderStats();
};


//------------------------------------------------------------------------------
// EncoderColumnLane

struct EncoderColumnLane
{
    /// Next element to accumulate, once we get it from the application
    unsigned NextElement[kColumnSumCount];

    /// Running sums.  See kColumnSumCount definition
    GrowingAlignedDataBuffer Sum[kColumnSumCount];

    /// Longest packet in this lane
    /// Note: I think it's a win to keep this per-lane because if the
    /// data size is highly variable we may reduce memory accesses
    unsigned LongestPacket = 0;
};


//------------------------------------------------------------------------------
// EncoderSubwindow

struct EncoderSubwindow
{
    /// Original packets in this subwindow indexed by packet number
    std::array<OriginalPacket, kSubwindowSize> Originals;

    /// Timestamp at which we last sent the packet
    std::array<uint32_t, kSubwindowSize> LastSendMsec;
};


//------------------------------------------------------------------------------
// EncoderPacketWindow

struct EncoderPacketWindow
{
    pktalloc::Allocator* TheAllocator = nullptr;
    EncoderStats* Stats = nullptr;

    /// Next column number to assign to a packet
    unsigned NextColumn = 0;

    /// Count of packets so far
    unsigned Count = 0;

    /// Start column of set
    /// Note: When Count == 0, this is undefined
    unsigned ColumnStart = 0;

    /// Longest packet
    /// Note: Undefined if count == 0
    unsigned LongestPacket = 0;

    /// Note: This is updated by RemoveUpTo()
    unsigned FirstUnremovedElement = 0;

    /// Sum element range [start...end)
    /// Note: End is the first element outside of the range
    unsigned SumStartElement = 0;
    unsigned SumEndElement = 0;
    unsigned SumColumnStart = 0;
    unsigned SumErasedCount = 0;

    /// Allocated Subwindows
    pktalloc::LightVector<EncoderSubwindow*> Subwindows;

    /// Running summations for each lane
    EncoderColumnLane Lanes[kColumnLaneCount];

    /// Temporary workspace reused each time subwindows must be shifted
    pktalloc::LightVector<EncoderSubwindow*> SubwindowsShift;

    /// If input is invalid or we run out of memory, the encoder is disabled
    /// to prevent it from allowing exploits to run or cause crashes
    bool EmergencyDisabled = false;


    /// Ctor initializes elements to default values
    EncoderPacketWindow();

    /// Convert a column to a window element
    SIAMESE_FORCE_INLINE unsigned ColumnToElement(unsigned column) const
    {
        return SubtractColumns(column, ColumnStart);
    }

    /// Validate that an element is within the window
    SIAMESE_FORCE_INLINE bool InvalidElement(unsigned element) const
    {
        return (element >= Count);
    }

    /// Convert a window element to a column
    SIAMESE_FORCE_INLINE unsigned ElementToColumn(unsigned element) const
    {
        return AddColumns(element, ColumnStart);
    }

    /// Get element from the window, indexed by window offset not column number
    /// Precondition: 0 <= element < Count
    SIAMESE_FORCE_INLINE OriginalPacket* GetWindowElement(unsigned windowElement)
    {
        SIAMESE_DEBUG_ASSERT(windowElement < Count);
        return &(Subwindows.GetRef(windowElement / kSubwindowSize)->Originals[windowElement % kSubwindowSize]);
    }

    /// Get element send timestamp from the window, indexed by window offset not column number
    /// Precondition: 0 <= element < Count
    SIAMESE_FORCE_INLINE uint32_t* GetWindowElementTimestampPtr(unsigned windowElement)
    {
        SIAMESE_DEBUG_ASSERT(windowElement < Count);
        return &(Subwindows.GetRef(windowElement / kSubwindowSize)->LastSendMsec[windowElement % kSubwindowSize]);
    }

    /// How many slots remain in the window?
    SIAMESE_FORCE_INLINE unsigned GetRemainingSlots() const
    {
        SIAMESE_DEBUG_ASSERT(SIAMESE_MAX_PACKETS >= Count);
        return SIAMESE_MAX_PACKETS - Count;
    }

    /// Append a packet to the end of the set
    SiameseResult Add(SiameseOriginalPacket& packet);

    /// Removes elements up to the given column
    void RemoveBefore(unsigned firstKeptColumn);

    /// Get next element at or after the given element that is in the given lane
    unsigned GetNextLaneElement(unsigned element, unsigned laneIndex)
    {
        SIAMESE_DEBUG_ASSERT(element < Count);
        SIAMESE_DEBUG_ASSERT(laneIndex < kColumnLaneCount);
        unsigned nextElement = element - (element % kColumnLaneCount) + laneIndex;
        if (nextElement < element)
            nextElement += kColumnLaneCount;
        SIAMESE_DEBUG_ASSERT(nextElement >= element);
        SIAMESE_DEBUG_ASSERT(nextElement % kColumnLaneCount == laneIndex);
        SIAMESE_DEBUG_ASSERT(nextElement < Count + kColumnLaneCount);
        return nextElement;
    }

    /// Reset lane sums from the given start element
    void ResetSums(unsigned elementStart);

    /// Get running sums for a lane
    const GrowingAlignedDataBuffer* GetSum(unsigned laneIndex, unsigned sumIndex, unsigned elementEnd);

    /// Returns the number of elements that have not been acknowledged yet
    unsigned GetUnacknowledgedCount()
    {
        SIAMESE_DEBUG_ASSERT(FirstUnremovedElement < Count || Count == 0);
        return Count - FirstUnremovedElement;
    }

    /// Start a new window from the given column
    void StartNewWindow(unsigned column);

    /// Clear the window
    void ClearWindow();

    /// Precondition: FirstUsedElement >= kSubwindowSize
    void RemoveElements();
};


//------------------------------------------------------------------------------
// EncoderAcknowledgementState

/// State related to the last received acknowledgement
struct EncoderAcknowledgementState
{
    pktalloc::Allocator* TheAllocator = nullptr;
    EncoderPacketWindow* TheWindow = nullptr;

    /// Loss range list raw data, copied from the acknowledgement
    uint8_t* Data = nullptr;

    /// Number of bytes used by the loss range data
    unsigned DataBytes = 0;

    /// Padding on the loss range data for speeding up decoding
    static const unsigned kPaddingBytes = 8;

    /// Next byte to process
    unsigned Offset = 0;

    /// Next column lost
    unsigned LossColumn = 0;

    /// Number of losses left in the current range
    unsigned LossCount = 0;

    /// Next column expected by receiver
    unsigned NextColumnExpected = 0;

    /// Next column to be used for RTO calculations
    unsigned NextRTOColumn = 0;

    /// If the oldest column is already found, then keep trying to send it
    bool FoundOldest = false;

    /// This is the oldest column to resend
    unsigned OldestColumn = 0;

    /// Initial Retransmit Timeout (RTO) in milliseconds
    static const unsigned kInitialRetransmitTimeoutMsec = 500; ///< milliseconds

    /// Retransmit Timeout (RTO) in milliseconds
    unsigned RetransmitTimeoutMsec = kInitialRetransmitTimeoutMsec;

    /// Maximum RTT value within a window
    WindowedMinMax< unsigned, WindowedMaxCompare<unsigned> > MaxWindowedRTT;


    /**
        OnAcknowledgementData()

        Handle acknowledgement data from decoder.

        This resets the NACK list iterator to the top, sets NextColumnExpected
        and moves NextQuickNACKColumn forward if needed.
        The FoundOldest state will be reset, forcing a re-scan of the NACK list.

        Returns true if the data was valid and state was updated.
        Return false if the data was invalid, leaving state unchanged.
    */
    bool OnAcknowledgementData(const uint8_t* data, unsigned bytes);

    /// Returns true if the column iterator is pointing at the first column
    SIAMESE_FORCE_INLINE bool IsIteratorAtFront() const
    {
        return LossColumn == NextColumnExpected;
    }

    /// Returns true if there are any negative acknowledgements
    SIAMESE_FORCE_INLINE bool HasNegativeAcknowledgements() const
    {
        return DataBytes > 0;
    }

    /// Get next loss column.
    /// Returns false if no more columns to read.  Call RestartLossIterator()
    /// to restart the iteration when it returns false.
    bool GetNextLossColumn(unsigned& columnOut);

    /// Reset the loss iterator to the start so we read through them all again
    void RestartLossIterator();

    /// Clear the ack data
    void Clear();

protected:
    /// Decode the next NACK range
    bool DecodeNextRange();

    /// Update RTO based on latest NACK ranges
    void UpdateRTO();
};


//------------------------------------------------------------------------------
// Encoder

/// Threshold number of elements before removing data
static const unsigned kEncoderRemoveThreshold = 2 * kSubwindowSize;
static_assert(kEncoderRemoveThreshold % kSubwindowSize == 0, "It removes on window boundaries");

class Encoder
{
public:
    Encoder();

    SIAMESE_FORCE_INLINE unsigned GetRemainingSlots() const
    {
        return Window.GetRemainingSlots();
    }

    /// Add an original data packet to the encoder
    SIAMESE_FORCE_INLINE SiameseResult Add(SiameseOriginalPacket& packet)
    {
        return Window.Add(packet);
    }

    /// Remove original data packet up to the given column
    SIAMESE_FORCE_INLINE void RemoveBefore(unsigned firstKeptColumn)
    {
        Window.RemoveBefore(firstKeptColumn);
    }

    /// Process an acknowledgement from the decoder
    SiameseResult Acknowledge(
        const uint8_t* data,
        unsigned bytes,
        unsigned& nextExpectedPacketNumOut);

    /// Retransmit an original packet in response to a NACK
    SiameseResult Retransmit(SiameseOriginalPacket& originalOut);

    /// Generate the next recovery packet for the data
    SiameseResult Encode(SiameseRecoveryPacket& recoveryOut);

    /// Get a packet in the set
    SiameseResult Get(SiameseOriginalPacket& packet);

    /// Get statistics
    SiameseResult GetStatistics(uint64_t* statsOut, unsigned statsCount);

protected:
    /// When the allocator goes out of scope all our buffer allocations are freed
    pktalloc::Allocator TheAllocator;

    /// Collected statistics
    EncoderStats Stats;

    /// Set of encoded packets in the sliding window
    EncoderPacketWindow Window;

    /// Acknowledgement state
    EncoderAcknowledgementState Ack;

    /// Keeps a copy of the last recovery packet to speed up generating the next one
    GrowingAlignedDataBuffer RecoveryPacket;

    /// Next row to generate for Siamese rows
    unsigned NextRow = 0;

    /// Next start column that can be all ones
    unsigned NextParityColumn = 0;

#ifdef SIAMESE_ENABLE_CAUCHY
    /// Next row to generate for Cauchy rows
    unsigned NextCauchyRow = 0;
#endif // SIAMESE_ENABLE_CAUCHY


    /// Normal case of generating recovery packet
    void AddDenseColumns(unsigned row, uint8_t* productWorkspace);
    void AddLightColumns(unsigned row, uint8_t* productWorkspace);

    /// Generate output for the case of a single input packet
    SiameseResult GenerateSinglePacket(SiameseRecoveryPacket& packet);

#ifdef SIAMESE_ENABLE_CAUCHY
    /// Generate output for the case of a small number of input packets
    SiameseResult GenerateCauchyPacket(SiameseRecoveryPacket& packet);
#endif // SIAMESE_ENABLE_CAUCHY

    /// Attempt to retransmit the given original data
    SiameseResult AttemptRetransmit(
        OriginalPacket* original,
        SiameseOriginalPacket& originalOut);
};


} // namespace siamese
