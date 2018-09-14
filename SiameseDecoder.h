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

#pragma once

/**
    Siamese Decoder Data Recovery Process

    (1) Collect data:

    This collects original data packets and recovery packets, until a solution
    may be possible (recovery is possible about 99.9% of the time).

    Recovery data is stored in a matrix with this structure:

                    original data:
        recovery 0: 012345
        recovery 1: 01234567
        recovery 2:     45678
        recovery 3:     456789

    such that the left and right sides of the non-zero matrix elements described
    by the recovery packets are monotonically increasing, which enables several
    algorithms to work quickly on the input data.

    (2) Generate recovery matrix:

    The recovery matrix is a square GF(2^^8) where the width of the matrix is
    the number of losses we are trying to recover.  The recovery matrix elements
    are sampled from a larger matrix that is implicit (not actually constructed),
    where the columns correspond to original data and the rows correspond to
    recovery packets.

    (3) Solve recovery matrix:

    We experimentally perform Gaussian elimination on the matrix to put it in
    upper triangular form.  If this is successful, then recovery can proceed.
    Note that we have done no operations on the original data yet, so this step
    is fairly inexpensive.

    To speed up this step with the density of the matrix in mind, we attempt
    GE without pivoting first and then switch to a pivoting algorithm as zeros
    are encountered.

    If this fails we attempt to build a larger recovery matrix involving more
    received recovery packets, which may also involve more lost original data.
    If recovery is not possible with the received data, then we wait for more.

    (4) Eliminate received data:

    This step involves most of the received data and takes the most time.
    Its complexity is slightly less than that of the encoder.  As a result,
    and improvement in encoder performance will translate to a faster decoder.

    For each recovery packet involved in solution we need to eliminate original
    data that is outside of the recovery matrix, so that the recovery matrix can
    be applied to recover the lost data.

    We construct the sums of received original data for each row as in the encoder,
    and roll the sums up as the left side is eliminated from later recovery packets.
    The sums are reused on multiple rows to eliminate data faster.

    (5) Recover original data:

    The same operations performed to arrive at the GE solution earlier are now
    performed on the recovery data packets.  We then multiply by the upper
    triangle of the recovery matrix in back substitution order.  Finally the
    diagonal is eliminated by dividing each recovery packet by the diagonal.
    The recovery packets now contain original data.

    The original data are prefixed by a length field so that the original data
    length can be recovered, since we support variable length input data.
*/

#include "SiameseCommon.h"

namespace siamese {


//------------------------------------------------------------------------------
// DecoderStats

struct DecoderStats
{
    /// SiameseDecoderStats
    uint64_t Counts[SiameseDecoderStats_Count];

    DecoderStats();
};


//------------------------------------------------------------------------------
// DecoderColumnLane

struct DecoderSum
{
    /// First element accumulated
    unsigned ElementStart = 0;

    /// Next element to accumulate
    unsigned ElementEnd = 0;

    /// Running sum
    GrowingAlignedDataBuffer Buffer;
};

struct DecoderColumnLane
{
    /// Running sums.  See kColumnSumCount definition
    DecoderSum Sums[kColumnSumCount];
};


//------------------------------------------------------------------------------
// RecoveryPacket

struct RecoveryPacket
{
    /// Linked list of recovery packets
    RecoveryPacket* Next = nullptr;
    RecoveryPacket* Prev = nullptr;

    /// Metadata attached to packet
    RecoveryMetadata Metadata;

    /// Element in the window where this packet starts LDPC/Cauchy protection.
    /// To be clear for Siamese rows this does not reflect the sum start.
    /// Note: The metadata ColumnStart can be before this element for rows that
    /// have long running sums, where the old data has been accumulated into sums
    /// and erased from memory.
    unsigned ElementStart = 0;

    /// Element in the window immediately after summing+LDPC ends
    unsigned ElementEnd = 0;

    /// Number of lost packets leading up to the end of this recovery packet range
    unsigned LostCount = 0;

    /// Packet data
    GrowingAlignedDataBuffer Buffer;
};


//------------------------------------------------------------------------------
// CheckedRegionState

/**
    The recovery region consists of the area covered by recovery packets.
    We iteratively check for solutions with a minimal number of recovery packets
    starting with the next expected lost original data packet if possible.
    We attempt to use as few recovery packets as possible in each solution since
    the solver can run faster in this case.

    The checked region consists of the recovery packets we have added to the
    recovery region so far, noting that there may be some recovery packets we
    have not added to the checked region yet.

    The checked region is helpful since recovery can fail or we may not have
    received enough recovery packets yet - It allows us to iteratively check
    if a solution is possible rather than starting over from scratch given
    that most data arrives in order.
*/
struct CheckedRegionState
{
    struct RecoveryMatrixState* RecoveryMatrix = nullptr;

