/** \file
    \brief Siamese FEC Implementation: Serialization
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
    Serializers are split off into this file for easier audits:

    + Plain-old data serialization
    + WriteByteStream helper class for wrapping an output byte buffer
    + ReadByteStream helper class for wrapping an input byte buffer

    Siamese format serialization:

    + Packet Number (1-3 bytes)
    + Packet Count (1-2 bytes)
    + Packet Length (1-4 bytes)
    + NACK Loss Range (1-7 bytes)
*/

#include "siamese.h"
#include "SiameseTools.h"
#include "SiameseCommon.h"
#include "gf256.h"

namespace siamese {


//------------------------------------------------------------------------------
// POD Serialization

SIAMESE_FORCE_INLINE uint16_t ReadU16_LE(const uint8_t* data)
{
#ifdef GF256_ALIGNED_ACCESSES
    return ((uint16_t)data[1] << 8) | data[0];
#else
    return *(uint16_t*)data;
#endif
}

SIAMESE_FORCE_INLINE uint32_t ReadU24_LE(const uint8_t* data)
{
    return ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
}

/// This version uses one memory read on Intel but requires at least 4 bytes in the buffer
SIAMESE_FORCE_INLINE uint32_t ReadU24_LE_Min4Bytes(const uint8_t* data)
{
#ifdef GF256_ALIGNED_ACCESSES
    return ReadU24_LE(data);
#else
    return *(uint32_t*)data & 0xFFFFFF;
#endif
}

SIAMESE_FORCE_INLINE uint32_t ReadU32_LE(const uint8_t* data)
{
#ifdef GF256_ALIGNED_ACCESSES
    return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
#else
    return *(uint32_t*)data;
#endif
}

SIAMESE_FORCE_INLINE uint64_t ReadU64_LE(const uint8_t* data)
{
#ifdef GF256_ALIGNED_ACCESSES
    return ((uint64_t)data[7] << 56) | ((uint64_t)data[6] << 48) | ((uint64_t)data[5] << 40) |
           ((uint64_t)data[4] << 32) | ((uint64_t)data[3] << 24) | ((uint64_t)data[2] << 16) |
           ((uint64_t)data[1] << 8) | data[0];
#else
    return *(uint64_t*)data;
#endif
}

SIAMESE_FORCE_INLINE void WriteU16_LE(uint8_t* data, uint16_t value)
{
#ifdef GF256_ALIGNED_ACCESSES
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint16_t*)data = value;
#endif
}

SIAMESE_FORCE_INLINE void WriteU24_LE(uint8_t* data, uint32_t value)
{
    data[2] = (uint8_t)(value >> 16);
    WriteU16_LE(data, (uint16_t)value);
}

SIAMESE_FORCE_INLINE void WriteU24_LE_Min4Bytes(uint8_t* data, uint32_t value)
{
#ifdef GF256_ALIGNED_ACCESSES
    WriteU24_LE(data, value);
#else
    *(uint32_t*)data = value;
#endif
}

SIAMESE_FORCE_INLINE void WriteU32_LE(uint8_t* data, uint32_t value)
{
#ifdef GF256_ALIGNED_ACCESSES
    data[3] = (uint8_t)(value >> 24);
    data[2] = (uint8_t)(value >> 16);
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint32_t*)data = value;
#endif
}

SIAMESE_FORCE_INLINE void WriteU64_LE(uint8_t* data, uint64_t value)
{
#ifdef GF256_ALIGNED_ACCESSES
    data[7] = (uint8_t)(value >> 56);
    data[6] = (uint8_t)(value >> 48);
    data[5] = (uint8_t)(value >> 40);
    data[4] = (uint8_t)(value >> 32);
    data[3] = (uint8_t)(value >> 24);
    data[2] = (uint8_t)(value >> 16);
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint64_t*)data = value;
#endif
}


//------------------------------------------------------------------------------
// WriteByteStream

/// Helper class to serialize POD types to a byte buffer
struct WriteByteStream
{
    /// Wrapped data pointer
    uint8_t* Data = nullptr;

    /// Number of wrapped buffer bytes
    unsigned BufferBytes = 0;

