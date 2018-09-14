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

#pragma once

#include <cstdint>

namespace security {


//------------------------------------------------------------------------------
// Constants

/// Encryption can be disabled if desired in order to inspect the protocol
/// All apps will need to be rebuilt
#define SECURITY_ENABLE_ENCRYPTION

/// Datagram checksums can be disabled to allow for intentional manipulation
/// All apps will need to be rebuilt
#define SECURITY_ENABLE_CHECKSUMS


//------------------------------------------------------------------------------
// SimpleCipher

/**
    SimpleCipher is a symmetric AEAD scheme based on XOR and the t1ha fast hash.

    Goals:
    + Allow for it to be disabled by passing key = 0 to trace packets if needed.
    + Obfuscate data and frustrate reverse-engineering.
    + Should look like real encryption (no obvious patterns in encrypted data).
    + Prevent attempts to modify packet data.
    + Prevent attempts to inject packets.
    + Prevent attempts to replay packets; provided by StrikeRegister.
    + Run very fast - Should not show up on a profiler.

    Non-goals:
    + The cipher does not provide data security.

    Encryption:

    To encrypt or decrypt a datagram, a 16 byte value Adder is constructed based
    on the datagram sequence.  A 16 byte Key is initialized to the expanded key.
    For each 16 bytes, Adder is added into the Key, and then the sum is XOR'd
    into the datagram message payload.  This ensures that there are no patterns
    visible in the data even if the plaintext is all zeros.

    Since the encryption will change for each datagram, it will not be possible
    to see if two datagrams contain the same data by passively observing, which
    denies a game cheater the option of selectively dropping duplicate packets.

    Tagging:

    Each datagram is tagged with a 16-bit hash of its data.  This tagging allows
    the receiver to authenticate that the datagram was sent by its peer.
    Authentication is done by recalculating the hash and verifying it matches.

    The authors surveyed hash algorithms and selected t1ha() as being especially
    fast, satisfying the speed goal.  Compared to cryptographic hash functions:
    + t1ha() is 25-60x faster than HighwayHash.
    + HighwayHash is 2x faster than SipHash.

    The hash includes the connection key and the datagram sequence, so that two
    identical datagrams from the same connection will have a minimal chance of
    being tagged with the same hash.  Furthermore, two identical datagrams
    from different connections with the same sequence will also have different
    looking hashes.  As a result, it is not possible to replay datagrams from
    the current or previous connections.

    Discussion:

    The encryption mainly serves to hide the message content from tools that
    can observe datagram contents, inject, and modify, but have no knowledge
    of the custom protocol.  This makes it much harder to attack the netcode,
    as the attacker would have to reverse-engineer the software.

    Tag validation can serve as another guard against accepting packets
    accidentally from previous or parallel connections that originate from
    the same host address.
*/

class SimpleCipher
{
public:
    /// Set up the cipher with an encryption key
    void Initialize(uint64_t encryptionKey);

    /// Encrypt/decrypt (same operation for both)
    void Cipher(uint8_t* data, unsigned bytes, uint64_t sequence);

    /// Generate 16-bit tag to attach to packets
    uint16_t Tag(const uint8_t* data, unsigned bytes, uint64_t sequence);

    /// Generate a 16-bit tag for an integer
    uint16_t TagInt(uint32_t data);

protected:
    /// Connection key expanded into four 32-bit keys, which are used
    /// by the Cipher() and Tag() functions
    uint32_t ExpandedKey[4];
};


} // namespace security