    /// Checked recovery region start window element
    unsigned ElementStart = 0;

    /// First and last recovery packets included in the check
    RecoveryPacket* FirstRecovery = nullptr;
    RecoveryPacket* LastRecovery = nullptr;

    /// One element after the recovery region we have tested
    unsigned NextCheckStart = 0;

    /// Count of recovery packets and lost packets so far in the recovery region
    unsigned RecoveryCount = 0, LostCount = 0;

    /// The current checked region failed solution last time
    bool SolveFailed = false;


    /// Clear the checked region forcing us to reconsider recovery data
    void Reset();

    /// Decrement all the element counters by a given amount
    void DecrementElementCounters(const unsigned elementCount);
};


//------------------------------------------------------------------------------
// RecoveryPacketList

struct RecoveryPacketList
{
    pktalloc::Allocator* TheAllocator = nullptr;
    CheckedRegionState* CheckedRegion = nullptr;

    /// Sorted list of recovery packets, ordered from oldest to newest
    RecoveryPacket* Head = nullptr; ///< Oldest recovery packet
    RecoveryPacket* Tail = nullptr; ///< Newest recovery packet

    /// Number of recovery packets in the list
    unsigned RecoveryPacketCount = 0;

    /// Last recovery packet metadata that was received in order
    RecoveryMetadata LastRecoveryMetadata;
    unsigned LastRecoveryBytes = 0;

    SIAMESE_FORCE_INLINE bool IsEmpty() const
    {
        SIAMESE_DEBUG_ASSERT((RecoveryPacketCount != 0) == (Head != nullptr));
        return RecoveryPacketCount == 0;
    }

    /// Insert recovery packet into sorted list.
    /// Out of order RecoveryPackets will not update LastRecoveryMetadata
    void Insert(RecoveryPacket* packet, bool outOfOrder);

    /// Delete all packets before this element
    void DeletePacketsBefore(const unsigned element);

    /// Decrement all the element counters by a given amount
    void DecrementElementCounters(const unsigned elementCount);

    /// Delete the given recovery packet from the list - Used only by unit test
    void Delete(RecoveryPacket* recovery);
};


//------------------------------------------------------------------------------
// DecoderSubwindow

struct DecoderSubwindow
{
    /// Original packets in this subwindow indexed by packet number
    std::array<OriginalPacket, kSubwindowSize> Originals;

    /// Keeping track of which entries are filled in more efficiently
    pktalloc::CustomBitSet<kSubwindowSize> Got;
    unsigned GotCount = 0;


    void Reset()
    {
        Got.ClearAll();
        GotCount = 0;

        for (unsigned i = 0; i < kSubwindowSize; ++i)
        {
            Originals[i].Column = 0;
            Originals[i].Buffer.Bytes = 0;
        }
    }
};


//------------------------------------------------------------------------------
// DecoderPacketWindow

struct DecoderPacketWindow
{
    pktalloc::Allocator* TheAllocator = nullptr;
    DecoderStats* Stats = nullptr;
    CheckedRegionState* CheckedRegion = nullptr;
    RecoveryPacketList* RecoveryPackets = nullptr;
    RecoveryMatrixState* RecoveryMatrix = nullptr;

    /// Count of packets so far
    unsigned Count = 0;

    /// Start column of set
    /// Note: When Count == 0, this is undefined
    unsigned ColumnStart = 0;

    /// Next expected element
    unsigned NextExpectedElement = 0;

    /// Allocated Subwindows
    pktalloc::LightVector<DecoderSubwindow*> Subwindows;

    /// Set of lanes we're maintaining
    DecoderColumnLane Lanes[kColumnLaneCount];
    unsigned SumColumnStart = 0;
    unsigned SumColumnCount = 0;

    /// Packets returned by RecoverOriginalPackets() on success
    pktalloc::LightVector<SiameseOriginalPacket> RecoveredPackets;
    bool HasRecoveredPackets = false;

    /// List of columns that have been recovered
    pktalloc::LightVector<unsigned> RecoveredColumns;

    /// Temporary workspace reused each time subwindows must be shifted
    pktalloc::LightVector<DecoderSubwindow*> SubwindowsShift;

    /// If input is invalid or we run out of memory, the decoder is disabled
    /// to prevent it from allowing exploits to run or cause crashes
    bool EmergencyDisabled = false;


    /// Are we running any sums right now?
    SIAMESE_FORCE_INLINE bool IsRunningSums() const
    {
        return SumColumnCount > 0;
    }

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