    /// Number of bytes written so far by Write*() functions
    unsigned WrittenBytes = 0;

    explicit WriteByteStream()
    {
    }
    explicit WriteByteStream(uint8_t* data, uint64_t bytes)
        : Data(data)
        , BufferBytes((unsigned)bytes)
    {
        SIAMESE_DEBUG_ASSERT(data != nullptr && bytes > 0);
    }

    SIAMESE_FORCE_INLINE uint8_t* Peek()
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes <= BufferBytes);
        return Data + WrittenBytes;
    }
    SIAMESE_FORCE_INLINE unsigned Remaining()
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes <= BufferBytes);
        return BufferBytes - WrittenBytes;
    }

    SIAMESE_FORCE_INLINE void Write8(uint8_t value)
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes + 1 <= BufferBytes);
        Data[WrittenBytes] = value;
        WrittenBytes++;
    }
    SIAMESE_FORCE_INLINE void Write16(uint16_t value)
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes + 2 <= BufferBytes);
        WriteU16_LE(Data + WrittenBytes, value);
        WrittenBytes += 2;
    }
    SIAMESE_FORCE_INLINE void Write24(uint32_t value)
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes + 3 <= BufferBytes);
        WriteU24_LE(Data + WrittenBytes, value);
        WrittenBytes += 3;
    }
    SIAMESE_FORCE_INLINE void Write32(uint32_t value)
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes + 4 <= BufferBytes);
        WriteU32_LE(Data + WrittenBytes, value);
        WrittenBytes += 4;
    }
    SIAMESE_FORCE_INLINE void Write64(uint64_t value)
    {
        SIAMESE_DEBUG_ASSERT(WrittenBytes + 8 <= BufferBytes);
        WriteU64_LE(Data + WrittenBytes, value);
        WrittenBytes += 8;
    }
    SIAMESE_FORCE_INLINE void WriteBuffer(const void* source, size_t bytes)
    {
        SIAMESE_DEBUG_ASSERT(source != nullptr || bytes == 0);
        SIAMESE_DEBUG_ASSERT(WrittenBytes + bytes <= BufferBytes);
        memcpy(Data + WrittenBytes, source, bytes);
        WrittenBytes += static_cast<unsigned>(bytes);
    }
};


//------------------------------------------------------------------------------
// ReadByteStream

/// Helper class to deserialize POD types from a byte buffer
struct ReadByteStream
{
    /// Wrapped data pointer
    const uint8_t* const Data;

    /// Number of wrapped buffer bytes
    const unsigned BufferBytes;

    /// Number of bytes read so far by Read*() functions
    unsigned BytesRead;


    ReadByteStream(const uint8_t* data, uint64_t bytes)
        : Data(data)
        , BufferBytes((unsigned)bytes)
    {
        SIAMESE_DEBUG_ASSERT(data != nullptr);
        BytesRead = 0;
    }

    SIAMESE_FORCE_INLINE const uint8_t* Peek()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead <= BufferBytes);
        return Data + BytesRead;
    }
    SIAMESE_FORCE_INLINE unsigned Remaining()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead <= BufferBytes);
        return BufferBytes - BytesRead;
    }
    SIAMESE_FORCE_INLINE void Skip(unsigned bytes)
    {
        SIAMESE_DEBUG_ASSERT(BytesRead + bytes <= BufferBytes);
        BytesRead += bytes;
    }

    SIAMESE_FORCE_INLINE const uint8_t* Read(unsigned bytes)
    {
        const uint8_t* data = Peek();
        Skip(bytes);
        return data;
    }
    SIAMESE_FORCE_INLINE uint8_t Read8()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead + 1 <= BufferBytes);
        uint8_t value = *Peek();
        BytesRead++;
        return value;
    }
    SIAMESE_FORCE_INLINE uint16_t Read16()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead + 2 <= BufferBytes);
        uint16_t value = ReadU16_LE(Peek());
        BytesRead += 2;
        return value;
    }
    SIAMESE_FORCE_INLINE uint32_t Read24()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead + 3 <= BufferBytes);
        uint32_t value = ReadU24_LE(Peek());
        BytesRead += 3;
        return value;
    }
    SIAMESE_FORCE_INLINE uint32_t Read32()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead + 4 <= BufferBytes);
        uint32_t value = ReadU32_LE(Peek());
        BytesRead += 4;
        return value;
    }
    SIAMESE_FORCE_INLINE uint64_t Read64()
    {
        SIAMESE_DEBUG_ASSERT(BytesRead + 8 <= BufferBytes);
        uint64_t value = ReadU64_LE(Peek());
        BytesRead += 8;
        return value;
    }
};


