/** \file
    \brief Siamese FEC Implementation: Codec Common Definitions
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
    This module provides core tools and constants used by the codec:

    + Parameters of the Siamese and Cauchy matrix structures
    + Growing buffer/matrix structures
    + OriginalPacket structure
    + RecoveryMetadata structure
*/

#include "siamese.h"
#include "SiameseTools.h"

#include "PacketAllocator.h"
#include "Logger.h"

#include "gf256.h"
static_assert(PKTALLOC_ALIGN_BYTES == GF256_ALIGN_BYTES, "headers are fighting");

#include <new>
#include <vector>
#include <array>
#include <algorithm>

/*
    Ideas:
    + Steal bits from length field for recovery metadata
    + Packet Ids should be relative to the last acknowledged packet to save bytes
*/

namespace siamese {


//------------------------------------------------------------------------------
// Compiler Flags

/// Mix in Cauchy and parity rows to improve recovery rate and speed if possible
#define SIAMESE_ENABLE_CAUCHY

/// Verbose diagnostic output
//#define SIAMESE_DECODER_DUMP_SOLVER_PERF
//#define SIAMESE_DECODER_DUMP_VERBOSE
//#define SIAMESE_ENCODER_DUMP_VERBOSE


//------------------------------------------------------------------------------
// Code Parameters

/// Maximum recovery loss count to avoid causing huge delays or hitting limits
static const unsigned kMaximumLossRecoveryCount = 255;

/// Number of values 3..255 that we cycle through
static const unsigned kColumnValuePeriod = 253;

/// Number of values 1..255 that we cycle through
static const unsigned kRowValuePeriod = 255;


SIAMESE_FORCE_INLINE uint8_t GetColumnValue(unsigned column)
{
    // Note: This LCG visits each value exactly once
    return (uint8_t)(3 + (column * 199) % kColumnValuePeriod);
}

SIAMESE_FORCE_INLINE uint8_t GetRowValue(unsigned row)
{
    return (uint8_t)(1 + (row + 1) % kRowValuePeriod);
}


/// Choose the row/column periods to be a power-of-two to simplify wrap-around calculations
static const unsigned kColumnPeriod = 0x400000;
static const unsigned kRowPeriod = kRowValuePeriod;

/// Returns true if the provided column difference is negative
SIAMESE_FORCE_INLINE bool IsColumnDeltaNegative(unsigned columnDelta)
{
    return (columnDelta >= kColumnPeriod / 2);
}

/// Returns delta (column_a - column_b) between two columns
SIAMESE_FORCE_INLINE unsigned SubtractColumns(unsigned column_a, unsigned column_b)
{
    return (column_a - column_b) % kColumnPeriod;
}

/// Returns sum of two columns as a column number
SIAMESE_FORCE_INLINE unsigned AddColumns(unsigned column_a, unsigned column_b)
{
    return (column_a + column_b) % kColumnPeriod;
}

/// Increments a column number by 1 and returns the incremented value
SIAMESE_FORCE_INLINE unsigned IncrementColumn1(unsigned column)
{
    return AddColumns(column, 1);
}

/// Number of parallel lanes to run
/// Lane#(Column) = Column % kColumnLaneCount
static const unsigned kColumnLaneCount = 8;

/// Number of running sums of original data
/// Note: This cannot be tuned without making code changes
/// Sum 0 = Parity XOR of all input data
/// Sum 1 = Product #1 sum XOR of all input data times its GetColumnValue()
/// Sum 2 = Product #2 sum XOR of all input data times its GetColumnValue() squared
static const unsigned kColumnSumCount = 3;

/// Rate at which we add random pairs of data
static const unsigned kPairAddRate = 16;

/// Keep this number of columns in each subwindow.
/// Data is eliminated from the front of the window at intervals
/// of this size in order to reduce the cost of elimination
static const unsigned kSubwindowSize = kColumnLaneCount * 8;

/// Thomas Wang's 32-bit -> 32-bit integer hash function
/// http://burtleburtle.net/bob/hash/integer.html
SIAMESE_FORCE_INLINE uint32_t Int32Hash(uint32_t key)
{
    key += ~(key << 15);
    key ^= (key >> 10);
    key += (key << 3);
    key ^= (key >> 6);
    key += ~(key << 11);
    key ^= (key >> 16);
    return key;
}

/// Calculate operation code for the given row and lane
SIAMESE_FORCE_INLINE unsigned GetRowOpcode(unsigned lane, unsigned row)
{
    SIAMESE_DEBUG_ASSERT(lane < kColumnLaneCount && row < kRowPeriod);
    static const uint32_t kSumMask = (1 << (kColumnSumCount * 2)) - 1;
    static const uint32_t kZeroValue = (1 << ((kColumnSumCount - 1) * 2));

    // This offset tunes the quality of the upper left of the generated matrix,
    // which is encountered in practice for the first block of input data
    static const unsigned kArbitraryOffset = 3;

    const uint32_t opcode = Int32Hash(lane + (row + kArbitraryOffset) * kColumnLaneCount) & kSumMask;
    return (opcode == 0) ? kZeroValue : (unsigned)opcode;
}


//------------------------------------------------------------------------------
// MDS Erasure Codes using Cauchy Matrix

/**
    For short codes the Siamese matrix multiplication algorithm is slower than
    Cauchy matrix multiplication, so when the number of columns is small we use
    the faster matrix structure instead, which also has perfect recovery rate.

    But the MDS Cauchy matrix is limited: It can only be used for around 200
    input packets in practice.  Furthermore it starts running slower around 64
    input packets so after that point we switch over to the full Siamese codec.
*/
#ifdef SIAMESE_ENABLE_CAUCHY

/// At/below this number of packets, we use a Cauchy coefficients for better
/// recovery properties and faster encoding.
/// For example, a window of 64 packets will use Cauchy instead of Siamese sums.
#define SIAMESE_CAUCHY_THRESHOLD 64 /**< Better under 6 recovery than Siamese */

/// If the window shrinks at/below this number of packets, we switch back to
/// using Cauchy coefficients for matrix rows.  This should be less than the
/// SIAMESE_CAUCHY_THRESHOLD to allow some hysteresis (fuzzy-logic).
#define SIAMESE_SUM_RESET_THRESHOLD 32 /**< Reset sums if window shrinks */

static const unsigned kCauchyMaxColumns = SIAMESE_CAUCHY_THRESHOLD;
static const unsigned kCauchyMaxRows = 256 - kCauchyMaxColumns;

static_assert(kCauchyMaxColumns <= 128, "too high");


/// Cauchy matrix element definitions for short codes
///  CauchyElement(i, j) = 1 / (X(i) - Y(j)) in GF(2^^8)
///      X(i) = i + kCauchyMaxColumns
///      Y(j) = j
/// Preconditions: row < kCauchyMaxRows, column < kCauchyMaxColumns
SIAMESE_FORCE_INLINE uint8_t CauchyElement(unsigned row, unsigned column)
{
    SIAMESE_DEBUG_ASSERT(row < kCauchyMaxRows && column < kCauchyMaxColumns);
    const uint8_t y_j = (uint8_t)column;
    const uint8_t x_i = (uint8_t)(row + kCauchyMaxColumns);
    return gf256_inv(x_i ^ y_j);
}

#endif // SIAMESE_ENABLE_CAUCHY


//------------------------------------------------------------------------------
// GrowingAlignedDataBuffer

/// A buffer of data that can grow as needed and has good memory alignment for
/// using SIMD operations on it.
struct GrowingAlignedDataBuffer
{
    /// Buffer data
    uint8_t* Data = nullptr;

