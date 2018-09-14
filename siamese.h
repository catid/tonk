/** \file
    \brief Siamese Streaming Erasure Code Main C API Header
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

#ifndef CAT_SIAMESE_H
#define CAT_SIAMESE_H

/** \page Siamese Streaming Erasure Code Library

    Siamese is a simple, portable library for streaming erasure codes in C++.
    From given data it generates redundancies that can recover lost originals.

    It is a streaming erasure code designed for low or medium rate streams
    under 2000 packets per round-trip and modest loss rates like 20% or less.

    It can also generate selective acknowledgements and retransmitted data to be
    useful as the core engine of a Hybrid ARQ transport protocol.

    Key features:
    + Designed for use on mobile handset, PC, and server hardware.
    + As a streaming erasure code, lost packets can be recovered immediately
      rather than waiting for a larger block to completely arrive.
    + The packet data can be of variable length up to half a gigabyte.
    + The codec makes no assumptions about received data being processed
      in-order by the application, but it can be used to efficiently implement
      both TCP-style in-order or first-in unordered consumption of data.
    + Provides a jitter buffer for incoming sequenced data.
    + This convenient C API is provided, making it easy to wrap for different
      languages, or to integrate as a plugin for an engine.
    + Handles round-trip time estimation and retransmit timeouts.
    + Only the siamese_encode() and siamese_decode() functions are expensive.

    Limitations:
    + The API is not thread-safe so a lock should be held while calling.
    + Recovery failure happens about 0.3% of the time and may require one more
      recovery packet in that case.
    + Decoding gets O(N^2) more expensive as the loss rate increases, which
      means it is suitable for modest loss rates and data sizes[1].

      [1] Note that for larger data, the Wirehair codec may be a better choice
          since it can handle much larger data without getting slower:
          https://github.com/catid/wh256
          For a window of 1000 packets in flight and 10% loss rate, the overall
          latency for data is about the same between codecs.  Wirehair becomes
          preferrable as the window size and loss rate grow.

    Performance features:
    + Novel generator matrix structure with fast matrix-vector multiplication,
      switching at low data rates to parity/Cauchy rows.
    + Vectorized arithmetic tuned for ARM NEON, Intel SSSE3, AVX2.
    + Custom memory allocator to store packet data in flight on both sides,
      speeding _add() functions by 5x; in-place buffer reuse where possible.
    + Minimized copies across API boundary (footers instead of headers, etc).
    + Matrix solver tuned for typical dense lower-triangular matrices.

    Code design principles:
    + Namespacing and simple C API (versioning + error codes) to enable reuse.
    + Assertions wherever possible to catch bugs closer to the root cause.
    + Optional software features enabled via #ifdef switches to bisect issues.
    + Modular design allowing graduated unit tests to verify code components.
    + Graceful handling of memory allocation failures.
    + Whitespace-clean and commented code for readability.
*/

/// Library header version
#define SIAMESE_VERSION 5

// Tweak if the functions are exported or statically linked
//#define SIAMESE_DLL /* Defined when building/linking as DLL */
//#define SIAMESE_BUILDING /* Defined by the library makefile */

#if defined(SIAMESE_BUILDING)
# if defined(SIAMESE_DLL)
    #define SIAMESE_EXPORT __declspec(dllexport)
# else
    #define SIAMESE_EXPORT
# endif
#else
# if defined(SIAMESE_DLL)
    #define SIAMESE_EXPORT __declspec(dllimport)
# else
    #define SIAMESE_EXPORT extern
# endif
#endif

#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif


//------------------------------------------------------------------------------
// Initialization API

/**
    Perform static initialization for the library, verifying that the platform
    is supported.
    
    Returns 0 on success and other values on failure.
*/
SIAMESE_EXPORT int siamese_init_(int version);
#define siamese_init() siamese_init_(SIAMESE_VERSION)


//------------------------------------------------------------------------------
// Shared Constants/Datatypes

/// Result type
typedef enum SiameseResultT
{
    /// Success code
    Siamese_Success           = 0,

    /// Common errors:
    Siamese_InvalidInput      = 1, ///< A function parameter was invalid
    Siamese_NeedMoreData      = 2, ///< More data is needed for this operation to succeed
    Siamese_MaxPacketsReached = 3, ///< Too many packets added
    Siamese_DuplicateData     = 4, ///< Duplicate data received

    /// Codec instance was disabled because it entered an invalid state by
    /// running out of memory, receiving invalid input, or a software bug.
    /// All further API calls will return this error code to avoid exploitation.
    Siamese_Disabled          = 5,

    SiameseResult_Count,                ///< For asserts
    SiameseResult_Padding = 0x7fffffff  ///< int32_t type
} SiameseResult;

