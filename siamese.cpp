/** \file
    \brief Siamese Streaming Erasure Code Main C API Source
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

#include "siamese.h"

#include "SiameseEncoder.h"
#include "SiameseDecoder.h"

extern "C" {


//------------------------------------------------------------------------------
// Initialization API

static bool m_Initialized = false;

SIAMESE_EXPORT int siamese_init_(int version)
{
    if (version != SIAMESE_VERSION)
        return Siamese_Disabled;

    if (0 != gf256_init())
        return Siamese_Disabled;

    m_Initialized = true;
    return Siamese_Success;
}


//------------------------------------------------------------------------------
// Encoder API

SIAMESE_EXPORT SiameseEncoder siamese_encoder_create()
{
    SIAMESE_DEBUG_ASSERT(m_Initialized); // Must call siamese_init() first
    if (!m_Initialized)
        return nullptr;

    siamese::Encoder* encoder = new(std::nothrow) siamese::Encoder;

    return reinterpret_cast<SiameseEncoder>(encoder);
}

SIAMESE_EXPORT void siamese_encoder_free(
    SiameseEncoder encoder_t)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    delete encoder;
}

SIAMESE_EXPORT SiameseResult siamese_encoder_is_ready(
    SiameseEncoder encoder_t ///< [in] Encoder to check
)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder) {
        return Siamese_InvalidInput;
    }

    // Provide a buffer of some packets, to allow the application some slack
    // to add two packets after checking.  Otherwise it's pretty easy for
    // app developers to write code that accidentally does that in practice.
    if (encoder->GetRemainingSlots() <= 2) {
        return Siamese_MaxPacketsReached;
    }

    return Siamese_Success;
}

SIAMESE_EXPORT SiameseResult siamese_encoder_add(
    SiameseEncoder encoder_t,
    SiameseOriginalPacket* packet)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || !packet || !packet->Data ||
        packet->DataBytes <= 0 || packet->DataBytes > SIAMESE_MAX_PACKET_BYTES)
    {
        return Siamese_InvalidInput;
    }

    return encoder->Add(*packet);
}

SIAMESE_EXPORT SiameseResult siamese_encoder_get(
    SiameseEncoder encoder_t,
    SiameseOriginalPacket* packet)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || !packet || packet->PacketNum > SIAMESE_PACKET_NUM_MAX)
    {
        return Siamese_InvalidInput;
    }

    return encoder->Get(*packet);
}

SIAMESE_EXPORT SiameseResult siamese_encoder_remove_before(
    SiameseEncoder encoder_t,
    unsigned packetNum)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || packetNum > SIAMESE_PACKET_NUM_MAX)
        return Siamese_InvalidInput;

    encoder->RemoveBefore(packetNum);
    return Siamese_Success;
}

SIAMESE_EXPORT SiameseResult siamese_encoder_ack(
    SiameseEncoder encoder_t,
    const void* buffer,
    unsigned bytes,
    unsigned* nextExpectedPacketNum)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || !buffer || bytes < 1 || !nextExpectedPacketNum)
        return Siamese_InvalidInput;

    return encoder->Acknowledge((uint8_t*)buffer, bytes, *nextExpectedPacketNum);
}

SIAMESE_EXPORT SiameseResult siamese_encoder_retransmit(
    SiameseEncoder encoder_t,
    SiameseOriginalPacket* original)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || !original)
        return Siamese_InvalidInput;

    return encoder->Retransmit(*original);
}

SIAMESE_EXPORT SiameseResult siamese_encode(
    SiameseEncoder encoder_t,
    SiameseRecoveryPacket* recovery)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || !recovery)
        return Siamese_InvalidInput;

    return encoder->Encode(*recovery);
}

SIAMESE_EXPORT SiameseResult siamese_encoder_stats(
    SiameseEncoder encoder_t,
    uint64_t* statsOut,
    unsigned statsCount)
{
    siamese::Encoder* encoder = reinterpret_cast<siamese::Encoder*>(encoder_t);
    if (!encoder || !statsOut || statsCount <= 0)
        return Siamese_InvalidInput;

    return encoder->GetStatistics(statsOut, statsCount);
}


//------------------------------------------------------------------------------
// Decoder API

SIAMESE_EXPORT SiameseDecoder siamese_decoder_create()
{
    SIAMESE_DEBUG_ASSERT(m_Initialized); // Must call siamese_init() first
    if (!m_Initialized)
        return nullptr;

    siamese::Decoder* decoder = new(std::nothrow) siamese::Decoder;

    return reinterpret_cast<SiameseDecoder>(decoder);
}

SIAMESE_EXPORT void siamese_decoder_free(
    SiameseDecoder decoder_t)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (decoder == nullptr)
        return;

    delete decoder;
}

SIAMESE_EXPORT SiameseResult siamese_decoder_add_original(
    SiameseDecoder decoder_t,
    const SiameseOriginalPacket* packet)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder || !packet || packet->DataBytes <= 0 ||
        packet->DataBytes > SIAMESE_MAX_PACKET_BYTES ||
        packet->PacketNum > SIAMESE_PACKET_NUM_MAX)
    {
        return Siamese_InvalidInput;
    }

    return decoder->AddOriginal(*packet);
}

SIAMESE_EXPORT SiameseResult siamese_decoder_add_recovery(
    SiameseDecoder decoder_t,
    const SiameseRecoveryPacket* packet)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder || !packet || !packet->Data || packet->DataBytes <= 0 ||
        packet->DataBytes > SIAMESE_MAX_PACKET_BYTES /* extra check to avoid integer overflows */)
    {
        return Siamese_InvalidInput;
    }

    return decoder->AddRecovery(*packet);
}

SIAMESE_EXPORT SiameseResult siamese_decoder_get(
    SiameseDecoder decoder_t,
    SiameseOriginalPacket* packet)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder || !packet || packet->PacketNum > SIAMESE_PACKET_NUM_MAX)
        return Siamese_InvalidInput;

    return decoder->Get(*packet);
}

SIAMESE_EXPORT SiameseResult siamese_decoder_is_ready(
    SiameseDecoder decoder_t)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder)
        return Siamese_InvalidInput;

    return decoder->IsReadyToDecode();
}

SIAMESE_EXPORT SiameseResult siamese_decode(
    SiameseDecoder decoder_t,
    SiameseOriginalPacket** packetsPtrOut,
    unsigned* countOut)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder || (!packetsPtrOut != !countOut))
        return Siamese_InvalidInput;

    return decoder->Decode(
        packetsPtrOut,
        countOut);
}

SIAMESE_EXPORT SiameseResult siamese_decoder_ack(
    SiameseDecoder decoder_t,
    void* buffer,
    unsigned byteLimit,
    unsigned* usedBytes)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder || !buffer || !usedBytes || byteLimit < SIAMESE_ACK_MIN_BYTES)
        return Siamese_InvalidInput;

    return decoder->GenerateAcknowledgement(
        (uint8_t*)buffer,
        byteLimit,
        *usedBytes);
}

SIAMESE_EXPORT SiameseResult siamese_decoder_stats(
    SiameseDecoder decoder_t,
    uint64_t* statsOut,
    unsigned statsCount)
{
    siamese::Decoder* decoder = reinterpret_cast<siamese::Decoder*>(decoder_t);
    if (!decoder || !statsOut || statsCount <= 0)
        return Siamese_InvalidInput;

    return decoder->GetStatistics(
        statsOut,
        statsCount);
}


} // extern "C"
