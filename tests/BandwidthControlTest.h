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

#pragma once

#include "Logger.h"
#include "TonkCppSDK.h"
#include "mau/mau.h"

#include <string>
#include <array>
#include <vector>

namespace test {


//------------------------------------------------------------------------------
// Constants

#define BWT_ENABLE_MAU

#if 0

static const int kParallelTests = 1;
//#define BWC_FULL_DUPLEX

#else

#if defined(_DEBUG) || defined(DEBUG)
static const int kParallelTests = 10;

#define BWC_FULL_DUPLEX
#else
static const int kParallelTests = 100;

#define BWC_FULL_DUPLEX
#endif

#endif


//------------------------------------------------------------------------------
// Test for BandwidthControl Component

struct BWCTestChannel;

class BWCTestSocket : public tonk::SDKSocket
{
public:
    BWCTestChannel* Channel = nullptr;
    bool Failed = false;
    bool Succeeded = false;

protected:
    tonk::SDKConnection* OnIncomingConnection(
        const TonkAddress& address ///< Address of the client requesting a connection
    ) override;
};

class BWCTestConnection : public tonk::SDKConnection
{
public:
    BWCTestSocket* Socket = nullptr;
    unsigned Sent = 0;
    unsigned Received = 0;

    unsigned SendBPS = 0;
    uint64_t LastRateUsec = 0;

protected:
    void OnConnect() override;
    void OnData(
        uint32_t          channel,  ///< Channel number attached to each message by sender
        const uint8_t*       data,  ///< Pointer to a buffer containing the message data
        uint32_t            bytes   ///< Number of bytes in the message
    ) override;
    void OnTick(
        uint64_t          nowUsec   ///< Current timestamp in microseconds
    ) override;
    void OnClose(
        const tonk::SDKJsonResult& reason
    ) override;
};

struct BWCTestChannel
{
    struct BandwidthControlTest* OverallTest = nullptr;
    const uint8_t* FileData = nullptr;
    unsigned FileSize = 0;
    uint16_t ClientPort, ServerPort;
    BWCTestSocket Client, Server;
#ifdef BWT_ENABLE_MAU
    MauChannelConfig ChannelConfig;
    MauProxyConfig ProxyConfig;
    MauProxy Proxy;
#endif // BTW_ENABLE_MAU
};

struct BandwidthControlTest
{
    std::array<BWCTestChannel, kParallelTests> Tests;
    std::vector<uint8_t> TestFile;
    std::atomic<int> SuccessCount = ATOMIC_VAR_INIT(0);
    std::atomic<bool> Failure = ATOMIC_VAR_INIT(false);

    // Set up for test
    bool Initialize();

    // Run the test
    bool RunTest();
};


} // namespace test
