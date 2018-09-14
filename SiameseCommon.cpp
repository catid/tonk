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

#include "SiameseCommon.h"
#include "SiameseSerializers.h"

namespace siamese {


//------------------------------------------------------------------------------
// GrowingAlignedByteMatrix

void GrowingAlignedByteMatrix::Free(pktalloc::Allocator* allocator)
{
    SIAMESE_DEBUG_ASSERT(allocator);
    if (Data)
    {
        allocator->Free(Data);
        Data             = nullptr;
        AllocatedRows    = 0;
        AllocatedColumns = 0;
    }
}

bool GrowingAlignedByteMatrix::Initialize(pktalloc::Allocator* allocator, unsigned rows, unsigned columns)
{
    Rows    = rows;
    Columns = columns;
    AllocatedRows    = rows + kExtraRows;
    AllocatedColumns = pktalloc::NextAlignedOffset(columns + kMinExtraColumns);

    Data = allocator->Reallocate(Data, AllocatedRows * AllocatedColumns, pktalloc::Realloc::Uninitialized);

    return Data != nullptr;
}

bool GrowingAlignedByteMatrix::Resize(pktalloc::Allocator* allocator, unsigned rows, unsigned columns)
{
    SIAMESE_DEBUG_ASSERT(allocator && rows > 0 && columns > 0 && columns <= kColumnPeriod);
    if (rows <= AllocatedRows && columns <= AllocatedColumns)
    {
        Rows    = rows;
        Columns = columns;
        return true;
    }

    const unsigned allocatedRows    = rows + kExtraRows;
    const unsigned allocatedColumns = pktalloc::NextAlignedOffset(columns + kMinExtraColumns);

    uint8_t* buffer = allocator->Allocate(allocatedRows * allocatedColumns);
    if (!buffer)
    {
        Free(allocator);
        return false;
    }

    // If we already allocated a buffer:
    if (Data)
    {
        uint8_t* oldBuffer        = Data;
        const unsigned oldColumns = Columns;

        if (oldColumns > 0)
        {
            // Maintain old data
            const unsigned oldRows   = Rows;
            const unsigned oldStride = AllocatedColumns;
            uint8_t* destRow = buffer;
            uint8_t* srcRow  = oldBuffer;

            unsigned copyCount = oldColumns;
            if (copyCount > columns)
            {
                SIAMESE_DEBUG_BREAK(); // Should never happen
                copyCount = columns;
            }

            for (unsigned i = 0; i < oldRows; ++i, destRow += allocatedColumns, srcRow += oldStride)
                memcpy(destRow, srcRow, copyCount);
        }

        allocator->Free(oldBuffer);
    }

    AllocatedRows    = allocatedRows;
    AllocatedColumns = allocatedColumns;
    Rows    = rows;
    Columns = columns;
    Data    = buffer;
    return true;
}


//------------------------------------------------------------------------------
// OriginalPacket

unsigned OriginalPacket::Initialize(pktalloc::Allocator* allocator, const SiameseOriginalPacket& packet)
{
    SIAMESE_DEBUG_ASSERT(allocator && packet.Data && packet.DataBytes > 0 && packet.PacketNum < kColumnPeriod);

    // Allocate space for the packet
    const unsigned bufferSize = kMaxPacketLengthFieldBytes + packet.DataBytes;
    if (!Buffer.Initialize(allocator, bufferSize))
        return 0;

    // Serialize the packet length into the front using a compressed format
    HeaderBytes = SerializeHeader_PacketLength(packet.DataBytes, Buffer.Data);
    SIAMESE_DEBUG_ASSERT(HeaderBytes <= kMaxPacketLengthFieldBytes);

    // Copy packet data after the length
    memcpy(Buffer.Data + HeaderBytes, packet.Data, packet.DataBytes);

    Buffer.Bytes = HeaderBytes + packet.DataBytes;

    Column = packet.PacketNum;

    return HeaderBytes;
}


} // namespace siamese