//------------------------------------------------------------------------------
// C++ Data Serialization: Packet Number

/// I suggest using this encoding to reduce the original data packet overhead.
/// This is used internally by the codec to encode the recovery packet header.

/// Maximum number of bytes used by the packet number in this encoding
static const unsigned kMaxPacketNumEncodedBytes = 3;

/// Serialize packet number into the front of a buffer, using 1-3 bytes.
/// Returns number of bytes written.
/// Preconditions:
///  + Input must range between 0..SIAMESE_PACKET_NUM_MAX.
///  + Buffer must have at least 3 bytes available.
SIAMESE_FORCE_INLINE unsigned SerializeHeader_PacketNum(unsigned packetNum, uint8_t* buffer)
{
    if (packetNum <= 0x7f)
    {
        buffer[0] = (uint8_t)packetNum;
        return 1;
    }

    if (packetNum <= 0x3fff)
    {
        buffer[0] = (uint8_t)(0x80 | (packetNum >> 8));
        buffer[1] = (uint8_t)packetNum;
        return 2;
    }

    buffer[0] = (uint8_t)(0xC0 | (packetNum >> 16));
    buffer[1] = (uint8_t)(packetNum >> 8);
    buffer[2] = (uint8_t)packetNum;
    return 3;
}

/// Deserialize packet number from the front of a buffer, using 1-3 bytes.
/// Returns number of bytes read and the decoded packet number.
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeHeader_PacketNum(const uint8_t* buffer, int bufferSpaceBytes, unsigned& packetNumOut)
{
    if (!buffer || bufferSpaceBytes < 1)
        return -1;

    const int byteCount = (buffer[0] >> 6);
    if (byteCount <= 1)
    {
        // High bit is clear: 7 bits of data
        packetNumOut = buffer[0];
        return 1;
    }

    if (bufferSpaceBytes < byteCount)
        return -1;

    if (byteCount == 2)
        packetNumOut = (((unsigned)buffer[0] << 8) | buffer[1]) & 0x3fff;
    else
        packetNumOut = (((unsigned)buffer[0] << 16) | ((unsigned)buffer[1] << 8) | buffer[2]) & 0x3fffff;
    return byteCount;
}


/// Serialize packet number into the end of a buffer, using 1-3 bytes.
/// Returns number of bytes written.
/// Preconditions:
///  + Input must range between 0..SIAMESE_PACKET_NUM_MAX.
///  + Buffer must have at least 3 bytes available.
SIAMESE_FORCE_INLINE unsigned SerializeFooter_PacketNum(unsigned packetNum, uint8_t* buffer)
{
    if (packetNum <= 0x7f)
    {
        buffer[0] = (uint8_t)packetNum;
        return 1;
    }

    if (packetNum <= 0x3fff)
    {
        buffer[0] = (uint8_t)packetNum;
        buffer[1] = (uint8_t)(0x80 | (packetNum >> 8));
        return 2;
    }

    buffer[0] = (uint8_t)packetNum;
    buffer[1] = (uint8_t)(packetNum >> 8);
    buffer[2] = (uint8_t)(0xC0 | (packetNum >> 16));
    return 3;
}

/// Deserialize packet number from the end of a buffer, using 1-3 bytes.
/// Returns number of bytes read and the decoded packet number.
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeFooter_PacketNum(const uint8_t* buffer, int bufferSpaceBytes, unsigned& packetNumOut)
{
    if (!buffer || bufferSpaceBytes < 1)
        return -1;

    buffer += bufferSpaceBytes - 1;
    const int byteCount = (buffer[0] >> 6);
    if (byteCount <= 1)
    {
        packetNumOut = buffer[0];
        return 1;
    }

    if (bufferSpaceBytes < byteCount)
    {
        return -1;
    }

    if (byteCount == 2)
        packetNumOut = (((unsigned)buffer[0] << 8) | buffer[-1]) & 0x3fff;
    else
        packetNumOut = (((unsigned)buffer[0] << 16) | ((unsigned)buffer[-1] << 8) | buffer[-2]) & 0x3fffff;
    return byteCount;
}