/// Range of the recovery packet numbers 0..255
#define SIAMESE_RECOVERY_NUM_MIN         0
#define SIAMESE_RECOVERY_NUM_MAX       255
#define SIAMESE_RECOVERY_NUM_COUNT     256

/// Maximum number of packets in the buffer at a time.
/// Note that practically only about 2000 makes sense
#define SIAMESE_MAX_PACKETS          16000

/// Range of the original packet numbers assigned by the codec.
/// Note that the first packet is always numbered 0
#define SIAMESE_PACKET_NUM_MIN           0
#define SIAMESE_PACKET_NUM_MAX    0x3fffff
#define SIAMESE_PACKET_NUM_COUNT  0x400000
#define SIAMESE_PACKET_NUM_BITS         22

/// Method to increment a packet number
#define SIAMESE_PACKET_NUM_INC(x)  ( (x + 1) & (SIAMESE_PACKET_NUM_COUNT - 1) )

/// Maximum number of bytes per packet, up to 536 million bytes
#define SIAMESE_MIN_PACKET_BYTES         1
#define SIAMESE_MAX_PACKET_BYTES 536870911 /* 0x1fffffff */

/// Maximum number of bytes that may be added to packet size for siamese_encode
/// Note that the actual overhead is closer to 6 bytes.
#define SIAMESE_MAX_ENCODE_OVERHEAD     8

/// Minimum number of bytes in an acknowledgement buffer
#define SIAMESE_ACK_MIN_BYTES          16

/// Original data packet
struct SiameseOriginalPacket
{
    unsigned PacketNum;        ///< Packet number for this packet
    unsigned DataBytes;        ///< Length of data in bytes
    const unsigned char* Data; ///< Original packet data
};

/// Recovery data packet
struct SiameseRecoveryPacket
{
    unsigned DataBytes;        ///< Length of data in bytes
    const unsigned char* Data; ///< Recovery packet data
};


//------------------------------------------------------------------------------
// Encoder API

/// Encoder object type
typedef struct SiameseEncoderImpl { int impl; }* SiameseEncoder;

/**
    Create a Siamese encoder.

    Returns 0 on failure.
*/
SIAMESE_EXPORT SiameseEncoder siamese_encoder_create();

/// Free memory for encoder
SIAMESE_EXPORT void siamese_encoder_free(
    SiameseEncoder encoder ///< [in] Encoder to free
);

