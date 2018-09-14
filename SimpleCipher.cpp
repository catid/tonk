/** \file
    \brief Simple Cipher
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of SimpleCipher nor the names of its contributors may be
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

#include "SimpleCipher.h"
#include "thirdparty/t1ha.h"

// Enable SIMD and other desktop optimizations
#if (defined (_MSC_VER) && _MSC_VER >= 1900)
    #define ENABLE_SIMD_ENCRYPTION
    #include <immintrin.h>

    // Compiler-specific 128-bit SIMD register keyword
    #define SECURITY_M128 __m128i

    #define ENABLE_UNALIGNED_ACCESSES
#endif // Desktop

// Compiler-specific force inline keyword
#ifdef _MSC_VER
    #define SECURITY_FORCE_INLINE inline __forceinline
#else
    #define SECURITY_FORCE_INLINE inline __attribute__((always_inline))
#endif

// Specify an intentionally unused variable (often a function parameter)
#define SECURITY_UNUSED(x) (void)(x);

namespace security {


//------------------------------------------------------------------------------
// Serializers

SECURITY_FORCE_INLINE uint16_t ReadU16_LE(const uint8_t* data)
{
#if !defined(ENABLE_UNALIGNED_ACCESSES)
    return ((uint16_t)data[1] << 8) | data[0];
#else
    return *(uint16_t*)data;
#endif
}

SECURITY_FORCE_INLINE void WriteU16_LE(uint8_t* data, uint16_t value)
{
#if !defined(ENABLE_UNALIGNED_ACCESSES)
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint16_t*)data = value;
#endif
}

static SECURITY_FORCE_INLINE uint32_t ReadU32_LE(const uint8_t* data)
{
#if !defined(ENABLE_UNALIGNED_ACCESSES)
    return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
#else
    return *(uint32_t*)data;
#endif
}

static SECURITY_FORCE_INLINE void WriteU32_LE(uint8_t* data, uint32_t value)
{
#if !defined(ENABLE_UNALIGNED_ACCESSES)
    data[3] = (uint8_t)(value >> 24);
    data[2] = (uint8_t)(value >> 16);
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint32_t*)data = value;
#endif
}

static SECURITY_FORCE_INLINE uint64_t ReadU64_LE(const uint8_t* data)
{
#if !defined(ENABLE_UNALIGNED_ACCESSES)
    return ((uint64_t)data[7] << 56) | ((uint64_t)data[6] << 48) | ((uint64_t)data[5] << 40) |
        ((uint64_t)data[4] << 32) | ((uint64_t)data[3] << 24) | ((uint64_t)data[2] << 16) |
        ((uint64_t)data[1] << 8) | data[0];
#else
    return *(uint64_t*)data;
#endif
}

static SECURITY_FORCE_INLINE void WriteU64_LE(uint8_t* data, uint64_t value)
{
#if !defined(ENABLE_UNALIGNED_ACCESSES)
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

/// Read 0-4 bytes into the low bits of a 32-bit integer
/// Preconditions: bytes <= 4
static SECURITY_FORCE_INLINE uint32_t TailReadLE32(const void* data, unsigned bytes)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t r = 0;
    switch (bytes)
    {
    case 4: return ReadU32_LE(p);
    case 3: r = (uint32_t)p[2] << 16; // Fall-thru
    case 2: return r + ReadU16_LE(p);
    case 1: return p[0];
    default: return 0;
    }
}

/// Read 0-8 bytes into the low bits of a 64-bit integer
/// Preconditions: bytes <= 8
static SECURITY_FORCE_INLINE uint64_t TailReadLE64(const void* data, unsigned bytes)
{
    if (bytes <= 4) {
        return TailReadLE32(data, bytes);
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint64_t r = 0;

    switch (bytes)
    {
    default:
    case 8: return ReadU64_LE(p);
    case 7: r = (uint64_t)p[6] << 8; // Fall-thru
    case 6:
        r += p[5];
        r <<= 8; // Fall-thru
    case 5:
        r += p[4];
        r <<= 32;
        return r + ReadU32_LE(p);
    }
}

/// Write 0-4 bytes from the low bits of a given 32-bit integer
/// Preconditions: bytes <= 4
static SECURITY_FORCE_INLINE void TailWriteLE32(uint32_t value, void* data, unsigned bytes)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(data);
    switch (bytes)
    {
    case 4: return WriteU32_LE(p, value);
    case 3: p[2] = (uint8_t)(value >> 16); // Fall-thru
    case 2: return WriteU16_LE(p, (uint16_t)value);
    case 1: p[0] = (uint8_t)value; // Fall-thru
    default: break;
    }
}

/// Write 0-8 bytes from the low bits of a given 64-bit integer
/// Preconditions: bytes <= 8
inline void TailWriteLE64(uint64_t value, void* data, unsigned bytes)
{
    if (bytes <= 4) {
        return TailWriteLE32((uint32_t)value, data, bytes);
    }

    uint8_t* p = reinterpret_cast<uint8_t*>(data);
    switch (bytes)
    {
    case 8: return WriteU64_LE(p, value);
    case 7: p[6] = (uint8_t)(value >> 48); // Fall-thru
    case 6: p[5] = (uint8_t)(value >> 40); // Fall-thru
    case 5: p[4] = (uint8_t)(value >> 32); // Fall-thru
    default: return WriteU32_LE(p, (uint32_t)value);
    }
}


//------------------------------------------------------------------------------
// SimpleCipher

static inline uint32_t MultiplyAndFold19(uint32_t a, uint32_t b)
{
    const uint64_t p = static_cast<uint64_t>(a) * b;
    return static_cast<uint32_t>(p) ^ static_cast<uint32_t>(p >> 19);
}

void SimpleCipher::Initialize(uint64_t encryptionKey)
{
    // Expand connection key to four 32-bit words:

    static const uint32_t q0 = 0xe7988061;
    static const uint32_t q1 = 0xbd9fa5c7;

    ExpandedKey[0] = (uint32_t)encryptionKey;
    ExpandedKey[1] = (uint32_t)(encryptionKey >> 32);
    ExpandedKey[2] = MultiplyAndFold19(q0, ExpandedKey[0]);
    ExpandedKey[3] = MultiplyAndFold19(q1, ExpandedKey[1]);
}

void SimpleCipher::Cipher(uint8_t* data, unsigned bytes, uint64_t sequence)
{
#ifndef SECURITY_ENABLE_ENCRYPTION
    SECURITY_UNUSED(data); SECURITY_UNUSED(bytes); SECURITY_UNUSED(nonce);
#else // SECURITY_ENABLE_ENCRYPTION
    // Skip encryption if the key is zero
    if (ExpandedKey[0] == 0 && ExpandedKey[1] == 0) {
        return;
    }

    /*
        Encryption and decryption are the same:

        (1) Mix session key and nonce into 16 byte datagram key
        (2) XOR 16 bytes of data at a time by the datagram key

        This admits a lot of attacks of course:  Plaintext data that is zeros
        will reveal the XOR key for the rest of that datagram.  Two 16-byte
        blocks can be XOR'd together to get the XOR of two plaintext pieces.
        Each key leak will eventually lead to session key recovery fairly fast.

        It's not secure. But it is very fast, hides the packet contents, and
        will slow down reverse-engineers who always win eventually.
    */

    static const uint32_t p0 = 0x905c4577;
    static const uint32_t p1 = 0xb84c88cb;
    static const uint32_t p2 = 0xde0efbf1;
    static const uint32_t p3 = 0x9f614c3d;
    static const uint32_t p4 = 0xf3f77d13;
    static const uint32_t x0 = 0x80771ab1;
    static const uint32_t x1 = 0x55f2cc88;

    const uint32_t n0 = (uint32_t)sequence + x0;
    const uint32_t n1 = MultiplyAndFold19((uint32_t)(sequence >> 32) + x1, p4);

    // Mix session key and nonce
    const uint32_t a[4] = {
        MultiplyAndFold19(p0, n0) + n1,
        MultiplyAndFold19(p1, n0) + n1,
        MultiplyAndFold19(p2, n0) + n1,
        MultiplyAndFold19(p3, n0) + n1
    };

    uint32_t k[4];
    for (int i = 0; i < 4; ++i) {
        k[i] = ExpandedKey[i];
    }

    // Handle sets of 16 bytes