/// Deserialize packet number from the end of a buffer, using 1-3 bytes.
/// Returns number of bytes read and the decoded packet number.
/// Note: This version does no length checking so it only works if the buffer contains at least 3 bytes
SIAMESE_FORCE_INLINE unsigned DeserializeFooter_PacketNum_Unsafe(const uint8_t* bufferEnd, unsigned& packetNumOut)
{
    const unsigned byteCount = (bufferEnd[-1] >> 6);
    if (byteCount <= 1)
    {
        packetNumOut = bufferEnd[-1];
        return 1;
    }
    if (byteCount == 2)
        packetNumOut = (((unsigned)bufferEnd[-1] << 8) | bufferEnd[-2]) & 0x3fff;
    else
        packetNumOut = (((unsigned)bufferEnd[-1] << 16) | ((unsigned)bufferEnd[-2] << 8) | bufferEnd[-3]) & 0x3fffff;
    return byteCount;
}


//------------------------------------------------------------------------------
// Data Serialization: Packet Count

static const unsigned kMaxPacketCountFieldBytes = 2;


/// Serialize count into the front of a buffer, using 1 or 2 bytes
/// Returns number of bytes written
/// Preconditions:
///  + Input must range between 1..SIAMESE_MAX_PACKETS
///  + Buffer must have at least kMaxPacketCountFieldBytes bytes available
SIAMESE_FORCE_INLINE unsigned SerializeHeader_PacketCount(unsigned count, uint8_t* buffer)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr && count >= 1 && count <= SIAMESE_MAX_PACKETS);

    if (count <= 127)
    {
        buffer[0] = (uint8_t)count;
        return 1;
    }

    buffer[0] = (uint8_t)(0x80 | (count >> 8));
    buffer[1] = (uint8_t)count;
    return 2;
}

/// Deserialize count from the front of a buffer, using 1 or 2 bytes
/// Returns number of bytes read and the decoded count
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeHeader_PacketCount(const uint8_t* buffer, unsigned bufferSpaceBytes, unsigned& countOut)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);

    if (bufferSpaceBytes < 1)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    if ((buffer[0] & 0x80) == 0)
    {
        countOut = buffer[0];
        return 1;
    }

    if (bufferSpaceBytes < 2)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    countOut = (buffer[1] | ((unsigned)buffer[0] << 8)) & 0x7fff;
    return 2;
}


/// Serialize count into the back of a buffer, using 1 or 2 bytes
/// Returns number of bytes written
/// Preconditions:
///  + Input must range between 1..SIAMESE_MAX_PACKETS
///  + Buffer must have at least kMaxPacketCountFieldBytes bytes available
SIAMESE_FORCE_INLINE unsigned SerializeFooter_PacketCount(unsigned count, uint8_t* buffer)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr && count <= SIAMESE_MAX_PACKETS);

    if (count <= 127)
    {
        buffer[0] = (uint8_t)count;
        return 1;
    }

    buffer[0] = (uint8_t)count;
    buffer[1] = (uint8_t)(0x80 | (count >> 8));
    return 2;
}

/// Deserialize count from the back of a buffer, using 1 or 2 bytes
/// Returns number of bytes read and the decoded count
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeFooter_PacketCount(const uint8_t* buffer, unsigned bufferSpaceBytes, unsigned& countOut)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);

    if (bufferSpaceBytes < 1)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    buffer += bufferSpaceBytes - 1;
    if ((buffer[0] & 0x80) == 0)
    {
        countOut = buffer[0];
        return 1;
    }

    if (bufferSpaceBytes < 2)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    countOut = (((unsigned)buffer[0] << 8) | buffer[-1]) & 0x7fff;
    return 2;
}