    /// Number of bytes of valid data
    unsigned Bytes = 0;


    /// Growing mantaining existing data in the buffer
    /// Newly grown buffer space will be initialized to zeros
    bool GrowZeroPadded(pktalloc::Allocator* allocator, unsigned bytes)
    {
        SIAMESE_DEBUG_ASSERT(allocator && bytes > 0);
        if (!Data || bytes > Bytes)
        {
            Data = allocator->Reallocate(Data, bytes, pktalloc::Realloc::CopyExisting);
            if (!Data)
            {
                Bytes = 0;
                return false;
            }

            memset(Data + Bytes, 0, bytes - Bytes);
            Bytes = bytes;
        }
        return true;
    }

    /// Growing *dropping* existing data in the buffer
    /// Newly grown buffer space will *not* be initialized to zeros
    bool Initialize(pktalloc::Allocator* allocator, unsigned bytes)
    {
        SIAMESE_DEBUG_ASSERT(allocator && bytes > 0);
        Data = allocator->Reallocate(Data, bytes, pktalloc::Realloc::Uninitialized);
        if (!Data)
        {
            Bytes = 0;
            return false;
        }
        Bytes = bytes;
        return true;
    }

    /// Free allocated memory
    void Free(pktalloc::Allocator* allocator)
    {
        SIAMESE_DEBUG_ASSERT(allocator);
        if (Data)
        {
            allocator->Free(Data);
            Data = nullptr;
            Bytes = 0;
        }
    }
};


//------------------------------------------------------------------------------
// GrowingAlignedByteMatrix

/// This is a matrix of bytes where the elements are stored in row-first order
/// and the first byte element of each row is aligned to cache-line boundaries.
/// Furthermore the matrix can grow in rows or columns, keeping existing data.
struct GrowingAlignedByteMatrix
{
    /// Buffer data
    uint8_t* Data = nullptr;

