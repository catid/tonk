/*
    Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

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

#include "TonkTestTools.h"

#include "../TonkineseProtocol.h"
#include "../TimeSync.h"
#include "../TonkineseBandwidth.h"
#include "../PacketCompression.h"
#include "BandwidthControlTest.h"

#ifdef VERBOSE_LOGS
static logger::Channel Logger("UnitTest", logger::Level::Trace);
#else
static logger::Channel Logger("UnitTest", logger::Level::Debug);
#endif


namespace tonk {

bool TestReadFooterField()
{
    uint8_t footer[8] = {
        0, 1, 2, 3, /* read behind here */ 4, 5, 6, 7
    };

    {
        uint32_t x = protocol::ReadFooterField(footer + 4, 0);
        if (x != 0)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }
    {
        uint32_t x = protocol::ReadFooterField(footer + 4, 1);
        if (x != 0x03)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }
    {
        uint32_t x = protocol::ReadFooterField(footer + 4, 2);
        if (x != 0x0302)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
        if (x != *(uint16_t*)(footer + 4 - 2))
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }
    {
        uint32_t x = protocol::ReadFooterField(footer + 4, 3);
        if (x != 0x030201)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    {
        protocol::WriteFooterField(footer + 4, 0, 0);
        uint32_t x = protocol::ReadFooterField(footer + 4, 0);
        if (x != 0)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }
    {
        const uint32_t value = 0x67;
        const uint32_t bytes = 1;
        protocol::WriteFooterField(footer + 4, value, bytes);
        uint32_t x = protocol::ReadFooterField(footer + 4 + bytes, bytes);
        if (x != value)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }
    {
        const uint32_t value = 0x4567;
        const uint32_t bytes = 2;
        protocol::WriteFooterField(footer + 4, value, bytes);
        uint32_t x = protocol::ReadFooterField(footer + 4 + bytes, bytes);
        if (x != value)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }
    {
        const uint32_t value = 0x234567;
        const uint32_t bytes = 3;
        protocol::WriteFooterField(footer + 4, value, bytes);
        uint32_t x = protocol::ReadFooterField(footer + 4 + bytes, bytes);
        if (x != value)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    return true;
}

} // namespace tonk