//------------------------------------------------------------------------------
// Data Serialization: Packet Length

/// Maximum number of bytes that may be used for packet length field
static const unsigned kMaxPacketLengthFieldBytes = 4;


/// Serialize packet length into the front of a buffer, using 1-4 bytes.
/// Returns number of bytes written.
/// Preconditions:
///  + Input must range between 1..SIAMESE_MAX_PACKET_BYTES.
///  + Buffer must have at least kMaxPacketLengthFieldBytes bytes available.
SIAMESE_FORCE_INLINE unsigned SerializeHeader_PacketLength(unsigned length, uint8_t* buffer)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);
    SIAMESE_DEBUG_ASSERT(length > 0 && length <= 0x1fffffff);
    if (length <= 0x7f)
    {
        buffer[0] = (uint8_t)length;
        return 1;
    }
    else if (length <= 0x3fff)
    {
        buffer[0] = (uint8_t)(0x80 | (length >> 8));
        buffer[1] = (uint8_t)length;
        return 2;
    }
    else if (length <= 0x1fffff)
    {
        buffer[0] = (uint8_t)(0xC0 | (length >> 16));
        buffer[1] = (uint8_t)(length >> 8);
        buffer[2] = (uint8_t)length;
        return 3;
    }
    buffer[0] = (uint8_t)(0xE0 | (length >> 24));
    buffer[1] = (uint8_t)(length >> 16);
    buffer[2] = (uint8_t)(length >> 8);
    buffer[3] = (uint8_t)length;
    return 4;
}

/// Deserialize packet length from the front of a buffer, using 1-4 bytes.
/// Returns number of bytes read and the decoded packet length.
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeHeader_PacketLength(const uint8_t* buffer, unsigned bufferSpaceBytes, unsigned& lengthOut)
{
    if (!buffer || bufferSpaceBytes < 1)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    const int byteCount = (buffer[0] >> 6);
    if (byteCount <= 1) // 1 byte:
    {
        lengthOut = buffer[0];
        return 1;
    }
    else if (byteCount == 2) // 2 bytes:
    {
        if (bufferSpaceBytes < 2)
        {
            SIAMESE_DEBUG_BREAK();
            return -1;
        }
        lengthOut = (((unsigned)buffer[0] << 8) | buffer[1]) & 0x3fff;
        return 2;
    }
    else if ((buffer[0] & 0xE0) == 0xC0) // 3 bytes:
    {
        if (bufferSpaceBytes < 3)
        {
            SIAMESE_DEBUG_BREAK();
            return -1;
        }
        lengthOut = (((unsigned)buffer[0] << 16) | ((unsigned)buffer[1] << 8) | buffer[2]) & 0x1fffff;
        return 3;
    }
    // 4 bytes:
    if (bufferSpaceBytes < 4)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    lengthOut = (((unsigned)buffer[0] << 24) | ((unsigned)buffer[1] << 16) |
                 ((unsigned)buffer[2] << 8) | buffer[3]) & 0x1fffffff;
    return 4;
}


/// Serialize packet length into the back of a buffer, using 1-4 bytes.
/// Returns number of bytes written.
/// Preconditions:
///  + Input must range between 1..SIAMESE_MAX_PACKET_BYTES.
///  + Buffer must have at least 4 bytes available.
SIAMESE_FORCE_INLINE unsigned SerializeFooter_PacketLength(unsigned length, uint8_t* buffer)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);
    SIAMESE_DEBUG_ASSERT(length > 0 && length <= 0x1fffffff);

    if (length <= 0x7f)
    {
        buffer[0] = (uint8_t)length;
        return 1;
    }
    else if (length <= 0x3fff)
    {
        buffer[0] = (uint8_t)length;
        buffer[1] = (uint8_t)(0x80 | (length >> 8));
        return 2;
    }
    else if (length <= 0x1fffff)
    {
        buffer[0] = (uint8_t)length;
        buffer[1] = (uint8_t)(length >> 8);
        buffer[2] = (uint8_t)(0xC0 | (length >> 16));
        return 3;
    }
    buffer[0] = (uint8_t)length;
    buffer[1] = (uint8_t)(length >> 8);
    buffer[2] = (uint8_t)(length >> 16);
    buffer[3] = (uint8_t)(0xE0 | (length >> 24));
    return 4;
}

