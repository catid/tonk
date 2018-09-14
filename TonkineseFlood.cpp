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

#include "TonkineseFlood.h"
#include "thirdparty/t1ha.h"

namespace tonk {

static logger::Channel ModuleLogger("Flood", MinimumLogLevel);


//------------------------------------------------------------------------------
// FloodDetector

Result FloodDetector::Initialize(unsigned maxPerAddr, unsigned minuteFloodThresh)
{
    MaximumConnectionsPerAddress = maxPerAddr;
    TONK_DEBUG_ASSERT(MaximumConnectionsPerAddress >= 1);

    FloodConnectionsPerMinute = minuteFloodThresh;
    TONK_DEBUG_ASSERT(FloodConnectionsPerMinute >= 1);

    // Seed the key generator
    uint64_t master_keys[2] = {};
    Result result = SecureRandom_Next((uint8_t*)master_keys, sizeof(master_keys));
    if (result.IsFail()) {
        return result;
    }
    KeyGen.Seed(master_keys[0], master_keys[1]);

    // Generate initial key material
    for (unsigned i = 0; i < kKeyCount; ++i) {
        for (unsigned j = 0; j < kKeyWords; ++j) {
            Keys[i][j] = KeyGen.Next();
        }
    }

    return Result::Success();
}

FloodReaction FloodDetector::OnConnectionRequest(
    const UDPAddress& source,
    uint64_t key,
    uint64_t& cookieOut)
{
    TONK_DEBUG_ASSERT(MaximumConnectionsPerAddress != 0); // Init not called

    // Zero out the port
    const UDPAddress ipaddr(source.address(), 0);
    unsigned ipConnectionCount = 0;

    // Find IP in count map
    const int mapIndex = CountMap.find_index(ipaddr);

    // If found:
    if (mapIndex >= 0)
    {
        ipConnectionCount = CountMap.get_node(mapIndex)->second;
        TONK_DEBUG_ASSERT(ipConnectionCount > 0);
        TONK_DEBUG_ASSERT(ipConnectionCount <= MaximumConnectionsPerAddress);

        // If IP connection count exceeds limit already:
        if (ipConnectionCount >= MaximumConnectionsPerAddress)
        {
            ModuleLogger.Debug("Connection flood from ", source.address().to_string(),
                ":", source.port(), " detected(", ipConnectionCount, "): Denied");

            // Too many connections from this address
            return FloodReaction::Deny;
        }
    }

    // Update and check overall connection rate:

    const uint64_t nowUsec = siamese::GetTimeUsec();
    const uint64_t elapsedUsec = nowUsec - BinStartUsec;

    // If newer than both bins:
    if (elapsedUsec >= kBinCount * kBinDurationUsec)
    {
        // Make second bin start at this time
        BinStartUsec = nowUsec - kBinDurationUsec;

        static_assert(kBinCount == 2, "Update this also");

        ConnectionCountA = ConnectionCountB;
        ConnectionCountB = 0;
    }

    ++ConnectionCountB;

    // If connection rate does not exceed warning threshold:
    const bool rateNotExceeded =
        (ConnectionCountA <= FloodConnectionsPerMinute) &&
        (ConnectionCountB <= FloodConnectionsPerMinute);
    if (rateNotExceeded)
    {
        ModuleLogger.Debug("Allowing connection from ", source.address().to_string(),
            ":", source.port(), " during low connection rate: [",
            ConnectionCountA, ", ", ConnectionCountB, " < ",
            FloodConnectionsPerMinute, "]/minute");

        // If IP is not in table:
        if (mapIndex < 0) {
            // Insert it
            CountMap.insert(ipaddr, 1);
        }
        else {
            // Increment the IP connection count
            CountMap.get_node(mapIndex)->second++;
        }

        // If we return Allow, we expect the application to call OnDisconnect
        return FloodReaction::Allow;
    }

    /**
        SYN Cookie Alogrithm

        The HashUDPAddress() function already produces hashes that are
        unique for each run.  However it does not adequately protect the
        input data from leaking, and it does not change over time.

        I'm not being rigorous about it but t1ha() seems like it provides
        enough mixing to be difficult to practically recover the key.

        The hash key needs to change over time to avoid stale cookies that
        could be replayed.  Assuming that a 3-way connection handshake
        completes within 10 seconds, we can pick a new key every 5 seconds.
        We should compare and accept SYN cookies generated by both the old
        key and the new key.

        To avoid having to guess twice, the low bit is set to 0 or 1 to
        indicate which key generated it.
    */

    // If it is time to rotate the keys:
    if (nowUsec - LastKeyUpdateUsec >= kKeyRotationIntervalUsec)
    {
        LastKeyUpdateUsec = nowUsec;

        ModuleLogger.Debug("Rotating connection flood cookie keys");

        // Pick which key to update next
        LastUpdatedKeyIndex ^= 1;

        // Replace the key with a new one
        for (unsigned j = 0; j < kKeyWords; ++j) {
            Keys[LastUpdatedKeyIndex][j] = KeyGen.Next();
        }
    }

    // Get which key the initiator was given by the flag bit
    const unsigned key_index = key & 1;
    const uint8_t* selected_key = (uint8_t*)&Keys[key_index];

    // Hash the initiator's source address (involves a secret key)
    const uint64_t source_hash = HashUDPAddress(source);

    // Compute the expected cookie value
    uint64_t expected_cookie = t1ha_local(
        selected_key,   // One or two keys as indicated by initiator
        kKeyBytes,      // Number of bytes in the key
        source_hash);   // Run-unique hash of source address

    // If cookie bits are the same:
    const bool same = (0 == ((expected_cookie ^ key) >> 1));
    if (same)
    {
        ModuleLogger.Debug("Connection flood cookie from ", source.address().to_string(),
            ":", source.port(), " for key ", key_index, " matched - Allowed");

        // If IP is not in table:
        if (mapIndex < 0) {
            // Insert it
            CountMap.insert(ipaddr, 1);
        }
        else {
            // Increment the IP connection count
            CountMap.get_node(mapIndex)->second++;
        }

        // If we return Allow, we expect the application to call OnDisconnect
        return FloodReaction::Allow;
    }

    // Otherwise: Initiator needs to be provided with a cookie

    TONK_DEBUG_ASSERT(LastUpdatedKeyIndex < 2);

    // If the key index we should use is not the one we did:
    if (key_index != LastUpdatedKeyIndex)
    {
        ModuleLogger.Debug("Guessed the wrong key index - Recalculating");

        const uint8_t* correct_key = (uint8_t*)&Keys[LastUpdatedKeyIndex];

        // Need to recompute the cookie with the correct key
        expected_cookie = t1ha_local(
            correct_key,    // One or two keys as indicated by initiator
            kKeyBytes,      // Number of bytes in the key
            source_hash);   // Run-unique hash of source address
    }

    ModuleLogger.Debug("Connection from ", source.address().to_string(), ":", source.port(),
        " did not match expected cookie.  Generating cookie from key ", LastUpdatedKeyIndex);

    // Attach key index to cookie and return it
    cookieOut = (expected_cookie & ~(uint64_t)1) | LastUpdatedKeyIndex;

    return FloodReaction::Cookie;
}

void FloodDetector::OnDisconnect(const UDPAddress& source)
{
    TONK_DEBUG_ASSERT(!source.address().is_unspecified());

    // IP address without port
    const UDPAddress ipaddr(source.address(), 0);

    // Find this IP address
    const int mapIndex = CountMap.find_index(ipaddr);

    // If IP address in not in the map:
    if (mapIndex < 0) {
        TONK_DEBUG_BREAK(); // Should never happen
        return;
    }

    unsigned ipConnectionCount = CountMap.get_node(mapIndex)->second;
    TONK_DEBUG_ASSERT(ipConnectionCount > 0);
    TONK_DEBUG_ASSERT(ipConnectionCount <= MaximumConnectionsPerAddress);

    // If this is the last connection from this IP:
    if (ipConnectionCount <= 1)
    {
        ModuleLogger.Debug("Erased final IP map entry for ", source.address().to_string());

        CountMap.erase_index(mapIndex);
    }
    else
    {
        --ipConnectionCount;

        ModuleLogger.Debug("Decrementing IP map entry for ",
            source.address().to_string(), ". Now: ", ipConnectionCount);

        CountMap.get_node(mapIndex)->second = ipConnectionCount;
    }
}


} // namespace tonk