    /// Get next element at or after the given element that is in the given lane
    SIAMESE_FORCE_INLINE unsigned GetNextLaneElement(unsigned element, unsigned laneIndex)
    {
        SIAMESE_DEBUG_ASSERT(element < Count && laneIndex < kColumnLaneCount);
        unsigned nextElement = element - (element % kColumnLaneCount) + laneIndex;
        if (nextElement < element) {
            nextElement += kColumnLaneCount;
        }
        SIAMESE_DEBUG_ASSERT(nextElement >= element);
        SIAMESE_DEBUG_ASSERT(nextElement % kColumnLaneCount == laneIndex);
        SIAMESE_DEBUG_ASSERT(nextElement < Count + kColumnLaneCount);
        return nextElement;
    }

    /// Get element from the window, indexed by window offset not column number
    /// Precondition: 0 <= element < Count
    SIAMESE_FORCE_INLINE OriginalPacket* GetWindowElement(unsigned windowElement)
    {
        SIAMESE_DEBUG_ASSERT(windowElement < Count);
        return &(Subwindows.GetRef(windowElement / kSubwindowSize)->Originals[windowElement % kSubwindowSize]);
    }

    /// Returns the number of lost packets in the given range (inclusive)
    /// windowElementStart < Count: First element to test
    /// windowElementStart <= Count: One element beyond the last one to test
    unsigned RangeLostPackets(unsigned windowElementStart, unsigned windowElementEnd);

    /// Returns Count if no more elements were lost
    /// Otherwise returns the next element that was lost at or after the given one
    unsigned FindNextLostElement(unsigned elementStart);

    /// Returns Count if no more elements were received
    /// Otherwise returns the next element that was received at or after the given one
    unsigned FindNextGotElement(unsigned elementStart);

    /// Find the next expected element
    void IterateNextExpectedElement(unsigned elementStart);

    /// Append a packet to the end of the set
    SiameseResult AddOriginal(const SiameseOriginalPacket& packet);

    /// Mark that we got a column
    /// Returns true if this was the next expected element
    bool MarkGotColumn(unsigned column);

    /// Make sure the window contains the given end element
    bool GrowWindow(unsigned windowElementEnd);

    /// Get running sums for a lane
    const GrowingAlignedDataBuffer* GetSum(unsigned laneIndex, unsigned sumIndex, unsigned elementEnd);

    /// Rebase running sums from the given element
    bool StartSums(unsigned elementStart, unsigned bufferBytes);

    /// Reset all sums to start from the given element
    void ResetSums(unsigned elementStart);

    /// Plug holes in the running sum from previous recovery action
    bool PlugSumHoles(unsigned elementStart);

    /// Removes elements from the front if they are no longer needed
    void RemoveElements();

    /// Returns the first window element that must be kept for recovery.
    /// This is used to determine how many window elements to remove
    unsigned GetFirstUsedWindowElement();
};


//------------------------------------------------------------------------------
// RecoveryMatrixState

/**
    We maintain a GF(2^^8) byte matrix that can grow a little in rows and
    columns to reattempt solving with a larger matrix that includes more
    lost columns and received recovery data, in the case that recovery fails.
    It is expected that recovery fails around 1% of the time.

    The matrix is also a bit oversized to allow us to prefetch the next row,
    and to align memory addresses with cache line boundaries for speed.
*/
struct RecoveryMatrixState
{
    pktalloc::Allocator* TheAllocator = nullptr;
    DecoderPacketWindow* Window = nullptr;
    CheckedRegionState* CheckedRegion = nullptr;

    struct RowInfo
    {
        RecoveryPacket* Recovery = nullptr;
        bool UsedForSolution = false;

        /// Number of non-zero matrix columns
        /// Note: This is useful because the matrix to solve often has zeros
        /// roughly above the diagonal.  As rows fill in during GE these counts
        /// must be updated to reflect their growth
        unsigned MatrixColumnCount = 0;
    };
    pktalloc::LightVector<RowInfo> Rows;

    struct ColumnInfo
    {
        /// Slot where the original packet data should end up
        OriginalPacket* Original = nullptr;

        /// Column number for the missing data
        unsigned Column = 0;

        /// Column multiplier
        uint8_t CX = 0;
    };
    pktalloc::LightVector<ColumnInfo> Columns;

    /// NextCheckStart value from the last time we populated columns
    unsigned PreviousNextCheckStart = 0;

    /// Recovery matrix
    GrowingAlignedByteMatrix Matrix;

    /// Array of pivots used for when rows need to be swapped
    /// This allows us to swap indices rather than swap whole rows to reduce memory accesses
    pktalloc::LightVector<unsigned> Pivots;

    /// Pivot to resume at when we get more data
    unsigned GEResumePivot = 0;


    /// Reset to initial state
    void Reset();

    /// Populate Rows and Columns arrays
    void PopulateColumns(const unsigned oldColumns, const unsigned newColumns);
    void PopulateRows(const unsigned oldRows, const unsigned newRows);

    /// Generate the matrix
    bool GenerateMatrix();

