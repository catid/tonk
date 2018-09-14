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

#include "PacketCompression.h"

namespace tonk {


//------------------------------------------------------------------------------
// MessageCompressor

Result MessageCompressor::Initialize(unsigned maxCompressedMessagesBytes)
{
    MaxCompressedMessagesBytes = maxCompressedMessagesBytes;
    CCtx = ZSTD_createCCtx();

    if (!CCtx) {
        return Result("SessionOutgoing::Initialize", "ZSTD_createCCtx failed", ErrorType::Zstd);
    }

    const size_t estimatedPacketSize = MaxCompressedMessagesBytes;

    ZSTD_parameters zParams;

    zParams.cParams = ZSTD_getCParams(
        kCompressionLevel,
        estimatedPacketSize,
        kCompressionDictBytes);

    zParams.fParams.checksumFlag = 0;
    zParams.fParams.contentSizeFlag = 0;
    zParams.fParams.noDictIDFlag = 1;

    const size_t icsResult = ZSTD_compressBegin_advanced(
        CCtx,
        nullptr,
        0,
        zParams,
        ZSTD_CONTENTSIZE_UNKNOWN);

    if (0 != ZSTD_isError(icsResult)) {
        TONK_DEBUG_BREAK();
        return Result("SessionOutgoing::Initialize", "ZSTD_compressBegin_advanced failed", ErrorType::Zstd, icsResult);
    }

    const size_t blockSizeBytes = ZSTD_getBlockSize(CCtx);
    if (blockSizeBytes < MaxCompressedMessagesBytes) {
        return Result("SessionOutgoing::Initialize", "Zstd block size is too small", ErrorType::Zstd);
    }

    return Result::Success();
}

MessageCompressor::~MessageCompressor()
{
    if (CCtx) {
        ZSTD_freeCCtx(CCtx);
    }
}

Result MessageCompressor::Compress(
    const uint8_t* data,
    unsigned bytes,
    uint8_t* destBuffer,
    unsigned& writtenBytes)
{
    TONK_DEBUG_ASSERT(bytes > 0 && destBuffer && data);

    writtenBytes = 0;

    // Insert data into history ring buffer
    TONK_DEBUG_ASSERT(MaxCompressedMessagesBytes >= bytes);
    void* history = History.Allocate(MaxCompressedMessagesBytes);
    memcpy(history, data, bytes);
    History.Commit(bytes);

    // Compress into scratch buffer, leaving room for a frame header
    const size_t result = ZSTD_compressBlock(
        CCtx,
        destBuffer,
        MaxCompressedMessagesBytes,
        history,
        bytes);

    // If no data to compress, or would require too much space,
    // or did not produce a small enough result:
    if (0 == result ||
        (size_t)-ZSTD_error_dstSize_tooSmall == result ||
        result >= bytes)
    {
        // Note: Input data was accumulated into history ring buffer
        return Result::Success();
    }

    // If compression failed:
    if (0 != ZSTD_isError(result))
    {
        std::string reason = "ZSTD_compressBlock failed: ";
        reason += ZSTD_getErrorName(result);
        TONK_DEBUG_BREAK();
        return Result("SessionOutgoing::compress", reason, ErrorType::Zstd, result);
    }

    writtenBytes = static_cast<unsigned>(result);
    TONK_DEBUG_ASSERT(writtenBytes <= MaxCompressedMessagesBytes);

    return Result::Success();
}


//------------------------------------------------------------------------------
// MessageDecompressor

Result MessageDecompressor::Initialize(unsigned maxCompressedMessagesBytes)
{
    MaxCompressedMessagesBytes = maxCompressedMessagesBytes;
    DCtx = ZSTD_createDCtx();

    if (!DCtx) {
        return Result("SessionIncoming::Initialize", "ZSTD_createDCtx failed", ErrorType::Zstd);
    }

    const size_t beginResult = ZSTD_decompressBegin(DCtx);

    if (0 != ZSTD_isError(beginResult)) {
        return Result("SessionIncoming::Initialize", "ZSTD_decompressBegin failed", ErrorType::Zstd, beginResult);
    }

    return Result::Success();
}

MessageDecompressor::~MessageDecompressor()
{
    if (DCtx) {
        ZSTD_freeDCtx(DCtx);
    }
}

void MessageDecompressor::InsertUncompressed(
    const uint8_t* data,
    unsigned bytes)
{
    if (bytes > MaxCompressedMessagesBytes) {
        TONK_DEBUG_BREAK(); // Invalid input
        return;
    }

    void* history = History.Allocate(MaxCompressedMessagesBytes);
    memcpy(history, data, bytes);
    ZSTD_insertBlock(DCtx, history, bytes);
    History.Commit(bytes);
}

Result MessageDecompressor::Decompress(
    const void* data,
    unsigned bytes,
    Decompressed& decompressed)
{
    // Decompress data into history ring buffer
    void* history = History.Allocate(MaxCompressedMessagesBytes);

    const size_t result = ZSTD_decompressBlock(
        DCtx,
        history,
        MaxCompressedMessagesBytes,
        data,
        bytes);

    // If decompression failed:
    if (0 == result || 0 != ZSTD_isError(result))
    {
        std::string reason = "ZSTD_decompressBlock failed: ";
        reason += ZSTD_getErrorName(result);
        TONK_DEBUG_BREAK();
        return Result("SessionOutgoing::decompress", reason, ErrorType::Zstd, result);
    }

    const uint8_t* datagramData = reinterpret_cast<uint8_t*>(history);
    const unsigned datagramBytes = static_cast<unsigned>(result);

    History.Commit(datagramBytes);

    decompressed.Data = datagramData;
    decompressed.Bytes = datagramBytes;

    return Result::Success();
}


} // namespace tonk