/**
    This function checks if the encoder is ready to accept more data.

    Sometimes it is better to know this ahead of trying to add data.
    For example if the reliable data is compressed first, then it would be
    better to avoid compressing data that cannot be sent.

    Returns 0 if the encoder can accept more data.
    Returns other values if the encoder CANNOT accept more data.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_is_ready(
    SiameseEncoder encoder ///< [in] Encoder to check
);

/**
    Add a packet of data to the end of the protected set.

    packet->PacketNum will be set to the next packet number.

    SiameseSerializers.h includes a serializer for Packet Number
    that can be used to reduce the original data packet overhead.

    To properly calculate RTT, siamese_encoder_add() must be called
    within about a millisecond of when the packet is sent.

    Returns 0 on success and other codes on error.
    Returns Siamese_MaxPacketsReached if SIAMESE_MAX_PACKETS are added.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_add(
    SiameseEncoder encoder,          ///< [in] Encoder to add to
    SiameseOriginalPacket* packet    ///< [in, out] Packet to add
);

/**
    Get a packet that was submitted to the codec.

    This is useful for cases where the loss rate exceeds the capabilities of
    this code and the data must be retransmitted some other way.
    The request will fail of the packet was removed via siamese_encoder_remove().

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_get(
    SiameseEncoder encoder,         ///< [in] Encoder to get from
    SiameseOriginalPacket* packet   ///< [out] Result Packet object
);

/**
    Remove all data up to but not including the specified packet from the set.

    The decoder can provide which packets it has received through a backchannel
    and when all packets up to a certain packet have been received, the packet
    number for that packet can be provided to this API.

    It's preferred to use the siamese_encoder_ack() method instead.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_remove_before(
    SiameseEncoder encoder,     ///< [in] Encoder to use
    unsigned firstKeptPacketNum ///< [in] First packet number to keep
);

/**
    Read an acknowledgement from the decoder.

    Consumes the data generated by siamese_decoder_ack().

    It is preferred for the application to check if Acknowledgement messages
    are being received out of order, and to reject them if they are re-ordered.

    On success 'nextExpectedPacketNum' will be set to the next packet number
    that is expected by the remote peer.

    To properly calculate RTT, siamese_encoder_ack() must be called
    within about a millisecond of when the Ack packet is received.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_ack(
    SiameseEncoder encoder,         ///< [in] Encoder to use
    const void* buffer,             ///< [in] Acknowledgement message buffer
    unsigned bytes,                 ///< [in] Bytes in message buffer
    unsigned* nextExpectedPacketNum ///< [out] Next packet number expected
);

/**
    Returns a packet that should be retransmitted.

    After receiving an acknowledgement, the encoder may decide it is better to
    retransmit original data rather than encoding a recovery packet.  In this
    case this function will succeed and the original data will be returned.

    The Siamese encoder internally handles estimating the round-trip time (RTT)
    and selecting an appropriate Retransmission Timeout (RTO) for data that has
    not been acknowledged yet.

    The returned packet data pointer is valid until one of the following
    functions are called:
        - siamese_encoder_remove_before()
        - siamese_encoder_ack()
        - siamese_encoder_free()

    Returns 0 on success.
    Returns Siamese_NeedMoreData if there is no data to retransmit.
    Returns other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_retransmit(
    SiameseEncoder encoder,         ///< [in] Encoder to use
    SiameseOriginalPacket* original ///< [out] Original Packet object to retransmit
);

/**
    Encode a recovery packet.

    The recovery packet data pointer is written into the 'recovery' structure.
    The returned data pointer is valid until the next call to siamese_encode().

    The number of bytes used may exceed the largest packet size added so far
    by up to SIAMESE_MAX_ENCODE_OVERHEAD bytes, though in practice it is
    closer to 4 bytes because the internal headers are all aggressively
    compressed to reduce overhead.

    -> This is the only function in the encoder API that has CPU overhead.

    Returns 0 on success.
    Returns Siamese_NeedMoreData if there is no data to encode.
    Returns other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_encode(
    SiameseEncoder encoder,         ///< [in] Encoder to use
    SiameseRecoveryPacket* recovery ///< [out] Recovery Packet generated
);


//------------------------------------------------------------------------------
// Decoder API

/// Decoder object type
typedef struct SiameseDecoderImpl { int impl; }* SiameseDecoder;

/**
    Create a Siamese decoder.

    Returns 0 on failure.
*/
SIAMESE_EXPORT SiameseDecoder siamese_decoder_create();

/// Free memory for decoder
SIAMESE_EXPORT void siamese_decoder_free(
    SiameseDecoder decoder  ///< [in] Decoder to free
);

/**
    Pass original data to the decoder.

    This function may return Siamese_DuplicateData to indicate that the data has
    already been processed through FEC recovery and that the data arrived late.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_decoder_add_original(
    SiameseDecoder decoder,             ///< [in] Decoder to use
    const SiameseOriginalPacket* packet ///< [in] Original Packet to add
);

/**
    Pass recovery data to the decoder from the encoder.

    FIXME: Siamese cannot be used for ARQ only.  Some FEC is required also.
    Note that calling this periodically is currently required for the library to
    work properly.  So Siamese cannot currently be used only for ARQ without FEC.
    Decoder cannot flush old data from memory without feedback from the encoder.

    Potential solutions: (1) Add ack-acks to Siamese (2) Detect high data
    rate and auto-disable the FEC code (3) Add option to turn off FEC code.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_decoder_add_recovery(
    SiameseDecoder decoder,             ///< [in] Decoder to use
    const SiameseRecoveryPacket* packet ///< [in] Recovery Packet to add
);

/**
    Get a packet that was submitted to the codec or recovered.

    This function is used after recovery succeeds or to aid processing data
    in sequence order rather than out of order.

    The returned packet data pointer is valid until one of the following
    functions are called:
        - siamese_decoder_add_recovery()
        - siamese_decode()
        - siamese_decoder_free()

    Returns Siamese_NeedMoreData if the packet was not found.
    Returns 0 if the packet was found: Returns a pointer to the packet data.
    Returns other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_decoder_get(
    SiameseDecoder decoder,         ///< [in] Decoder to use
    SiameseOriginalPacket* packet   ///< [out] Original packet retrieved
);

/**
    Checks if enough data has arrived to perform decoding.

    Returns 0 if siamese_decode() should be called.
*/
SIAMESE_EXPORT SiameseResult siamese_decoder_is_ready(
    SiameseDecoder decoder  ///< [in] Decoder to use
);