/// Deserialize packet length from the back of a buffer, using 1-4 bytes.
/// Returns number of bytes read and the decoded packet length.
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeFooter_PacketLength(const uint8_t* buffer, unsigned bufferSpaceBytes, unsigned& lengthOut)
{
    if (!buffer || bufferSpaceBytes < 1)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    buffer += bufferSpaceBytes - 1;
    const int byteCount = (buffer[0] >> 6);
    if (byteCount <= 1) // 1 byte:
    {
        lengthOut = buffer[0];
        return 1;
    }
    else if (byteCount == 2) // 2 bytes:
    {
        if (bufferSpaceBytes < 2)
        {
            SIAMESE_DEBUG_BREAK();
            return -1;
        }
        lengthOut = (((unsigned)buffer[0] << 8) | buffer[-1]) & 0x3fff;
        return 2;
    }
    else if ((buffer[0] & 0xE0) == 0xC0) // 3 bytes:
    {
        if (bufferSpaceBytes < 3)
        {
            SIAMESE_DEBUG_BREAK();
            return -1;
        }
        lengthOut = (((unsigned)buffer[0] << 16) | ((unsigned)buffer[-1] << 8) | buffer[-2]) & 0x1fffff;
        return 3;
    }
    // 4 bytes:
    if (bufferSpaceBytes < 4)
    {
        SIAMESE_DEBUG_BREAK();
        return -1;
    }
    lengthOut = (((unsigned)buffer[0] << 24) | ((unsigned)buffer[-1] << 16) |
        ((unsigned)buffer[-2] << 8) | buffer[-3]) & 0x1fffffff;
    return 4;
}


//------------------------------------------------------------------------------
// Data Serialization: Recovery Metadata

/// Maximum number of metadata bytes that may be tagged to the recovery packets
static const int kMaxRecoveryMetadataBytes = 8;


/// Serialize recovery metadata into the back of a buffer, using 2-6 bytes
/// Returns number of bytes written
SIAMESE_FORCE_INLINE unsigned SerializeFooter_RecoveryMetadata(const RecoveryMetadata& metadata, uint8_t* buffer)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);
    SIAMESE_DEBUG_ASSERT(metadata.ColumnStart < kColumnPeriod);

    unsigned bytes = 0;
    if (metadata.SumCount > 1)
    {
        SIAMESE_DEBUG_ASSERT(metadata.Row < kRowPeriod);
        buffer[0] = (uint8_t)metadata.Row;
        ++bytes;
        SIAMESE_DEBUG_ASSERT(metadata.LDPCCount <= metadata.SumCount);
        bytes += SerializeFooter_PacketCount(metadata.LDPCCount, buffer + bytes);
    }
    bytes += SerializeFooter_PacketNum(metadata.ColumnStart, buffer + bytes);
    SIAMESE_DEBUG_ASSERT(metadata.SumCount >= 1);
    bytes += SerializeFooter_PacketCount(metadata.SumCount - 1, buffer + bytes);
    return bytes;
}

/// Deserialize recovery metadata from the back of a buffer, using 2-6 bytes
/// Returns number of bytes read and the decoded count
/// Returns -1 on format error.
SIAMESE_FORCE_INLINE int DeserializeFooter_RecoveryMetadata(const uint8_t* buffer, unsigned bufferSpaceBytes, RecoveryMetadata& metadataOut)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);

    const unsigned originalBufferSpace = bufferSpaceBytes;

    int fieldSize = DeserializeFooter_PacketCount(buffer, bufferSpaceBytes, metadataOut.SumCount);
    if (fieldSize < 0)
        return -1;
    bufferSpaceBytes -= fieldSize;
    metadataOut.SumCount++;

    fieldSize = DeserializeFooter_PacketNum(buffer, bufferSpaceBytes, metadataOut.ColumnStart);
    if (fieldSize < 0)
        return -1;
    bufferSpaceBytes -= fieldSize;

    if (metadataOut.SumCount <= 1)
    {
        metadataOut.LDPCCount = 1;
        metadataOut.Row = 0;
    }
    else
    {
        fieldSize = DeserializeFooter_PacketCount(buffer, bufferSpaceBytes, metadataOut.LDPCCount);
        if (fieldSize < 0)
            return -1;
        bufferSpaceBytes -= fieldSize;
        if (metadataOut.SumCount < metadataOut.LDPCCount)
            return -1;

        if (bufferSpaceBytes >= 1)
            metadataOut.Row = buffer[--bufferSpaceBytes];
        else
        {
            SIAMESE_DEBUG_BREAK();
            return -1;
        }
    }

    return originalBufferSpace - bufferSpaceBytes;
}