    /// Attempt to put the matrix in upper-triangular form
    bool GaussianElimination();

    /// Decrement all the element counters by a given amount
    void DecrementElementCounters(const unsigned elementCount);

protected:
    /// Resume GE from a previous failure point
    void ResumeGE(const unsigned oldRows, const unsigned rows);

    /// Run GE with pivots after a column is found to be zero
    bool PivotedGaussianElimination(unsigned pivot_i);

    /// rem_row[] += ge_row[] * y
    SIAMESE_FORCE_INLINE void MulAddRows(
        const uint8_t* ge_row, uint8_t* rem_row, unsigned columnStart,
        const unsigned columnEnd, uint8_t y)
    {
#ifdef GF256_ALIGNED_ACCESSES
        // Do unaligned operations first
        // Note: Each row starts at an aliged address
        unsigned unalignedEnd = NextAlignedOffset(columnStart);
        if (unalignedEnd > columnEnd)
            unalignedEnd = columnEnd;
        for (; columnStart < unalignedEnd; ++columnStart)
            rem_row[columnStart] ^= gf256_mul(ge_row[columnStart], y);
        if (columnStart >= columnEnd)
            return;
#endif

        gf256_muladd_mem(rem_row + columnStart, y, ge_row + columnStart, columnEnd - columnStart);
    }

    /// Internal function common to both GE functions, used to eliminate a row of data
    SIAMESE_FORCE_INLINE bool EliminateRow(
        const uint8_t* ge_row, uint8_t* rem_row, const unsigned pivot_i,
        const unsigned columnEnd, const uint8_t val_i)
    {
        // Skip if the element j,i is already zero
        const uint8_t val_j = rem_row[pivot_i];
        if (val_j == 0)
            return false;

        // Calculate element j,i elimination constant based on pivot row value
        const uint8_t y = gf256_div(val_j, val_i);

        // Remember what value was used to zero element j,i
        rem_row[pivot_i] = y;

        MulAddRows(ge_row, rem_row, pivot_i + 1, columnEnd, y);
        return true;
    }
};


//------------------------------------------------------------------------------
// Decoder

/// Threshold number of elements before removing data
static const unsigned kDecoderRemoveThreshold = 2 * kSubwindowSize;
static_assert(kDecoderRemoveThreshold % kSubwindowSize == 0, "It removes on window boundaries");

class Decoder
{
public:
    Decoder();

    SiameseResult AddRecovery(const SiameseRecoveryPacket& packet);

    SIAMESE_FORCE_INLINE SiameseResult AddOriginal(
        const SiameseOriginalPacket& packet)
    {
        return Window.AddOriginal(packet);
    }

    SIAMESE_FORCE_INLINE SiameseResult IsReadyToDecode()
    {
        // If there are already recovered packets to return:
        if (Window.HasRecoveredPackets || CheckRecoveryPossible())
            return Siamese_Success;
        return Siamese_NeedMoreData;
    }

    SiameseResult Decode(
        SiameseOriginalPacket** packetsPtrOut,
        unsigned* countOut);

    SiameseResult Get(SiameseOriginalPacket& packet);

    SiameseResult GenerateAcknowledgement(
        uint8_t* buffer,
        unsigned byteLimit,
        unsigned& usedBytesOut);

    SiameseResult GetStatistics(
        uint64_t* statsOut,
        unsigned statsCount);

protected:
    /// When the allocator goes out of scope all our buffer allocations are freed
    pktalloc::Allocator TheAllocator;

    /// Collected statistics
    DecoderStats Stats;

    /// Region of the solution space we have checked already to enable iterative checks
    CheckedRegionState CheckedRegion;

    /// Received recovery packets
    RecoveryPacketList RecoveryPackets;

    /// Window of original data
    DecoderPacketWindow Window;

    /// Matrix containing recovery packets that may admit a solution
    RecoveryMatrixState RecoveryMatrix;

    /// Product sum for current row
    GrowingAlignedDataBuffer ProductSum;

    /// Filter data that is received out of order by comparing its extent to
    /// the latest column seen so far
    unsigned LatestColumn = 0;


    /// Handle single recovery packet
    bool AddSingleRecovery(const SiameseRecoveryPacket& packet, const RecoveryMetadata& metadata, int footerSize);

    /// Attempt to solve the checked region matrix
    SiameseResult DecodeCheckedRegion();

    /// Returns true if recovery is possible
    bool CheckRecoveryPossible();

    /// Recovery step: Eliminate original data that was successfully received
    bool EliminateOriginalData();

    /// Recovery step: Multiply lower triangle following solution order
    bool MultiplyLowerTriangle();

    /// Recovery step: Back-substitute upper triangle to reveal original data
    SiameseResult BackSubstitution();
};


} // namespace siamese
