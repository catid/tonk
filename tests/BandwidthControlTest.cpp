/*
    Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

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

#include "BandwidthControlTest.h"
#include "SiameseTools.h"

namespace test {

static logger::Channel Logger("Test", logger::Level::Debug);


//------------------------------------------------------------------------------
// Test for BandwidthControl Component

bool BandwidthControlTest::Initialize()
{
    uint32_t seed = 4;

    // Generate a test file to send
    TestFile.resize(100 * 1000000);
    siamese::PCGRandom prng;
    prng.Seed(seed);
    uint64_t* fileData = (uint64_t*)TestFile.data();
    unsigned fileWords = (unsigned)(TestFile.size() / 8);
#if 0
    for (unsigned i = 0; i < fileWords; i += 4)
    {
        uint64_t x = prng.Next();
        x |= (uint64_t)prng.Next() << 32;
        for (int j = 0; j < 4; ++j) {
            fileData[i + j] = x;
        }
    }

    for (unsigned i = 0; i < fileWords; i += 2000)
    {
        for (unsigned j = 0; j < 1400/8; ++j)
        {
            uint64_t x = prng.Next();
            x |= (uint64_t)prng.Next() << 32;
            fileData[i + j] = x;
        }
    }
#else
    for (unsigned i = 0; i < fileWords; ++i)
    {
        uint64_t x = prng.Next();
        x |= (uint64_t)prng.Next() << 32;
        fileData[i] = x; // &0xfefefefefefefefeULL;
    }
#endif

    for (BWCTestChannel& test : Tests)
    {
        test.ClientPort = 1;
        test.ServerPort = 2;

        test.OverallTest = this;
        test.FileData = TestFile.data();
        test.FileSize = (unsigned)TestFile.size();

#ifdef BWT_ENABLE_MAU
        // Set channel character
        test.ChannelConfig.RNGSeed = seed++;
        test.ChannelConfig.LossRate = 0.01f;
        test.ChannelConfig.DeliveryRate = 1.f;
        test.ChannelConfig.Router_MBPS = 4.f;
        test.ChannelConfig.Router_QueueMsec = 1000;
        test.ChannelConfig.Router_RED_QueueFraction = 0.5f;
        test.ChannelConfig.LightSpeedMsec = 100;

        test.ChannelConfig.DuplicateRate = 0.001f;
        test.ChannelConfig.CorruptionRate = 0.001f;
        //test.ChannelConfig.DuplicateRate = 0.f;
        //test.ChannelConfig.CorruptionRate = 0.f;

        // Reordering settings
        test.ChannelConfig.ReorderMinimumLatencyMsec = 100;
        test.ChannelConfig.ReorderMaximumLatencyMsec = 550;
        test.ChannelConfig.OrderRate = 0.8f;
        test.ChannelConfig.ReorderRate = 0.01f;
        //test.ChannelConfig.ReorderRate = 0.005f;

        // Configure proxy
        test.ProxyConfig.Version = MAU_VERSION;
        test.ProxyConfig.MaxDatagramBytes = 2000;
        test.ProxyConfig.UDPListenPort = 0; // Should not require an actual UDP port
        test.ProxyConfig.UDPRecvBufferSizeBytes = 0;
        test.ProxyConfig.UDPSendBufferSizeBytes = 0;
        test.ProxyConfig.SendHookContext = &test;
        test.ProxyConfig.SendHook = [](
            MauAppContextPtr context,
            uint16_t destPort,
            const void* data,
            unsigned bytes)
        {
            BWCTestChannel* channel = reinterpret_cast<BWCTestChannel*>(context);
            TonkSocket destSock;
            uint16_t sourcePort;

            if (destPort == channel->ServerPort)
            {
                destSock = channel->Server.GetSocket();
                sourcePort = channel->ClientPort;
            }
            else if (destPort == channel->ClientPort)
            {
                destSock = channel->Client.GetSocket();
                sourcePort = channel->ServerPort;
            }
            else
            {
                TONK_CPP_SDK_DEBUG_BREAK(); // Should never happen
                return;
            }

            tonk_inject(destSock, sourcePort, data, bytes);
        };

        // Create the proxy object
        MauResult mauResult = mau_proxy_create(
            &test.ProxyConfig,
            &test.ChannelConfig,
            "127.0.0.1",
            test.ServerPort,
            &test.Proxy);
        if (mauResult != Mau_Success)
        {
            Logger.Error("Mau proxy create failed: ", mauResult);
            TONK_CPP_SDK_DEBUG_BREAK();
            return false;
        }
#endif // BWT_ENABLE_MAU

        // Configure the Tonk objects
        test.Client.Config.BandwidthLimitBPS = 20 * 1000000;
        test.Client.Config.ConnectionTimeoutUsec = 2 * 1000000;
        test.Client.Config.InterfaceAddress = nullptr;
        test.Client.Config.MaximumClients = 1;
        test.Client.Config.NoDataTimeoutUsec = 20 * 1000000;
        test.Client.Config.TimerIntervalUsec = 20 * 1000;
        test.Client.Config.UDPConnectIntervalUsec = 100 * 1000;
        test.Client.Config.UDPRecvBufferSizeBytes = 20 * 1000;
        test.Client.Config.UDPSendBufferSizeBytes = 20 * 1000;
        test.Client.Config.WorkerCount = 2;
        uint32_t flags = 0;
        //flags |= TONK_FLAGS_ENABLE_FEC;
        //flags |= TONK_FLAGS_ENABLE_UPNP;
        flags |= TONK_FLAGS_DISABLE_COMPRESSION;
        test.Client.Config.Flags = flags;

        // Both client and server bind to a random port
        test.Client.Config.UDPListenPort = 0;

        // Copy client settings to server
        test.Server.Config = test.Client.Config;

        // Client-specific settings
        test.Client.Config.SendToAppContextPtr = &test;
        test.Client.Config.SendToHook = [](
            TonkAppContextPtr context, ///< [in] Application context pointer
            uint16_t         destPort, ///< [in] Destination port
            const uint8_t*       data, ///< [in] Message data
            unsigned            bytes  ///< [in] Message bytes
            )
        {
            BWCTestChannel* channel = reinterpret_cast<BWCTestChannel*>(context);

#ifdef BWT_ENABLE_MAU
            mau_proxy_inject(
                channel->Proxy,
                channel->ClientPort,
                data,
                bytes);
#else
            tonk_inject(channel->Server.GetSocket(), channel->ClientPort, data, bytes);
#endif
        };
        test.Client.Channel = &test;

        // Server-specific settings
        test.Server.Config.SendToAppContextPtr = &test;
        test.Server.Config.SendToHook = [](
            TonkAppContextPtr context, ///< [in] Application context pointer
            uint16_t         destPort, ///< [in] Destination port
            const uint8_t*       data, ///< [in] Message data
            unsigned            bytes  ///< [in] Message bytes
            )
        {
            BWCTestChannel* channel = reinterpret_cast<BWCTestChannel*>(context);

#ifdef BWT_ENABLE_MAU
            mau_proxy_inject(
                channel->Proxy,
                channel->ServerPort,
                data,
                bytes);
#else
            tonk_inject(channel->Client.GetSocket(), channel->ServerPort, data, bytes);
#endif
        };
        test.Server.Channel = &test;

        tonk::SDKResult serverResult = test.Server.Create();
        if (serverResult.Failed())
        {
            Logger.Error("Server create failed: ", serverResult.ToString());
            TONK_CPP_SDK_DEBUG_BREAK();
            return false;
        }

        tonk::SDKResult clientResult = test.Client.Create();
        if (clientResult.Failed())
        {
            Logger.Error("Client create failed: ", clientResult.ToString());
            TONK_CPP_SDK_DEBUG_BREAK();
            return false;
        }
    }

    return true;
}

bool BandwidthControlTest::RunTest()
{
    bool success = false;

    for (BWCTestChannel& test : Tests)
    {
        auto connPtr = std::make_shared<BWCTestConnection>();
        connPtr->Socket = &test.Client;

        tonk::SDKResult result = test.Client.Connect(
            connPtr.get(),
            "127.0.0.1",
            test.ServerPort);
        if (!result)
        {
            Logger.Error("Connect failed: ", result.ToString());
            return false;
        }
    }

    uint64_t t0 = tonk_time();
    static const uint64_t kMaxDuration = 300 * 1000 * 1000;

#ifdef BWC_FULL_DUPLEX
    const int stopCount = 2 * (int)Tests.size();
#else
    const int stopCount = (int)Tests.size();
#endif

    for (;;)
    {
        if (Failure)
        {
            TONK_CPP_SDK_DEBUG_BREAK();
            Logger.Error("One of the tests failed oh no!");
            break;
        }
        if (SuccessCount >= stopCount)
        {
            success = true;
            Logger.Info("Stopping early - All the tests completed successfully");
            break;
        }
        uint64_t t1 = tonk_time();
        uint64_t duration = t1 - t0;
        if (duration > kMaxDuration)
        {
            TONK_CPP_SDK_DEBUG_BREAK();
            Logger.Error("Timed out before file was transferred");
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

#if 0
        /*
            Test channel shapes:

            Step functions.
            Ramp up.
            Ramp down.
            Finish at a high rate to drain.
        */

        // Add changing channel
        static int ctr = 0;
        ++ctr;

        if (ctr < 100)
        {
            for (BWCTestChannel& test : Tests)
            {
                test.ChannelConfig.Router_MBPS = 1.f;
                mau_proxy_config(test.Proxy, &test.ChannelConfig);
            }
        }
        else if (ctr < 200)
        {
            for (BWCTestChannel& test : Tests)
            {
                test.ChannelConfig.Router_MBPS = 2.f;
                mau_proxy_config(test.Proxy, &test.ChannelConfig);
            }
        }
        else if (ctr < 300)
        {
            for (BWCTestChannel& test : Tests)
            {
                test.ChannelConfig.Router_MBPS = 1.5f;
                mau_proxy_config(test.Proxy, &test.ChannelConfig);
            }
        }
        else if (ctr < 400)
        {
            for (BWCTestChannel& test : Tests)
            {
                test.ChannelConfig.Router_MBPS += 0.01f;
                mau_proxy_config(test.Proxy, &test.ChannelConfig);
            }
        }
        else if (ctr < 500)
        {
            for (BWCTestChannel& test : Tests)
            {
                test.ChannelConfig.Router_MBPS -= 0.01f;
                mau_proxy_config(test.Proxy, &test.ChannelConfig);
            }
        }
        else
        {
            for (BWCTestChannel& test : Tests)
            {
                test.ChannelConfig.Router_MBPS = 3.f;
                mau_proxy_config(test.Proxy, &test.ChannelConfig);
            }
        }