//------------------------------------------------------------------------------
// Data Serialization: Negative acknowledgement loss range (NACK loss range)

/// Maximum number of bytes that may be used for NACK loss range
static const unsigned kMaxLossRangeFieldBytes = 7;

/**
    Buffer format:

        <Next Expected Packet Number (1-3 bytes)>
        <Loss Offset and Flags (1-3 bytes)> [Extended Loss Count (0-1 byte)]
        <Loss Offset and Flags (1-3 bytes)> [Extended Loss Count (0-1 byte)]
        ...
        <Loss Offset and Flags (1-3 bytes)> [Extended Loss Count (0-1 byte)]

        The (Next Expected Packet Number) is in the PacketNum serialization format.

    Then there is a series of Lost Packet Ranges compressed as bitfields,
    ordered left-to-right from LSB to MSB:

    <Loss Offset and Flags: CC X NNNNN (1 byte)>

        CC = 00 : Range includes a single packet.
        CC = 01 : Range includes two packets.
        CC = 10 : Range includes three packets.
        CC = 11 : Consecutive Loss Count byte follows the flags.

        X = 1 : This byte is followed by an extended bit field byte.
        X = 0 : This byte is *not* followed by more bitfield bytes.

        NNNNN : Low 5 bits of relative packet number.

    [Loss Offset and Flags Extended 1: NNNNNNN X (1 byte)]
    [Loss Offset and Flags Extended 2: NNNNNNN X (1 byte)]

        X = 1 : This byte is followed by an extended bit field byte.
        X = 0 : This byte is *not* followed by more bit field bytes.

        NNNNNNN : Additional 7 bits of relative packet number.

    [Loss Offset and Flags Extended 3: NNN 00000(1 byte)]

        NNN : High 3 bits of relative packet number.

        00000 : Reserved should be set to 0.

    [Extended Loss Count (0-1 byte)]

        This is offset by 3 since the CC bits in the flags can already
        represent losses ranging between 1 and 3 packets in length.
*/


