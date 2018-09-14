/** \file
    \brief Tonk: Packet Compression
    \copyright Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonk nor the names of its contributors may be
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

#define ZSTD_STATIC_LINKING_ONLY /* Enable advanced API */
#include "thirdparty/zstd/zstd.h" // Zstd
#include "thirdparty/zstd/zstd_errors.h"

namespace tonk {


//------------------------------------------------------------------------------
// Compression Constants

/// Zstd compression level
static const int kCompressionLevel = 1;

/// Compression history buffer size
static const unsigned kCompressionDictBytes = 24 * 1000;


//------------------------------------------------------------------------------
// Ring Buffer

template<size_t kBufferBytes>
class RingBuffer
{
public:
    /// Get a contiguous region that is at least `bytes` in size
    TONK_FORCE_INLINE void* Allocate(unsigned bytes)
    {
        if (NextWriteOffset + bytes > kBufferBytes) {
            NextWriteOffset = 0;
        }
        return Buffer + NextWriteOffset;
    }

    /// Commit some number of bytes up to allocated bytes
    TONK_FORCE_INLINE void Commit(unsigned bytes)
    {
        TONK_DEBUG_ASSERT(NextWriteOffset + bytes <= kBufferBytes);
        NextWriteOffset += bytes;
    }

protected:
    /// Ring buffer that eats its own tail
    uint8_t Buffer[kBufferBytes];

    /// Next offset to write from the front of the buffer
    unsigned NextWriteOffset = 0; ///< in bytes
};


//------------------------------------------------------------------------------
// MessageCompressor

/**
    class MessageCompressor

    This class compresses messages in a reliable-in-order packet stream,
    using a fixed amount of memory to process gigabytes of data.
*/
class MessageCompressor
{
public:
    /**
        Initialize()

        Initialize the compressor with a maximum compressed message size.
        This must match the initialization of the MessageDecompressor.

        Returns an error if the compressor has stopped unexpectedly.
    */
    Result Initialize(unsigned maxCompressedMessagesBytes);

    ~MessageCompressor();

    /**
        Compress()

        Compress data to the destination buffer `destBuffer`.

        Returns writtenBytes = 0 if data should not be compressed.
        In this case, the decompressor must call InsertUncompressed()
        with the same data that was passed to this Compress() call,
        in the same order as the original data.

        Returns written Bytes > 0 if the data was compressed into the
        destination buffer.  In this case, the decompressor must call
        Decompress() with the compressed buffer, in the same order as
        the original data.

        Please see the "Why a Sync Frame is Needed" note below for
        special handling on application usage of this code.

        Returns an error if the compressor has stopped unexpectedly.
    */
    Result Compress(
        const uint8_t* data,
        unsigned bytes,
        uint8_t* dest,
        unsigned& writtenBytes);

protected:
    /// Dictionary history used by decompressor
    RingBuffer<kCompressionDictBytes> History;

    /// Zstd context object used to compress packets
    ZSTD_CCtx* CCtx = nullptr;

    /// Maximum size of compressed data
    unsigned MaxCompressedMessagesBytes = 0;
};

/*
    Why a Sync Frame is Needed:

    When two packets fail to compress in a row, there is
    some special handling that needs to happen to avoid
    data corruption.

    For example:
        [ App -> Client Compression ] --socket--> Server Decompression

    (1) App queues a message of 500 bytes.
    (2) App separately queues a message of 600 bytes.
    (3) Client coalesces these into a single datagram.
    (4) Client sends the combined datagram to the Server.

    If both (1) and (2) fail to compress, then the Server
    will not know if these were both attempted to be
    compressed together or separately, without help.

    In this case it may be a good idea to emit a Sync Frame,
    which gives the receiver a hint to call InsertUncompressed()
    separately for each message.


    What happens if we don't do this?

    If the history ring buffer is near the end and is about
    to wrap around, then if the two messages are compressed
    together, they may both be written to the end.  But if
    they are compressed separately, then the second one can
    instead be written around to the front of the ring.

    This is because when we allocate extra bytes in the ring
    on the decompressor, we do not know how large the message
    is yet.  So we always allocate the same number of bytes
    on the compressor and decompressor sides and then commit
    just the actual number that was needed.

    MessageCompressor::Compress() calls History.Allocate()
    and then History.Commit() to only move the write pointer
    ahead by the number actually committed.  The Allocate()
    behavior needs to be synchronized for the sender and the
    receiver, so this extra synchronization frame forces the
    receiver to treat these as two compression attempts.
*/


//------------------------------------------------------------------------------
// MessageDecompressor

struct Decompressed
{
    const uint8_t* Data;
    unsigned Bytes;
};

/**
    class MessageDecompressor

    This class decompresses messages in a reliable-in-order packet stream,
    using a fixed amount of memory to process gigabytes of data.

    Compressed and uncompressed messages must be delivered to the decompressor
    in the same order as the original calls to Compress(), whether that data
    was successfully compressed or not.
*/
class MessageDecompressor
{
public:
    /**
        Initialize()

        Initialize the compressor with a maximum compressed message size.
        This must match the initialization of the MessageDecompressor.

        Returns an error if the compressor has stopped unexpectedly.
    */
    Result Initialize(unsigned maxCompressedMessagesBytes);

    ~MessageDecompressor();

    /**
        Decompress()

        Decompress a buffer containing one or more messages.

        Returns an error if the decompressor has stopped unexpectedly.
    */
    Result Decompress(
        const void* data,
        unsigned bytes,
        Decompressed& decompressed);

    /**
        InsertUncompressed()

        Insert a message that failed to compress when Compress() was called.

        Returns an error if the decompressor has stopped unexpectedly.
    */
    void InsertUncompressed(
        const uint8_t* data,
        unsigned bytes);

protected:
    /// Dictionary history used by decompressor
    RingBuffer<kCompressionDictBytes> History;

    /// Zstd context object used to decompress packets
    ZSTD_DCtx* DCtx = nullptr;

    /// Maximum size of compressed data
    unsigned MaxCompressedMessagesBytes = 0;
};


} // namespace tonk
