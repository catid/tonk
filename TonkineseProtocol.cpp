/** \file
    \brief Tonk Implementation: Wire Protocol Definition
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonkinese nor the names of its contributors may be
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

#include "TonkineseProtocol.h"

namespace tonk { namespace protocol {

static logger::Channel ModuleLogger("Protocol", MinimumLogLevel);


//------------------------------------------------------------------------------
// Footer

bool DatagramIsTruncated(const uint8_t* data, size_t bytes)
{
    // If datagram is too short for any valid content:
    if (bytes < kEncryptionTagBytes + kFlagsBytes) {
        return true;
    }

    // Read flags byte
    const uint8_t* footer = data + bytes - kFlagsBytes - kEncryptionTagBytes;
    const FlagsReader flags = footer[0];

    // If the datagram is not large enough to contain the footer:
    if (flags.IsConnectionDatagram() &&
        bytes < flags.ConnectedFooterBytes())
    {
        // This is truncated
        return true;
    }

    // Datagram is large enough to contain valid data
    return false;
}


//------------------------------------------------------------------------------
// Handshakes


namespace handshake {


void WriteC2SConnectionRequest(uint8_t* data, uint64_t key)
{
    siamese::WriteU32_LE(data, kMagicC2SConnectionRequest);
    siamese::WriteU64_LE(data + 4, key);
}

void WriteS2CCookieResponse(uint8_t* data, uint64_t cookie)
{
    siamese::WriteU32_LE(data, kMagicS2CCookieResponse);
    siamese::WriteU64_LE(data + 4, cookie);
}

void WriteP2PConnectionRequest(uint8_t* data, uint64_t key)
{
    siamese::WriteU32_LE(data, kMagicP2PConnectionRequest);
    siamese::WriteU64_LE(data + 4, key);
}

void WriteDisconnect(uint8_t* data, uint64_t key)
{
    siamese::WriteU32_LE(data, HandshakeType_Disconnect);
    siamese::WriteU64_LE(data + 4, key);
}

void WriteS2CUnknownSourceAddress(uint8_t* data)
{
    siamese::WriteU32_LE(data, HandshakeType_S2CUnknownSourceAddress);
    siamese::WriteU64_LE(data + 4, 0);
}

void WriteC2SUpdateSourceAddress(uint8_t* data, uint64_t key, uint32_t id)
{
    siamese::WriteU32_LE(data, (id << 8) | HandshakeType_C2SUpdateSourceAddress);
    siamese::WriteU64_LE(data + 4, key);
}

HandshakeType GetType(const uint8_t* data)
{
    const uint32_t type = siamese::ReadU32_LE(data);
    if (type == kMagicC2SConnectionRequest) {
        return HandshakeType_C2SConnectionRequest;
    }
    if (type == kMagicS2CCookieResponse) {
        return HandshakeType_S2CCookieResponse;
    }
    if (type == kMagicP2PConnectionRequest) {
        return HandshakeType_Peer2PeerConnect;
    }
    if (type == HandshakeType_Disconnect) {
        return HandshakeType_Disconnect;
    }
    if (type == HandshakeType_S2CUnknownSourceAddress) {
        return HandshakeType_S2CUnknownSourceAddress;
    }
    if (static_cast<uint8_t>(type) == HandshakeType_C2SUpdateSourceAddress) {
        return HandshakeType_C2SUpdateSourceAddress;
    }
    return HandshakeType_NotHandshake;
}

uint64_t GetKey(const uint8_t* data)
{
    return siamese::ReadU64_LE(data + 4);
}

uint32_t GetConnectionId(const uint8_t* data)
{
    return siamese::ReadU24_LE_Min4Bytes(data + 1);
}


} // namespace handshake


//------------------------------------------------------------------------------
// Packet Generators

void GenerateDisconnect(uint64_t key, uint8_t* datagram)
{
    // Write timestamp = 0
    siamese::WriteU24_LE(datagram, 0);
    datagram += protocol::kTimestampBytes;

    handshake::WriteDisconnect(datagram, key);
    datagram += kHandshakeBytes;

    // Write flags byte
    datagram[0] = kHandshakeMask | kConnectionMask | kSeqCompMask;
    ++datagram;

    // Write hash value = 0
    siamese::WriteU16_LE(datagram, 0);
    static_assert(kMessageFrameBytes == 2, "Update this");
}

void GenerateS2CCookieResponse(uint64_t cookie, uint8_t* datagram)
{
    // Write timestamp = 0
    siamese::WriteU24_LE(datagram, 0);
    datagram += protocol::kTimestampBytes;

    handshake::WriteS2CCookieResponse(datagram, cookie);
    datagram += kHandshakeBytes;

    // Write flags byte
    datagram[0] = kHandshakeMask | kConnectionMask | kSeqCompMask;
    ++datagram;

    // Write hash value = 0
    siamese::WriteU16_LE(datagram, 0);
    static_assert(kMessageFrameBytes == 2, "Update this");
}

void GenerateS2CUnknownSource(uint8_t* datagram)
{
    // Write timestamp = 0
    siamese::WriteU24_LE(datagram, 0);
    datagram += protocol::kTimestampBytes;

    handshake::WriteS2CUnknownSourceAddress(datagram);
    datagram += kHandshakeBytes;

    // Write flags byte
    datagram[0] = kHandshakeMask | kConnectionMask | kSeqCompMask;
    ++datagram;

    // Write hash value = 0
    siamese::WriteU16_LE(datagram, 0);
    static_assert(kMessageFrameBytes == 2, "Update this");
}


//------------------------------------------------------------------------------
// NAT Traversal Protocol

uint16_t CalculateExactNATPort(uint16_t externalPort, unsigned portIndex)
{
    TONK_DEBUG_ASSERT(externalPort != 0);

    // All later ports are randomly selected
    if (portIndex >= kNATRound1Exact_PortCount) {
        return 0;
    }

    // Do not poke around OS service ports
    static const unsigned kFirstClientPort = 1024;
    if (externalPort < kFirstClientPort) {
        return 0;
    }

    unsigned port;

    static const unsigned kBelowHalf = kNATRound1Exact_PortCount / 2;
    if (portIndex < kBelowHalf) {
        port = externalPort - kBelowHalf + portIndex;
    }
    else {
        port = externalPort + portIndex - kBelowHalf + 1;
    }

    // Do not fall off the edges
    static const unsigned kLastClientPort = 65535;
    if (port < kFirstClientPort || port > kLastClientPort) {
        return 0;
    }

    return static_cast<uint16_t>(port);
}

uint16_t CalculateFuzzyNATPort(uint16_t externalPort, siamese::PCGRandom& portPrng)
{
    TONK_DEBUG_ASSERT(externalPort != 0);

    int port = externalPort;
    port -= kNATFuzzyPortRange;
    port += portPrng.Next() % (kNATFuzzyPortRange * 2);
    if (port <= 0) {
        port += kNATFuzzyPortRange;
    }
    else if (port >= 65536) {
        port -= kNATFuzzyPortRange;
    }
    return static_cast<uint16_t>(port);
}

uint16_t CalculateRandomNATPort(siamese::PCGRandom& portPrng)
{
    return 1024 + (portPrng.Next() % (65536 - 1024));
}

// 1 = Receiver is the tie breaker if both sides manage to punch through
static const uint8_t kWinTiesBit = 1;

// 1 = Address encoded is IPv6, 0 = IPv4
static const uint8_t kAddressV6Bit = 2;

// 1 = Is the peer's NAT mapped external port field available?
// 0 = Field is not present
static const uint8_t kHasPeerNATBit = 4;

bool P2PConnectParams::Write(siamese::WriteByteStream& stream) const
{
    if (stream.Remaining() < kMaxBytes) {
        return false;
    }

    uint8_t flags = 0;
    if (WinTies) {
        flags |= kWinTiesBit;
    }
    if (PeerExternalAddress.address().is_v6()) {
        flags |= kAddressV6Bit;
    }
    if (PeerNATMappedPort != 0) {
        flags |= kHasPeerNATBit;
    }

    stream.Write8(flags);
    stream.Write64(EncryptionKey);
    stream.Write16(ProtocolRoundIntervalMsec);
    stream.Write16(ShotTS16);
    stream.Write16(SelfExternalPort);
    if (PeerNATMappedPort != 0) {
        stream.Write16(PeerNATMappedPort);
    }
    stream.Write16(PeerExternalAddress.port());

    const asio::ip::address ipaddr = PeerExternalAddress.address();
    if (ipaddr.is_v4())
    {
        const asio::ip::address_v4 v4addr = ipaddr.to_v4();
        const auto addrData = v4addr.to_bytes();
        stream.WriteBuffer(&addrData[0], addrData.size());
    }
    else if (ipaddr.is_v6())
    {
        const asio::ip::address_v6 v6addr = ipaddr.to_v6();
        const auto addrData = v6addr.to_bytes();
        stream.WriteBuffer(&addrData[0], addrData.size());
    }
    else
    {
        TONK_DEBUG_BREAK();
        return false;
    }

    return true;
}

bool P2PConnectParams::Read(siamese::ReadByteStream& stream)
{
    if (stream.Remaining() < kMinBytes) {
        return false;
    }

    const uint8_t flags = stream.Read8();

    WinTies = (flags & kWinTiesBit) != 0;
    EncryptionKey = stream.Read64();
    ProtocolRoundIntervalMsec = stream.Read16();
    ShotTS16 = stream.Read16();
    SelfExternalPort = stream.Read16();
    if (0 != (flags & kHasPeerNATBit)) {
        PeerNATMappedPort = stream.Read16();
    }
    else {
        PeerNATMappedPort = 0;
    }

    const uint16_t port = stream.Read16();

    if (0 != (flags & kAddressV6Bit))
    {
        // IPv6 address is 16 bytes instead of 4
        if (stream.Remaining() < kMaxBytes) {
            return false;
        }

        auto addrPtr = reinterpret_cast<const std::array<uint8_t, 16>*>(stream.Read(16));
        PeerExternalAddress = UDPAddress(asio::ip::address_v6(*addrPtr), port);
    }
    else
    {
        auto addrPtr = reinterpret_cast<const std::array<uint8_t, 4>*>(stream.Read(4));
        PeerExternalAddress = UDPAddress(asio::ip::address_v4(*addrPtr), port);
    }

    return true;
}


}} // namespace tonk::protocol