/// Serialize NACK loss range into the front of a buffer, using 1-kMaxLossRangeFieldBytes bytes.
/// Returns number of bytes written.
/// Preconditions:
///  + Input must range between 0..SIAMESE_MAX_PACKET_BYTES.
///  + Buffer must have at least kMaxLossRangeFieldBytes bytes available.
SIAMESE_FORCE_INLINE unsigned SerializeHeader_NACKLossRange(
    unsigned relativeStart, unsigned lossCountM1, uint8_t* buffer)
{
    SIAMESE_DEBUG_ASSERT(buffer != nullptr);

    SIAMESE_DEBUG_ASSERT(relativeStart <= SIAMESE_PACKET_NUM_MAX);
    static_assert((1 << 22) == SIAMESE_PACKET_NUM_COUNT, "Update this");

    unsigned byte0 = (lossCountM1 <= 2) ? lossCountM1 : 3;
    byte0 |= (relativeStart << 3);
    unsigned encodedBytes = 1;

    if (relativeStart >= (1 << 5))
    {
        unsigned byte1 = relativeStart >> 5;
        if (relativeStart >= (1 << (5 + 7)))
        {
            unsigned byte2 = relativeStart >> (5 + 7);
            if (relativeStart >= (1 << (5 + 7 + 7)))
            {
                buffer[3] = (uint8_t)(relativeStart >> (5 + 7 + 7));
                byte2 |= 0x80;
                ++encodedBytes;
            }
            SIAMESE_DEBUG_ASSERT(relativeStart >= (1 << (5 + 7 + 7)) || byte2 < 128);
            buffer[2] = (uint8_t)byte2;
            byte1 |= 0x80;
            ++encodedBytes;
        }
        SIAMESE_DEBUG_ASSERT(relativeStart >= (1 << (5 + 7)) || byte1 < 128);
        buffer[1] = (uint8_t)byte1;
        byte0 |= 4;
        ++encodedBytes;
    }
    buffer[0] = (uint8_t)byte0;

    // Encode the loss count:
    if (lossCountM1 >= 3)
    {
        // Advance buffer write pointer
        buffer += encodedBytes;

        lossCountM1 -= 3;
        unsigned byte1 = lossCountM1;
        if (lossCountM1 >= (1 << 7))
        {
            unsigned byte2 = (lossCountM1 >> 7);
            if (lossCountM1 >= (1 << (7 + 7)))
            {
                SIAMESE_DEBUG_ASSERT((lossCountM1 >> (7 + 7)) < 256);
                unsigned byte3 = (lossCountM1 >> (7 + 7));
                buffer[2] = (uint8_t)byte3;
                byte2 |= 0x80;
                ++encodedBytes;
            }
            SIAMESE_DEBUG_ASSERT(lossCountM1 >= (1 << (7 + 7)) || byte2 < 128);
            buffer[1] = (uint8_t)byte2;
            byte1 |= 0x80;
            ++encodedBytes;
        }
        SIAMESE_DEBUG_ASSERT(lossCountM1 >= (1 << 7) || byte1 < 128);
        buffer[0] = (uint8_t)byte1;
        ++encodedBytes;
    }

    SIAMESE_DEBUG_ASSERT(encodedBytes <= kMaxLossRangeFieldBytes);
    return encodedBytes;
}

/// Deserialize NACK loss range from the front of a buffer, using 1-kMaxLossRangeFieldBytes bytes.
/// Returns number of bytes read and the decoded packet length.
/// Returns -1 on format error.
/// Precondition: bufferSpaceBytes >= kMaxLossRangeFieldBytes
SIAMESE_FORCE_INLINE int DeserializeHeader_NACKLossRange(const uint8_t* buffer, unsigned bufferSpaceBytes,
                                           unsigned& relativeStartOut, unsigned& lossCountM1Out)
{
    if (!buffer || bufferSpaceBytes < kMaxLossRangeFieldBytes)
    {
        SIAMESE_DEBUG_BREAK(); // Invalid input
        return -1;
    }

    const unsigned byte0   = buffer[0];
    unsigned lossCountM1   = (byte0 & 3);
    unsigned relativeStart = byte0 >> 3;
    unsigned byteCount     = 1;

    if (byte0 & 4)
    {
        ++byteCount;
        const unsigned byte1 = buffer[1];
        relativeStart |= (byte1 & 0x7f) << 5;
        if (byte1 & 0x80)
        {
            ++byteCount;
            const unsigned byte2 = buffer[2];
            relativeStart |= (byte2 & 0x7f) << (5 + 7);
            if (byte2 & 0x80)
            {
                ++byteCount;
                const unsigned byte3 = buffer[3];
                relativeStart |= byte3 << (5 + 7 + 7);
            }
        }
    }

    // If there is an extended loss count:
    if (lossCountM1 == 3)
    {
        buffer += byteCount;

        const unsigned byte1 = buffer[0];
        lossCountM1 += byte1 & 0x7f;
        if (byte1 & 0x80)
        {
            const unsigned byte2 = buffer[1];
            lossCountM1 += (byte2 & 0x7f) << 7;
            if (byte2 & 0x80)
            {
                const unsigned byte3 = buffer[2];
                lossCountM1 += byte3 << (7 + 7);
                ++byteCount;
            }
            ++byteCount;
        }
        ++byteCount;
    }

    relativeStartOut = relativeStart;
    lossCountM1Out   = lossCountM1;

    SIAMESE_DEBUG_ASSERT(byteCount <= kMaxLossRangeFieldBytes);
    return byteCount;
}


} // namespace siamese
