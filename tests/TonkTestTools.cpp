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

#include "TonkTestTools.h"

#include <cassert>

namespace test {


#ifdef VERBOSE_LOGS
static logger::Channel Logger("Test", logger::Level::Trace);
#else
static logger::Channel Logger("Test", logger::Level::Debug);
#endif


//------------------------------------------------------------------------------
// FunctionTimers

FunctionTimer t_tonk_socket_create("tonk_socket_create");
FunctionTimer t_tonk_socket_destroy("tonk_socket_destroy");
FunctionTimer t_tonk_connect("tonk_connect");
FunctionTimer t_tonk_send("tonk_send");
FunctionTimer t_tonk_flush("tonk_flush");
FunctionTimer t_tonk_free("tonk_free");


//------------------------------------------------------------------------------
// FunctionTimer

FunctionTimer::FunctionTimer(const std::string& name)
{
    FunctionName = name;
}

void FunctionTimer::BeginCall()
{
    CallLock.lock();
    assert(t0 == 0);
    t0 = tonk_time();
}

void FunctionTimer::EndCall()
{
    assert(t0 != 0);
    uint64_t t1 = tonk_time();
    ++Invokations;
    TotalUsec += t1 - t0;
    t0 = 0;
    CallLock.unlock();
}

void FunctionTimer::Reset()
{
    assert(t0 == 0);
    t0 = 0;
    Invokations = 0;
    TotalUsec = 0;
}

void FunctionTimer::Print(unsigned trials)
{
    if (trials == 1)
    {
        Logger.Info(FunctionName, " called ", Invokations, " times.  ", TotalUsec / (double)Invokations, " usec avg for all invokations.  ", TotalUsec, " usec");
    }
    else
    {
        Logger.Info(FunctionName, " called ", Invokations / (float)trials, " times per trial (avg).  ", TotalUsec / (double)Invokations, " usec avg for all invokations.  ", TotalUsec / (float)trials, " usec (avg) of ", trials, " trials");
    }
}


//------------------------------------------------------------------------------
// Shared Test Helpers

void LogTonkStatus(tonk::SDKConnection* connection)
{
    TonkStatus status = connection->GetStatus();
    TonkStatusEx statusEx = connection->GetStatusEx();

    std::ostringstream ss;
    if (status.Flags & TonkFlag_Connecting) {
        ss << "TonkFlag_Connecting ";
    }
    if (status.Flags & TonkFlag_Connected) {
        ss << "TonkFlag_Connected ";
    }
    if (status.Flags & TonkFlag_Disconnected) {
        ss << "TonkFlag_Disconnected ";
    }
    if (status.Flags & TonkFlag_Initiated) {
        ss << "TonkFlag_Initiated ";
    }
    if (status.Flags & TonkFlag_TimeSync) {
        ss << "TonkFlag_TimeSync ";
    }
    if (status.Flags & TonkFlag_NATMap_Local) {
        ss << "TonkFlag_NATMap_Local ";
    }
    if (status.Flags & TonkFlag_NATMap_Remote) {
        ss << "TonkFlag_NATMap_Remote ";
    }
    ss << " AppBPS=" << status.AppBPS;
    ss << " ReliableQueueMsec=" << status.ReliableQueueMsec;
    ss << " LowPriQueueMsec=" << status.LowPriQueueMsec;
    //ss << " ConnectionKey=" << statusEx.ConnectionKey;
    ss << " RemoteId=" << status.LocallyAssignedIdForRemoteHost;
    ss << " LocalId=" << statusEx.RemoteAssignedIdForLocalHost;
    if (statusEx.LocalNATMapExternalPort != 0) {
        ss << " LocalUPnPPort=" << statusEx.LocalNATMapExternalPort;
    }
    if (statusEx.RemoteNATMapExternalPort != 0) {
        ss << " RemoteUPnPPort=" << statusEx.RemoteNATMapExternalPort;
    }
    ss << " Self=" << statusEx.Local.NetworkString << ":" << statusEx.Local.UDPPort;
    ss << " Remote=" << statusEx.Remote.NetworkString << ":" << statusEx.Remote.UDPPort;
    ss << " PeerSeenBPS=" << statusEx.PeerSeenBPS;
    ss << " PeerSeenLossRate=" << statusEx.PeerSeenLossRate;
    ss << " IncomingBPS=" << statusEx.IncomingBPS;
    ss << " IncomingLossRate=" << statusEx.IncomingLossRate;
    ss << " TripUsec=" << statusEx.TripUsec;
    Logger.Info("tonk_status: ", ss.str());
}

static const unsigned kPreConnectDataBytes = 10;
static const unsigned kPreConnectChannel = TonkChannel_Reliable0 + TonkChannel_Last;
static const uint8_t PreConnectData[kPreConnectDataBytes] = {
    ID_PreConnectDataTest,
    9, 8, 7, 6, 5, 4, 3, 2, 1
};

bool SendPreconnectData(tonk::SDKConnection* connection)
{
    uint8_t hello[1 + 1400] = {};
    hello[0] = ID_ConnectionRebroadcast;
    std::string Hello = "Pre-connect message";
    memcpy(hello + 1, Hello.c_str(), Hello.length() + 1);

    t_tonk_send.BeginCall();
    tonk::SDKResult result = connection->Send(hello, 1 + Hello.length() + 1, TonkChannel_Reliable0);
    t_tonk_send.EndCall();

    if (!result)
    {
        Logger.Error("tonk_send 1 failed: ", result.ToString());
        return false;
    }

    t_tonk_send.BeginCall();
    result = connection->Send(PreConnectData, kPreConnectDataBytes, kPreConnectChannel);
    t_tonk_send.EndCall();

    if (!result)
    {
        Logger.Error("tonk_send 2 failed: ", result.ToString());
        return false;
    }

    Logger.Debug("Sent preconnect data");

    t_tonk_flush.BeginCall();
    connection->Flush();
    t_tonk_flush.EndCall();

    return true;
}

bool ValidatePreconnectData(unsigned channel, const uint8_t* data, unsigned bytes)
{
    if (bytes != kPreConnectDataBytes || channel != kPreConnectChannel)
        return false;

    for (unsigned i = 0; i < kPreConnectDataBytes; ++i)
        if (data[i] != PreConnectData[i])
            return false;

    return true;
}


//------------------------------------------------------------------------------
// OWDTimeStatistics

void OWDTimeStatistics::AddSample(uint64_t owdUsec)
{
    if (SampleCount < kMaxSamples)
        ++SampleCount;

    Samples[SampleIndex] = owdUsec;

    // Pick next sample index
    if (++SampleIndex >= kMaxSamples)
        SampleIndex = 0;
}

typedef std::vector<uint64_t>::size_type offset_t;

void OWDTimeStatistics::PrintStatistics()
{
    uint64_t percentile1 = 0;
    if (SampleCount > 200)
    {
        offset_t goalOffset = (offset_t)(0.99 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile1 = Samples[goalOffset];
    }

    uint64_t percentile5 = 0;
    if (SampleCount > 100)
    {
        offset_t goalOffset = (offset_t)(0.95 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile5 = Samples[goalOffset];
    }

    uint64_t percentile25 = 0;
    if (SampleCount > 4)
    {
        offset_t goalOffset = (offset_t)(0.75 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile25 = Samples[goalOffset];
    }

    uint64_t percentile50 = 0;
    if (SampleCount > 4)
    {
        offset_t goalOffset = (offset_t)(0.5 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile50 = Samples[goalOffset];
    }

    uint64_t percentile75 = 0;
    if (SampleCount > 8)
    {
        offset_t goalOffset = (offset_t)(0.25 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile75 = Samples[goalOffset];
    }

    uint64_t percentile95 = 0;
    if (SampleCount > 40)
    {
        offset_t goalOffset = (offset_t)(0.05 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile95 = Samples[goalOffset];
    }

    uint64_t percentile99 = 0;
    if (SampleCount > 200)
    {
        offset_t goalOffset = (offset_t)(0.01 * SampleCount);
        std::nth_element(Samples.begin(), Samples.begin() + goalOffset, Samples.begin() + SampleCount,
            [](uint64_t a, uint64_t b)->bool {
            return a < b;
        });
        percentile99 = Samples[goalOffset];
    }

    Logger.Info("One-way  1% percentile latency = ", percentile1 / 1000.f, " msec");
    Logger.Info("One-way  5% percentile latency = ", percentile5 / 1000.f, " msec");
    Logger.Info("One-way 25% percentile latency = ", percentile25 / 1000.f, " msec");
    Logger.Info("One-way 50% percentile latency = ", percentile50 / 1000.f, " msec (median)");
    Logger.Info("One-way 75% percentile latency = ", percentile75 / 1000.f, " msec");
    Logger.Info("One-way 95% percentile latency = ", percentile95 / 1000.f, " msec");
    Logger.Info("One-way 99% percentile latency = ", percentile99 / 1000.f, " msec");
}


} // namespace test