bool TestTimeSync()
{
    siamese::PCGRandom prng;
    prng.Seed(1);

    uint64_t s_bias = 0x65762a4f65762a4fULL;
    uint64_t t = 0x1234567891234567ULL;

    const uint64_t owdUsec = 1000 * 100; // 100 ms

    for (unsigned i = 0; i < 6; ++i)
    {
        TimeSynchronizer c_timesync, s_timesync;

        uint64_t c2s_remoteSendUsec = 0;
        uint64_t c2s_localRecvUsec = 0;

        uint64_t s2c_remoteSendUsec = 0;
        uint64_t s2c_localRecvUsec = 0;

        s_bias += prng.Next();
        t += prng.Next();

        // Initial setup:
        for (unsigned j = 0; j < 10000; ++j)
        {
            uint64_t c_sendTime = t;
            uint64_t c2s_delay = owdUsec + prng.Next() % (30 * 1000);
            t += c2s_delay; // up to 100 ms more
            uint64_t s_recvTime = t + s_bias;
            t += prng.Next() % (30 * 1000); // up to 30 ms more
            uint64_t s_sendTime = t + s_bias;
            uint64_t s2c_delay = owdUsec + prng.Next() % (30 * 1000); // up to 100 ms more
            t += s2c_delay;
            uint64_t c_recvTime = t;
            t += owdUsec + prng.Next() % (30 * 1000); // up to 100 ms more

            c2s_remoteSendUsec = c_sendTime;
            Counter24 c2s_remoteSendTS24 = (uint32_t)(c2s_remoteSendUsec >> kTime23LostBits);
            c2s_localRecvUsec = s_recvTime;
            s_timesync.OnAuthenticatedDatagramTimestamp(c2s_remoteSendTS24, c2s_localRecvUsec);

            s2c_remoteSendUsec = s_sendTime;
            Counter24 s2c_remoteSendTS24 = (uint32_t)(s2c_remoteSendUsec >> kTime23LostBits);
            s2c_localRecvUsec = c_recvTime;
            c_timesync.OnAuthenticatedDatagramTimestamp(s2c_remoteSendTS24, s2c_localRecvUsec);

            Counter24 c_min = c_timesync.GetMinDeltaTS24();
            Counter24 s_min = s_timesync.GetMinDeltaTS24();

            s_timesync.OnPeerMinDeltaTS24(c_min);
            c_timesync.OnPeerMinDeltaTS24(s_min);
        }

        if (!c_timesync.IsSynchronized())
        {
            TONK_DEBUG_BREAK();
            return false;
        }

        if (!s_timesync.IsSynchronized())
        {
            TONK_DEBUG_BREAK();
            return false;
        }

        // Client sends timestamp x to server:
        for (unsigned j = 0; j < 1000; ++j)
        {
            uint64_t c_sendTime = t;
            uint16_t x = c_timesync.ToRemoteTime16(c_sendTime);
            uint64_t actualServerTime = c_sendTime + s_bias;

            uint64_t delay = owdUsec + prng.Next() % (100 * 1000); // up to 100 ms more
            t += delay;

            uint64_t s_recvTime = t + s_bias;
            uint64_t y = s_timesync.FromLocalTime16(s_recvTime, x);

            int64_t delta = (int64_t)(y - actualServerTime);

            if (delta < -2000 || delta > 2000)
            {
                TONK_DEBUG_BREAK();
                return false;
            }

            t += prng.Next() % (30 * 1000); // up to 30 ms more
        }

        // Server sends timestamp x to client:
        for (unsigned j = 0; j < 1000; ++j)
        {
            uint64_t s_sendTime = t + s_bias;
            uint16_t x = s_timesync.ToRemoteTime16(s_sendTime);
            uint64_t actualClientTime = s_sendTime - s_bias;

            uint64_t delay = owdUsec + prng.Next() % (100 * 1000); // up to 100 ms more
            t += delay;

            uint64_t c_recvTime = t;
            uint64_t y = c_timesync.FromLocalTime16(c_recvTime, x);

            int64_t delta = (int64_t)(y - actualClientTime);

            if (delta < -3000 || delta > 3000)
            {
                TONK_DEBUG_BREAK();
                return false;
            }

            t += prng.Next() % (30 * 1000); // up to 30 ms more
        }
    }

    return true;
}