#endif
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5000));

    for (BWCTestChannel& test : Tests)
    {
#ifdef BWT_ENABLE_MAU
        // Disable the proxy so that we can shut down each end asynchronously
        mau_proxy_stop(test.Proxy);
#endif // BTW_ENABLE_MAU

        // Destroy the client and then the server, connection and socket
        test.Client.Destroy();
        test.Server.Destroy();

#ifdef BWT_ENABLE_MAU
        mau_proxy_destroy(test.Proxy);
#endif // BTW_ENABLE_MAU
    }

    return success;
}


tonk::SDKConnection* BWCTestSocket::OnIncomingConnection(
    const TonkAddress& address ///< Address of the client requesting a connection
)
{
    auto shared = std::make_shared<BWCTestConnection>();
    shared->SetSelfReference();
    shared->Socket = this;

    Logger.Debug("Test server got connection request from ", address.NetworkString, " : ", address.UDPPort);

    return shared.get();
}

void BWCTestConnection::OnConnect()
{
    //TonkStatus status = GetStatus();
    TonkStatusEx statusEx = GetStatusEx();
    Logger.Debug("Connected with ", statusEx.Remote.NetworkString, " : ", statusEx.Remote.UDPPort);
}

void BWCTestConnection::OnData(
    uint32_t          channel,  ///< Channel number attached to each message by sender
    const uint8_t*       data,  ///< Pointer to a buffer containing the message data
    uint32_t            bytes   ///< Number of bytes in the message
)
{
    const uint8_t* fileData = Socket->Channel->FileData;
    const unsigned fileSize = Socket->Channel->FileSize;

    if (channel == TonkChannel_Reliable0 + 1)
    {
        if (Received + bytes > fileSize)
        {
            TONK_CPP_SDK_DEBUG_BREAK(); // Invalid input
            Logger.Error("TEST FAIL: Too many bytes received");
            Socket->Failed = true;
            Socket->Channel->OverallTest->Failure = true;
            return;
        }
        if (0 != memcmp(data, fileData + Received, bytes))
        {
            TONK_CPP_SDK_DEBUG_BREAK(); // Invalid input
            Logger.Error("TEST FAIL: Data mismatch");
            Socket->Failed = true;
            Socket->Channel->OverallTest->Failure = true;
            return;
        }
        Received += bytes;
        //Logger.Info("Got ", Received);
        if (Received == fileSize && !Socket->Failed)
        {
            Socket->Succeeded = true;
            Socket->Channel->OverallTest->SuccessCount++;
            Logger.Info("File received successfully!");
        }
    }
}

