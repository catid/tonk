/** \file
    \brief Tonk: Flood Protection
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
#include "TonkineseProtocol.h"
#include "TonkineseMaps.h"

namespace tonk {


//------------------------------------------------------------------------------
// FloodDetector

enum class FloodReaction
{
    /// Allow connection immediately
    Allow,

    /// Allow - but require a cookie exchange first to mitigate flood attacks
    Cookie,

    /// Deny connection
    Deny
};

/// This class keeps track of connections from each IP and connection rate to
/// trigger mitigations to avoid trivial attacks against Tonk-based services
class FloodDetector
{
public:
    /// Initialize detector with maximum connections allowed per IP address
    /// and the maximum connection per minute before detecting a flood
    Result Initialize(unsigned maxPerAddr, unsigned minuteFloodThresh);

    /// Given the source address, this function records a request from the given
    /// address and decides whether the connection should be allowed or denied
    FloodReaction OnConnectionRequest(
        const UDPAddress& source,
        uint64_t key,
        uint64_t& cookieOut);

    // Note: I do not think that we need to react to address changes.
    // Filtering floods from the initial connection address is sufficient
    // and less likely to have bugs

    /// If OnConnectionRequest() returned Allow, then OnDisconnect() must be
    /// called to decrement the count of connections from that address.
    /// The same address must be passed to both functions
    void OnDisconnect(const UDPAddress& source);

protected:
    /// Configured maximum number of connections per address
    unsigned MaximumConnectionsPerAddress = 0;

    /// Configured connections per minute triggering flood warning
    unsigned FloodConnectionsPerMinute = 0;

    using MapT = LightHashMap<UDPAddress, uint32_t, UDPAddressHasher>;

    /// Number of connections for each address.
    // Note: This only keeps track of IP address.
    // It uses UDPAddress with the port number set to 0
    MapT CountMap;

    /// Number of bins
    static const unsigned kBinCount = 2;

    /// Number of microseconds per bin.  Set to one minute to match units
    static const unsigned kBinDurationUsec = 60 * 1000 * 1000;

    /// Number of cookie keys
    static const unsigned kKeyCount = 2;

    /// Count of bytes in a key
    static const unsigned kKeyBytes = 16;

    /// Number of words in the keys
    static const unsigned kKeyWords = kKeyBytes / sizeof(uint32_t);

    /// Rotate keys every 5 seconds
    static const unsigned kKeyRotationIntervalUsec = 5 * 1000 * 1000;

    /// Count of connections in earlier bin
    unsigned ConnectionCountA = 0;

    /// Count of connections in later bin
    unsigned ConnectionCountB = 0;

    /// Start of earlier bin
    uint64_t BinStartUsec = 0;

    /// Set of hash keys that get rotated periodically
    std::array<uint32_t, kKeyWords> Keys[kKeyCount];

    /// Generator for the keys
    siamese::PCGRandom KeyGen;

    /// Last time a key was updated
    uint64_t LastKeyUpdateUsec = 0;

    /// Last key that was updated by key rotation
    unsigned LastUpdatedKeyIndex = 0;
};


} // namespace tonk
