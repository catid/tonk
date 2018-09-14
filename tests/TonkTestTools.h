/** \file
    \brief Tonk Test Tools
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

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

#include "Logger.h"
#include "TonkCppSDK.h"

#include <string>
#include <array>

#define VERBOSE_LOGS

namespace test {


//------------------------------------------------------------------------------
// Shared Test Constants

static const uint16_t kUDPServerPort_Server = 5060;

#define TONK_TEST_SERVER_IP "slycog.com"
//#define TONK_TEST_SERVER_IP "localhost"

static const uint16_t kUDPServerPort_Client = 5060;
//static const uint16_t kUDPServerPort_Client = 10200;

static const uint8_t ID_P2PConnectionStart = 2;
static const uint8_t ID_ConnectionAdded = 3;
static const uint8_t ID_ConnectionRemoved = 4;
static const uint8_t ID_ConnectionRebroadcast = 5;
static const uint8_t ID_PreConnectDataTest = 6;
static const uint8_t ID_LowPriBulkData_NoTimestamp = 7;
static const uint8_t ID_LowPriBulkData_HasTimestamp = 8;

static const uint8_t ID_Advertise_Ping = 100;
static const uint8_t ID_Advertise_Pong = 101;
static const unsigned kPongBytes = 5;

static const unsigned kLowPriBulkData_Channel = TonkChannel_LowPri0 + 2;
static const unsigned kLowPriBulkData_Bytes = 1400;
static const uint32_t kLowPriBulkMagic = 0xadd12d; // 24 bits
static const unsigned kStatsReportIntervalUsec = 2000000;
static const unsigned kLowPriQueueDepthMsec = 50;

bool SendPreconnectData(tonk::SDKConnection* connection);
bool ValidatePreconnectData(unsigned channel, const uint8_t* data, unsigned bytes);

#define TONK_ENABLE_1MBPS_DATA_SERVER
//#define TONK_ENABLE_1MBPS_DATA_CLIENT


//------------------------------------------------------------------------------
// Shared Test Helpers

void LogTonkStatus(tonk::SDKConnection* connection);


//------------------------------------------------------------------------------
// FunctionTimer

class FunctionTimer
{
public:
    FunctionTimer(const std::string& name);

    void BeginCall();
    void EndCall();
    void Reset();
    void Print(unsigned trials = 1);


    std::mutex CallLock;
    uint64_t t0 = 0;
    uint64_t Invokations = 0;
    uint64_t TotalUsec = 0;
    std::string FunctionName;
};


//------------------------------------------------------------------------------
// FunctionTimers

extern FunctionTimer t_tonk_socket_create;
extern FunctionTimer t_tonk_socket_destroy;
extern FunctionTimer t_tonk_connect;
extern FunctionTimer t_tonk_send;
extern FunctionTimer t_tonk_flush;
extern FunctionTimer t_tonk_free;


//------------------------------------------------------------------------------
// OWDTimeStatistics

static const unsigned kOWDStatsInterval = 200;

struct OWDTimeStatistics
{
    static const unsigned kMaxSamples = 1000;
    std::array<uint64_t, kMaxSamples> Samples;

    unsigned SampleCount = 0;
    unsigned SampleIndex = 0;

    void Clear()
    {
        SampleCount = 0;
        SampleIndex = 0;
    }
    void AddSample(uint64_t owdUsec);
    void PrintStatistics();
};


} // namespace test