void BWCTestConnection::OnTick(
    uint64_t          nowUsec   ///< Current timestamp in microseconds
)
{
    TonkStatus status = GetStatus();
#ifndef BWC_FULL_DUPLEX
    if (0 != (status.Flags & TonkFlag_Initiated))
    {
        return;
    }
#endif
    unsigned reliableDepthUsec = status.ReliableQueueMsec * 1000;
    unsigned targetDepthUsec = status.TimerIntervalUsec * 2;
    if (reliableDepthUsec < targetDepthUsec)
    {
#if 0
        // Simulate a video encoder that cannot send at precisely the target rate
        if (nowUsec - LastRateUsec > 300000)
        {
            LastRateUsec = nowUsec;
            SendBPS = (unsigned)(status.AppBPS * 0.9);
        }
#else
        SendBPS = status.AppBPS;
#endif

        unsigned deficitUsec = targetDepthUsec - reliableDepthUsec;
        unsigned deficitBytes = ((uint64_t)SendBPS * deficitUsec) / 1000000;

        const uint8_t* fileData = Socket->Channel->FileData;
        const unsigned fileSize = Socket->Channel->FileSize;

        static const unsigned kBytesChunk = 8192;
        for (unsigned bytes = 0; bytes < deficitBytes; bytes += kBytesChunk)
        {
            if (Sent >= fileSize)
            {
                break;
            }

            unsigned sendBytes = kBytesChunk;
            if (Sent + sendBytes > fileSize)
            {
                sendBytes = fileSize - Sent;
            }

            Send(fileData + Sent, sendBytes, TonkChannel_Reliable0 + 1);
            //Logger.Info("Tick: Sending ", Sent, " BPS = ", status.AppBPS);

            Sent += sendBytes;
        }
    }

#if 0
    uint8_t extra[10] = {};
    Send(extra, 10, TonkChannel_Reliable0);
    //Send(extra, 10, TonkChannel_Reliable0 + 1);
    Send(extra, 10, TonkChannel_Reliable0 + 2);
    Send(extra, 10, TonkChannel_Reliable0 + 3);
    Send(extra, 10, TonkChannel_Reliable0 + 4);
    Send(extra, 10, TonkChannel_Reliable0 + 5);
    Send(extra, 10, TonkChannel_LowPri0);
    Send(extra, 10, TonkChannel_LowPri0 + 1);
    Send(extra, 10, TonkChannel_LowPri0 + 2);
    Send(extra, 10, TonkChannel_LowPri0 + 3);
    Send(extra, 10, TonkChannel_LowPri0 + 4);
    Send(extra, 10, TonkChannel_Reliable0);
#endif
}

void BWCTestConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    Logger.Debug("Connection closed: ", reason.ToString());
}


} // namespace test