    /// Used rows, columns
    unsigned Rows    = 0;
    unsigned Columns = 0;

    /// Allocate a few extra rows, columns whenenver we grow the matrix
    /// This is tuned for the expected maximum recovery failure rate
    static const unsigned kExtraRows       = 4;
    static const unsigned kMinExtraColumns = 4;

    /// Allocated rows, columns
    unsigned AllocatedRows    = 0;
    unsigned AllocatedColumns = 0;


    /// Initialize matrix to the given size
    /// New elements have undefined initial state
    bool Initialize(pktalloc::Allocator* allocator, unsigned rows, unsigned columns);

    /// Growing mantaining existing data in the buffer
    /// New elements have undefined initial state
    bool Resize(pktalloc::Allocator* allocator, unsigned rows, unsigned columns);

    /// Reset to initial empty state
    SIAMESE_FORCE_INLINE void Clear()
    {
        Rows    = 0;
        Columns = 0;
    }

    /// Getter
    SIAMESE_FORCE_INLINE uint8_t Get(unsigned row, unsigned column)
    {
        SIAMESE_DEBUG_ASSERT(Data && row < Rows && column < Columns);
        return Data[row * AllocatedColumns + column];
    }

    /// Free allocated memory
    void Free(pktalloc::Allocator* allocator);
};


//------------------------------------------------------------------------------
// OriginalPacket

/// Original packet
struct OriginalPacket
{
    /// Original packet data, prefixed with length field
    GrowingAlignedDataBuffer Buffer;

    /// Keep track of the column index for this packet
    unsigned Column = 0;

    /// Keep track of the number of bytes for header on the packet data
    unsigned HeaderBytes = 0;


    /// Write data to buffer with length prefix and initialize other members
    /// Returns the number of bytes overhead, or 0 on out-of-memory error
    unsigned Initialize(pktalloc::Allocator* allocator, const SiameseOriginalPacket& packet);
};


//------------------------------------------------------------------------------
// Recovery Metadata

/// Metadata header attached to each recovery packet
struct RecoveryMetadata
{
    /// Recovery packet identifer 0..SIAMESE_RECOVERY_NUM_MAX
    unsigned Row; ///< 1 byte

    /// A value between 0..SIAMESE_PACKET_NUM_MAX
    unsigned ColumnStart; ///< up to 3 bytes

    /// Number of packets in the sum set 1..SIAMESE_MAX_PACKETS
    /// These start from ColumnStart
    unsigned SumCount; ///< up to 2 bytes

    /// Number of packets in the LDPC set 1..SIAMESE_MAX_PACKETS
    /// These are on the right side and overlapping with the sum set
    unsigned LDPCCount; ///< up to 2 bytes

    /**
        Visualization of the relationship between ColumnStart,
        SumCount, and LDPCCount:

                             | - - LDPC Count - - |
            | - - - - - - Sum Count - - - - - - - |
            ^
            ColumnStart
    */
};


} // namespace siamese