/**
    Attempt to decode packets based on received data.

    packetsPtrOut: Returned pointer to recovered packets
    countOut: Return packet count

    The packetsPtrOut[] array will be in increasing packet number order.

    Returned packet data is guaranteed to be the first time it is seen,
    provided that the application respects the Siamese_DuplicateData flag
    that is returned from siamese_decoder_add_original(), or by calling
    the siamese_decoder_get() to check if the packet has been received.

    Data that is recovered will be put into the set, so that calling the
    siamese_decoder_get() function will return the recovered data.

    Calling siamese_decoder_is_ready() again will indicate if decoding
    should be attempted again.  It may be possible to decode twice.

    -> This is the only function in the decoder API that has CPU overhead.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_decode(
    SiameseDecoder decoder,                 ///< [in] Decoder to use
    SiameseOriginalPacket** packetsPtrOut,  ///< [out] Original packets recovered
    unsigned* countOut                      ///< [out] Number of packets recovered
);

/**
    This writes an acknowledgement message to the provided buffer, which
    includes the next expected packet number and a list of negative
    acknowledgement ranges (NACK list).

    buffer:      Buffer to fill
    byteLimit:   Buffer size in bytes, which must be >= SIAMESE_ACK_MIN_BYTES
    usedBytes:   Returns the number of bytes used in the buffer

    If the buffer is not big enough to represent all missing data, then the
    list will be truncated.

    Returns Siamese_NeedMoreData if there is no data received so far.
    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_decoder_ack(
    SiameseDecoder decoder, ///< [in] Decoder to use
    void* buffer,           ///< [out] Acknowledgement buffer
    unsigned byteLimit,     ///< [in] Maximum bytes to fill
    unsigned* usedBytes     ///< [out] Number of bytes used in buffer
);


//------------------------------------------------------------------------------
// Statistics API

/// Encoder statistics
typedef enum SiameseEncoderStatsT
{
    // Number of original packets added
    SiameseEncoderStats_OriginalCount,

    // Number of original bytes
    SiameseEncoderStats_OriginalBytes,

    // Number of recovery packets encoded
    SiameseEncoderStats_RecoveryCount,

    // Number of recovery bytes encoded
    SiameseEncoderStats_RecoveryBytes,

    // Number of retransmitted packets
    SiameseEncoderStats_RetransmitCount,

    // Number of retransmitted bytes
    SiameseEncoderStats_RetransmitBytes,

    // Number of acknowledgements received
    SiameseEncoderStats_AckCount,

    // Number of acknowledgement bytes
    SiameseEncoderStats_AckBytes,

    // Return number of bytes of memory used by the codec
    SiameseEncoderStats_MemoryUsed,

    SiameseEncoderStats_Count
} SiameseEncoderStats;

/**
    Return an array of statistics collected by the encoder for all time.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_encoder_stats(
    SiameseEncoder encoder, ///< [in] Encoder to use
    uint64_t* statsOut,     ///< [out] Returned stats
    unsigned statsCount     ///< [in] Number of stats to return
);

/// Decoder statistics
typedef enum SiameseDecoderStatsT
{
    // Number of original packets received
    SiameseDecoderStats_OriginalCount,

    // Number of original bytes received
    SiameseDecoderStats_OriginalBytes,

    // Number of recovery packets received
    SiameseDecoderStats_RecoveryCount,

    // Number of recovery bytes received
    SiameseDecoderStats_RecoveryBytes,

    // Number of acknowledgements generated
    SiameseDecoderStats_AckCount,

    // Number of acknowledgement bytes
    SiameseDecoderStats_AckBytes,

    // Count of original packets we ignored because they were already recovered
    SiameseDecoderStats_DupedOriginalCount,

    // Number of successfully recovered original data packets from solution
    SiameseDecoderStats_SolveSuccessCount,

    // Count of the number of failed solution attempts
    SiameseDecoderStats_SolveFailCount,

    // Count of the number of ignored recovery packets since original data
    // arrived successfully
    SiameseDecoderStats_DupedRecoveryCount,
    // Note: Total packet overhead is SolveFailCount + IgnoredRecoveryCount

    // Return number of bytes of memory used by the codec
    SiameseDecoderStats_MemoryUsed,

    SiameseDecoderStats_Count
} SiameseDecoderStats;

/**
    Return an array of statistics collected by the decoder for all time.

    Returns 0 on success and other codes on error.
*/
SIAMESE_EXPORT SiameseResult siamese_decoder_stats(
    SiameseDecoder decoder, ///< [in] Decoder to use
    uint64_t* statsOut,     ///< [out] Stats to return
    unsigned statsCount     ///< [in] Number of stats to return
);


#ifdef __cplusplus
}
#endif


#endif // CAT_SIAMESE_H