bool TestFixedPointCompress()
{
    for (uint32_t word = 0; word < 2048; ++word)
    {
        uint16_t x = tonk::FixedPointCompress32to16(word);
        uint32_t r = tonk::FixedPointDecompress16to32(x);
        if (r != word)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    for (uint32_t word = 2048; word < 0xffffff00; word += 16)
    {
        uint16_t x = tonk::FixedPointCompress32to16(word);
        uint32_t r = tonk::FixedPointDecompress16to32(x);
        int32_t s = r - word;
        if (s < 0) {
            s = -s;
        }
        if (s > word * 0.001)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    for (uint32_t word = 0; word < 16; ++word)
    {
        uint8_t x = tonk::FixedPointCompress16to8((uint16_t)word);
        uint16_t r = tonk::FixedPointDecompress8to16(x);
        if (r != (uint16_t)word)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    for (uint32_t word = 16; word <= 0xffff; ++word)
    {
        uint8_t x = tonk::FixedPointCompress16to8((uint16_t)word);
        uint16_t r = tonk::FixedPointDecompress8to16(x);
        int16_t s = r - word;
        if (s < 0) {
            s = -s;
        }
        if (s > word * 0.13f)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    return true;
}


//-----------------------------------------------------------------------------
// Test data injection

struct InjectConnection;

struct InjectServer : tonk::SDKSocket
{
    uint16_t VirtualPort = 0;
    InjectServer* OtherPeer = nullptr;

    std::atomic<bool> GotConnected = ATOMIC_VAR_INIT(false);
    std::atomic<bool> GotTestData = ATOMIC_VAR_INIT(false);
    std::atomic<bool> Shutdown = ATOMIC_VAR_INIT(false);

    bool Initialize()
    {
        // Route send calls through this hook instead of socket
        this->Config.SendToAppContextPtr = (TonkAppContextPtr)this;
        this->Config.SendToHook = [](
            TonkAppContextPtr context, ///< [in] Application context pointer
            uint16_t         destPort, ///< [in] Destination port
            const uint8_t*       data, ///< [in] Message data
            uint32_t            bytes  ///< [in] Message bytes
            )
        {
            InjectServer* thiz = (InjectServer*)context;

            // If shutdown is in progress:
            if (thiz->Shutdown) {
                return; // Avoid referencing a dead peer object
            }

            //Logger.Debug("Using SendToHook and tonk_inject_recvfrom to proxy ", bytes, " bytes");
            tonk_inject(
                thiz->OtherPeer->GetSocket(),
                thiz->VirtualPort,
                data,
                bytes);
        };
        this->Config.MaximumClients = 10;

        auto result = this->Create();
        if (!result)
        {
            Logger.Error("Socket create failed: ", result.ToString());
            return false;
        }

        return true;
    }

    bool ConnectToPeer();

    // SDKSocket:
    tonk::SDKConnection* OnIncomingConnection(const TonkAddress& address) override;

    // List of connections
    tonk::SDKConnectionList<InjectConnection> ConnectionList;
};

struct InjectConnection : tonk::SDKConnection
{
    InjectServer* Server;

    InjectConnection(InjectServer* server)
    {
        Server = server;
    }

    void OnConnect() override
    {
        char message[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
        tonk::SDKResult result = Send(
            message,
            sizeof(message),
            TonkChannel_Reliable0);
        if (!result) {
            Logger.Error("Send failed: ", result.ToString());
        }
        Server->GotConnected = true;
    }
    void OnData(
        uint32_t          channel,  // Channel number attached to each message by sender
        const uint8_t*       data,  // Pointer to a buffer containing the message data
        uint32_t            bytes   // Number of bytes in the message
    ) override
    {
        if (channel == TonkChannel_Reliable0 &&
            bytes == 10)
        {
            for (unsigned i = 0; i < 10; ++i) {
                if (data[i] != i) {
                    return;
                }
            }
            Server->GotTestData = true;
        }
    }
    void OnTick(
        uint64_t          nowUsec   // Current timestamp in microseconds
    ) override
    {

    }
    void OnClose(
        const tonk::SDKJsonResult& reason
    ) override
    {
        Server->ConnectionList.Remove(this);
    }
};

bool InjectServer::ConnectToPeer()
{
    auto connPtr = std::make_shared<InjectConnection>(this);

    tonk::SDKResult result = Connect(
        connPtr.get(),
        "127.0.0.1",
        OtherPeer->VirtualPort);
    if (!result) {
        Logger.Error("Connect failed: ", result.ToString());
        return false;
    }

    ConnectionList.Insert(connPtr.get());

    return true;
}

tonk::SDKConnection* InjectServer::OnIncomingConnection(const TonkAddress& address)
{
    auto shared = std::make_shared<InjectConnection>(this);

    // Insert into connection list to prevent it from going out of scope
    ConnectionList.Insert(shared.get());

    return shared.get();
}


bool TestInjectInterface()
{
    InjectServer a, b;

    a.VirtualPort = 1;
    b.VirtualPort = 2;
    a.OtherPeer = &b;
    b.OtherPeer = &a;

    if (!a.Initialize()) {
        return false;
    }
    if (!b.Initialize()) {
        return false;
    }

    if (!a.ConnectToPeer()) {
        return false;
    }

    static const uint64_t kTimeoutSeconds = 2000;
    const uint64_t t0 = siamese::GetTimeMsec();

    for (;;)
    {
        const uint64_t t1 = siamese::GetTimeMsec();

        // If timeout expired:
        if (t1 - t0 > kTimeoutSeconds * 1000) {
            break;
        }

        // If test was successful:
        if (a.GotConnected && a.GotTestData &&
            b.GotConnected && b.GotTestData)
        {
            break;
        }

        // Wait a little longer
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // Mark objects shutdown so our SendHook does not route data to a dead object
    a.Shutdown = true;
    b.Shutdown = true;

    if (a.GotConnected && a.GotTestData &&
        b.GotConnected && b.GotTestData)
    {
        Logger.Info("Test successful: Used injection instead of sockets to form connection and exchange data");
        return true;
    }

    Logger.Error("Injection test failed: a.GotConnected=", a.GotConnected);
    Logger.Error("Injection test failed: a.GotTestData=", a.GotTestData);
    Logger.Error("Injection test failed: b.GotConnected=", b.GotConnected);
    Logger.Error("Injection test failed: b.GotTestData=", b.GotTestData);
    return false;
}


//-----------------------------------------------------------------------------
// BWC Test

bool TestBandwidthControl()
{
    test::BandwidthControlTest tester;

    if (!tester.Initialize())
    {
        Logger.Error("Unable to initialize BWCTest");
        return false;
    }

    if (!tester.RunTest())
    {
        Logger.Error("Error while running BWCTest");
        return false;
    }

    return true;
}


//-----------------------------------------------------------------------------
// Test for Itoa

bool TestItoa()
{
    for (unsigned value = 0; value <= 1000000; ++value)
    {
        std::string ref = std::to_string(value);

        char buf[32];
        memset(buf, 0xcc, sizeof(buf));

        unsigned digits = tonk::inplace_itoa_vitaut_1_cat(buf + 11, value);

        // Check digit count
        if (digits != ref.size()) {
            TONK_DEBUG_BREAK();
            return false;
        }

        // Look for memory corruption
        for (int i = 0; i < 11; ++i)
        {
            if (buf[i] != (char)0xcc) {
                TONK_DEBUG_BREAK();
                return false;
            }
        }
        for (int i = 11 + digits; i < 32; ++i)
        {
            if (buf[i] != (char)0xcc) {
                TONK_DEBUG_BREAK();
                return false;
            }
        }

        for (unsigned i = 0; i < digits; ++i)
        {
            if (buf[11 + i] != ref[i]) {
                TONK_DEBUG_BREAK();
                return false;
            }
        }
    }

    return true;
}


//-----------------------------------------------------------------------------
// Compression Test

bool TestCompression()
{
    tonk::MessageCompressor compressor;
    tonk::MessageDecompressor decompressor;

    static const unsigned kMaxCompressedBytes = 1300;

    tonk::Result result = compressor.Initialize(kMaxCompressedBytes);
    if (result.IsFail()) {
        TONK_DEBUG_BREAK();
        return false;
    }
    result = decompressor.Initialize(kMaxCompressedBytes);
    if (result.IsFail()) {
        TONK_DEBUG_BREAK();
        return false;
    }

    static const unsigned kMessageBytes = 400;
    static_assert(kMessageBytes % 4 == 0, "Fixme");

    uint8_t message[kMessageBytes];
    uint32_t* words = (uint32_t*)message;

    siamese::PCGRandom prng;

    uint8_t dest[kMessageBytes * 2];
    unsigned writtenBytes = 0;

    static const int kMessageCount = 100;

    for (int i = 0; i < kMessageCount; ++i)
    {
        int seed;
        if (i >= kMessageCount/2) {
            seed = kMessageCount - i;
        }
        else {
            seed = i;
        }
        prng.Seed(seed);
        for (int j = 0; j < kMessageBytes / 4; ++j) {
            words[j] = prng.Next();
        }

        result = compressor.Compress(
            message,
            kMessageBytes,
            dest,
            writtenBytes);
        if (result.IsFail()) {
            TONK_DEBUG_BREAK();
            return false;
        }

        if (writtenBytes > 0)
        {
            Logger.Info("Seed=", seed, ": Compressed ", kMessageBytes, " to ", writtenBytes);

            tonk::Decompressed decompressed;
            result = decompressor.Decompress(dest, writtenBytes, decompressed);
            if (result.IsFail()) {
                TONK_DEBUG_BREAK();
                return false;
            }

#if 0
            if (0 != memcmp(decompressor.History.Buffer, compressor.History.Buffer, tonk::kCompressionDictBytes))
            {
                TONK_DEBUG_BREAK();
                return false;
            }
#endif

            Logger.Info("Seed=", seed, ": Decompressed ", writtenBytes, " to ", decompressed.Bytes);

            if (decompressed.Bytes != kMessageBytes) {
                TONK_DEBUG_BREAK();
                return false;
            }
            if (0 != memcmp(decompressed.Data, message, kMessageBytes)) {
                TONK_DEBUG_BREAK();
                return false;
            }
        }
        else
        {
            Logger.Info("Seed=", seed, ": Compression unable to reduce size");

            decompressor.InsertUncompressed(message, kMessageBytes);

#if 0
            if (0 != memcmp(decompressor.History.Buffer, compressor.History.Buffer, tonk::kCompressionDictBytes))
            {
                TONK_DEBUG_BREAK();
                return false;
            }
#endif
        }
    }

    return true;
}


//-----------------------------------------------------------------------------
// SenderBandwidthControl Test

bool TestSenderBandwidthControl()
{
    tonk::SenderBandwidthControl control;
    logger::Channel logger("test channel", logger::Level::Silent);
    TimeSynchronizer sync;

    tonk::SenderBandwidthControl::Dependencies deps;
    deps.Logger = &logger;
    deps.TimeSync = &sync;
    tonk::SenderBandwidthControl::Parameters params;
    params.MaximumBPS = 4000000;

    control.Initialize(deps, params);

    tonk::BandwidthShape shape;
    shape.AppBPS = 10000;
    shape.FECRate = 0.01f;

    control.OnReceiverFeedback(shape);

    uint64_t nowUsec = 0;

    control.RecalculateAvailableBytes(nowUsec);

    int available = control.GetAvailableBytes();

    if (available != 0)
    {
        TONK_DEBUG_BREAK();
        return false;
    }

    for (int i = 1; i < 10; ++i)
    {
        nowUsec += 100000;
        control.RecalculateAvailableBytes(nowUsec);
        available = control.GetAvailableBytes();

        if (available != 100 * i)
        {
            TONK_DEBUG_BREAK();
            return false;
        }
    }

    return true;
}


//-----------------------------------------------------------------------------
// Main

int main()
{
    if (!TestSenderBandwidthControl())
    {
        Logger.Error("Failure: SenderBandwidthControl");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!TestBandwidthControl())
    {
        Logger.Error("Failure: TestBandwidthControl");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!TestInjectInterface())
    {
        Logger.Error("Failure: TestInjectInterface");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!TestCompression())
    {
        Logger.Error("Failure: TestCompression");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!TestItoa())
    {
        Logger.Error("Failure: TestItoa");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!tonk::TestReadFooterField())
    {
        Logger.Error("Failure: footer reader test failed");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!TestFixedPointCompress())
    {
        Logger.Error("Failure: TestFixedPointCompress");
        TONK_DEBUG_BREAK();
        return -1;
    }

    if (!TestTimeSync())
    {
        Logger.Error("Failure: time sync test failed");
        TONK_DEBUG_BREAK();
        return -1;
    }

    Logger.Debug("SUCCESS! Press ENTER key to terminate");

    logger::Flush();

    ::getchar();
    Logger.Debug("...Key press detected.  Terminating..");

    logger::Flush();

    return 0;
}