#ifdef ENABLE_SIMD_ENCRYPTION
    if (bytes >= 16)
    {
        SECURITY_M128 key = _mm_set_epi32(k[3], k[2], k[1], k[0]);
        const SECURITY_M128 add = _mm_set_epi32(a[3], a[2], a[1], a[0]);

        while (bytes >= 16)
        {
            key = _mm_add_epi32(key, add);

            SECURITY_M128* data16 = reinterpret_cast<SECURITY_M128*>(data);
            _mm_storeu_si128(data16, _mm_xor_si128(_mm_loadu_si128(data16), key));

            data += 16, bytes -= 16;
        }

        union {
            SECURITY_M128 v;
            uint32_t a[4];
        } e;
        e.v = key;
        for (int i = 0; i < 4; ++i) {
            k[i] = e.a[i];
        }
    }
#else
    while (bytes >= 16)
    {
        for (int i = 0; i < 4; ++i)
        {
            k[i] += a[i];
            const uint32_t x = ReadU32_LE(data + i * 4) ^ k[i];
            WriteU32_LE(data + i * 4, x);
        }

        data += 16, bytes -= 16;
    }
#endif

    // XOR remaining bytes
    unsigned i = 0;
    while (bytes >= 4)
    {
        k[i & 3] += a[i & 3];

        WriteU32_LE(data, ReadU32_LE(data) ^ k[i & 3]);
        data += 4, bytes -= 4, ++i;
    }

    if (bytes > 0)
    {
        k[i & 3] += a[i & 3];

        uint32_t x = TailReadLE32(data, bytes);
        x ^= k[i & 3];
        TailWriteLE32(x, data, bytes);
    }
#endif // SECURITY_ENABLE_ENCRYPTION
}

uint16_t SimpleCipher::Tag(const uint8_t* data, unsigned bytes, uint64_t nonce)
{
#ifdef SECURITY_ENABLE_CHECKSUMS
    const uint64_t key = ((uint64_t)ExpandedKey[1] << 32) | ExpandedKey[0];
    const uint64_t tag64 = t1ha(data, bytes, nonce ^ key);
    return (uint16_t)(tag64 >> 19);
#else // SECURITY_ENABLE_CHECKSUMS
    SECURITY_UNUSED(data); SECURITY_UNUSED(bytes); SECURITY_UNUSED(nonce);
    return 0;
#endif // SECURITY_ENABLE_CHECKSUMS
}

static SECURITY_FORCE_INLINE uint64_t rot64(uint64_t v, unsigned s) {
    return (v >> s) | (v << (64 - s));
}

uint16_t SimpleCipher::TagInt(uint32_t data)
{
#ifdef SECURITY_ENABLE_CHECKSUMS
    const uint32_t key = ExpandedKey[0];
    const uint64_t prod = (uint64_t)key * (data ^ ExpandedKey[1]);
    const uint64_t tag64 = prod ^ rot64(prod, 41);
    return (uint16_t)(tag64 >> 19);
#else // SECURITY_ENABLE_CHECKSUMS
    SECURITY_UNUSED(data);
    return 0;
#endif // SECURITY_ENABLE_CHECKSUMS
}



} // namespace security
