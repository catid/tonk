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

using namespace test;


#ifdef VERBOSE_LOGS
static logger::Channel Logger("ServerTest", logger::Level::Trace);
#else
static logger::Channel Logger("ServerTest", logger::Level::Debug);
#endif


class MyServer;

class MyServerConnection : public tonk::SDKConnection
{
    int StatusLogCounter = 0;
    bool P2PRequested = false;
    MyServer* Server = nullptr;

    OWDTimeStatistics OWDStats;
    uint64_t LastStatsUsec = 0;
    uint32_t NextOutgoingWord = 0;
    uint32_t NextPacketIndex = 0;

public:
    MyServerConnection(MyServer* server)
        : Server(server)
    {}

    void OnConnect() override;
    void OnData(
        uint32_t          channel,  // Channel number attached to each message by sender
        const uint8_t*       data,  // Pointer to a buffer containing the message data
        uint32_t            bytes   // Number of bytes in the message
    ) override;
    void OnTick(
        uint64_t          nowUsec   // Current timestamp in microseconds
    ) override;
    void OnClose(
        const tonk::SDKJsonResult& reason
    ) override;
};

class MyServer : public tonk::SDKSocket
{
public:
    bool Initialize();

    // SDKSocket:
    tonk::SDKConnection* OnIncomingConnection(const TonkAddress& address) override;
    void OnAdvertisement(
        const std::string& ipString, ///< Source IP address of advertisement
        uint16_t               port, ///< Source port address of advertisement
        const uint8_t*         data,  ///< Pointer to a buffer containing the message data
        uint32_t              bytes) override;  ///< Number of bytes in the message

    // List of connections
    tonk::SDKConnectionList<MyServerConnection> ConnectionList;
};


void MyServerConnection::OnConnect()
{
    LogTonkStatus(this);

    TonkStatus status = GetStatus();

    auto connections = Server->ConnectionList.GetList();
    for (auto& connection : connections)
    {
        TonkStatus cs = connection->GetStatus();

        if (connection.get() != this)
        {
            Logger.Info("Broadcasting connection ", cs.LocallyAssignedIdForRemoteHost,
                " about ", status.LocallyAssignedIdForRemoteHost, " connecting");

            uint8_t data[1 + 4];
            data[0] = ID_ConnectionAdded;
            tonk::WriteU32_LE(data + 1, status.LocallyAssignedIdForRemoteHost);
            connection->Send(data, sizeof(data), TonkChannel_Reliable0);
        }

        uint8_t data[1 + 4];
        data[0] = ID_ConnectionAdded;
        tonk::WriteU32_LE(data + 1, cs.LocallyAssignedIdForRemoteHost);
        Send(data, sizeof(data), TonkChannel_Reliable0);
    }

    // Send two buffers in quick succession in unused channels.
    // The goal is to get the Flush() function to insert a sync
    // frame between these two buffers to test this rare case.
    static const int kBytesA = 100;
    static const int kBytesB = 200;
    uint8_t incompressible_a[kBytesA];
    uint8_t incompressible_b[kBytesB];
    for (int i = 0; i < kBytesA; ++i) {
        incompressible_a[i] = (uint8_t)(i);
    }
    for (int i = 0; i < kBytesB; ++i) {
        incompressible_b[i] = (uint8_t)(kBytesB - i);
    }
    Send(incompressible_a, kBytesA, TonkChannel_LowPri0 + 5);
    Send(incompressible_b, kBytesB, TonkChannel_Reliable0 + 5);
}

void MyServerConnection::OnData(
    uint32_t          channel,  // Channel number attached to each message by sender
    const uint8_t*       data,  // Pointer to a buffer containing the message data
    uint32_t            bytes   // Number of bytes in the message
)
{
    if (bytes <= 0) {
        return;
    }

    if (data[0] == 200)
    {
        static uint8_t expected = 0;
        ++expected;
        if (data[1] != expected)
        {
            //Logger.Info("UNORDERED: ", (int)data[1]);
            expected = data[1];
        }
        return;
    }

    if (data[0] == ID_LowPriBulkData_NoTimestamp)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(channel == kLowPriBulkData_Channel);
        TONK_CPP_SDK_DEBUG_ASSERT(bytes == kLowPriBulkData_Bytes);
        uint32_t magic = tonk::ReadU24_LE(data + 1 + 2 + 3);
        if (magic != kLowPriBulkMagic) {
            Logger.Error("Corrupted data: Bad magic");
        }
        TONK_CPP_SDK_DEBUG_ASSERT(magic == kLowPriBulkMagic);
        uint32_t packetIndex = tonk::ReadU32_LE(data + 1 + 2 + 3 + 3);
        TONK_CPP_SDK_DEBUG_ASSERT(NextPacketIndex == packetIndex);
        if (NextPacketIndex != packetIndex) {
            Logger.Error("Corrupted data: Incorrect packet id");
        }
        NextPacketIndex = packetIndex + 1;
        for (unsigned i = 1 + 2 + 3 + 3 + 4; i < bytes; ++i) {
            TONK_CPP_SDK_DEBUG_ASSERT(data[i] == (uint8_t)i);
        }

        return;
    }

    if (data[0] == ID_LowPriBulkData_HasTimestamp)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(channel == kLowPriBulkData_Channel);
        if (channel != kLowPriBulkData_Channel) {
            Logger.Error("Corrupted data: Wrong channel");
        }
        TONK_CPP_SDK_DEBUG_ASSERT(bytes == kLowPriBulkData_Bytes);
        uint16_t ts16 = tonk::ReadU16_LE(data + 1);
        uint32_t ts23 = tonk::ReadU24_LE(data + 1 + 2);
        uint64_t lo16 = FromLocalTime16(ts16);
        uint64_t lo23 = FromLocalTime23(ts23);
        uint32_t magic = tonk::ReadU24_LE(data + 1 + 2 + 3);
        if (magic != kLowPriBulkMagic) {
            Logger.Error("Corrupted data: Bad magic");
        }
        TONK_CPP_SDK_DEBUG_ASSERT(magic == kLowPriBulkMagic);
        uint32_t packetIndex = tonk::ReadU32_LE(data + 1 + 2 + 3 + 3);
        TONK_CPP_SDK_DEBUG_ASSERT(NextPacketIndex == packetIndex);
        if (NextPacketIndex != packetIndex) {
            Logger.Error("Corrupted data: Incorrect packet id");
        }
        NextPacketIndex = packetIndex + 1;
        for (unsigned i = 1 + 2 + 3 + 3 + 4; i < bytes; ++i)
        {
            TONK_CPP_SDK_DEBUG_ASSERT(data[i] == (uint8_t)i);
        }

        if (0 != (GetStatus().Flags & TonkFlag_TimeSync))
        {
            uint64_t nowUsec = tonk_time();
            uint64_t owdUsec = nowUsec - lo23;
            OWDStats.AddSample(owdUsec);

            if (nowUsec - LastStatsUsec >= kStatsReportIntervalUsec)
            {
                LastStatsUsec = nowUsec;
                Logger.Info("DATA RESULT: Transmission time (acc=16) = ", (int)(nowUsec - lo16), ", (acc=23) = ", (int)(nowUsec - lo23), " # ", packetIndex);
                OWDStats.PrintStatistics();
            }
        }

        return;
    }
    if (data[0] == ID_P2PConnectionStart)
    {
        P2PRequested = true;

        TonkStatus statusBob = GetStatus();

        auto connections = Server->ConnectionList.GetList();
        for (auto& connection : connections)
        {
            // If there is a second connection that also requested P2P:
            if (connection.get() != this && connection->P2PRequested)
            {
                TonkStatus statusAlice = connection->GetStatus();

                Logger.Info("Connecting Alice(", statusAlice.LocallyAssignedIdForRemoteHost,
                    ") and Bob(", statusBob.LocallyAssignedIdForRemoteHost, ")");

                tonk::SDKResult result = this->P2PConnect(connection.get());
                if (result)
                {
                    Logger.Error("P2PConnect failed: ", result.ToString());
                    continue;
                }

                break;
            }
        }
        return;
    }
    if (data[0] == ID_ConnectionRebroadcast)
    {
        TonkStatus status = GetStatus();

        const char* str = (char*)data + 1;
        Logger.Info("Rebroadcast request from ", status.LocallyAssignedIdForRemoteHost, " : '", str, "'");

        uint8_t msg[1400];
        msg[0] = ID_ConnectionRebroadcast;
        tonk::WriteU24_LE_Min4Bytes(msg + 1, status.LocallyAssignedIdForRemoteHost);
        memcpy(msg + 1 + 3, data + 1, bytes - 1);

        auto connections = Server->ConnectionList.GetList();
        for (auto& connection : connections)
        {
            // If there is a second connection that also requested P2P:
            if (connection.get() != this) {
                connection->Send(msg, 1 + 3 + bytes, TonkChannel_Reliable0 + 1);
            }
        }
        return;
    }
    if (data[0] == ID_PreConnectDataTest)
    {
        Logger.Debug("Got pre-connect data test");

        const bool validated = ValidatePreconnectData(channel, data, bytes);

        if (!validated)
        {
            Logger.Error("ERROR: Preconnect data was invalid!");
            TONK_CPP_SDK_DEBUG_BREAK();
        }
        return;
    }

    Logger.Error("MyServerConnection: Got ", bytes, " bytes of message data unexpected type ", (int)data[0]);
    TONK_CPP_SDK_DEBUG_BREAK();
}

void MyServerConnection::OnTick(
    uint64_t          nowUsec   // Current timestamp in microseconds
)
{
    if (++StatusLogCounter >= 100)
    {
        StatusLogCounter = 0;
        LogTonkStatus(this);
    }

    TonkStatus status = GetStatus();

    {
        static uint8_t counter = 0;
        uint8_t data[2] = { 200, ++counter };
        Send(data, sizeof(data), TonkChannel_Unordered);
    }

#ifdef TONK_ENABLE_1MBPS_DATA_SERVER
    if (status.LowPriQueueMsec < kLowPriQueueDepthMsec)
    {
        unsigned fillMsec = kLowPriQueueDepthMsec - status.LowPriQueueMsec;
        unsigned bytes = (unsigned)(((uint64_t)status.AppBPS * fillMsec) / 1000);
        unsigned packetsToSend = (bytes + kLowPriBulkData_Bytes - 1) / kLowPriBulkData_Bytes;
        for (unsigned i = 0; i < packetsToSend; ++i)
        {
            uint8_t data[kLowPriBulkData_Bytes] = {};
            for (unsigned i = 0; i < kLowPriBulkData_Bytes; ++i) {
                data[i] = (uint8_t)i;
            }

            const uint16_t ts16 = ToRemoteTime16(nowUsec);
            const uint32_t ts23 = ToRemoteTime23(nowUsec);

            data[0] = (status.Flags & TonkFlag_TimeSync) ? ID_LowPriBulkData_HasTimestamp : ID_LowPriBulkData_NoTimestamp;
            tonk::WriteU16_LE(data + 1, ts16);
            tonk::WriteU24_LE(data + 1 + 2, ts23);
            tonk::WriteU24_LE(data + 1 + 2 + 3, kLowPriBulkMagic);
            tonk::WriteU32_LE(data + 1 + 2 + 3 + 3, NextOutgoingWord++);

            t_tonk_send.BeginCall();
            Send(data, sizeof(data), kLowPriBulkData_Channel);
            t_tonk_send.EndCall();
        }
    }
#endif
}

void MyServerConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    Logger.Info("MyServerConnection: OnClose(", reason.ToString(), ")");

    LogTonkStatus(this);

    TonkStatus status = GetStatus();

    auto connections = Server->ConnectionList.GetList();
    for (auto& connection : connections)
    {
        TonkStatus cs = connection->GetStatus();
        Logger.Info("Informing connection ", cs.LocallyAssignedIdForRemoteHost, " about ", status.LocallyAssignedIdForRemoteHost, " disconnecting");

        uint8_t data[1 + 4];
        data[0] = ID_ConnectionRemoved;
        tonk::WriteU32_LE(data + 1, status.LocallyAssignedIdForRemoteHost);
        connection->Send(data, sizeof(data), TonkChannel_Reliable0);
    }

    Server->ConnectionList.Remove(this);
}


bool MyServer::Initialize()
{
    // Set configuration
    Config.UDPListenPort = kUDPServerPort_Server;
    Config.MaximumClients = 10;
    Config.Flags = TONK_FLAGS_ENABLE_UPNP;
    Config.Flags |= TONK_FLAGS_DISABLE_COMPRESSION;
    Config.MinuteFloodThresh = 2;
    Config.BandwidthLimitBPS = 4000000;
    //Config.InterfaceAddress = "127.0.0.1";

    tonk::SDKJsonResult result = Create();
    if (!result) {
        Logger.Error("Unable to create socket: ", result.ToString());
    }

    return result;
}

tonk::SDKConnection* MyServer::OnIncomingConnection(const TonkAddress& /*address*/)
{
    auto shared = std::make_shared<MyServerConnection>(this);

    // Insert into connection list to prevent it from going out of scope
    ConnectionList.Insert(shared.get());

    return shared.get();
}

void MyServer::OnAdvertisement(
    const std::string& ipString, ///< Source IP address of advertisement
    uint16_t               port, ///< Source port address of advertisement
    const uint8_t*         data,  ///< Pointer to a buffer containing the message data
    uint32_t              bytes)  ///< Number of bytes in the message
{
    if (bytes == test::kPongBytes)
    {
        if (data[0] == test::ID_Advertise_Ping)
        {
            Logger.Info("Advertisement PING received from ", ipString, ":", port, " - ", bytes, " bytes");

            uint8_t pong[test::kPongBytes] = {
                test::ID_Advertise_Pong, 1, 2, 3, 4
            };
            Advertise(ipString, port, pong, test::kPongBytes);
            return;
        }
        if (data[0] == test::ID_Advertise_Pong)
        {
            Logger.Info("Advertisement PONG received from ", ipString, ":", port, " - ", bytes, " bytes");
            return;
        }
    }

    Logger.Error("Unknown advertisement received from ", ipString, ":", port, " - ", bytes, " bytes");
}


#include <iostream> // cin

int main()
{
    {
        MyServer server;
        if (server.Initialize())
        {
            Logger.Debug("Press ENTER key to stop server. Type something in and press ENTER to send an advertisement broadcast");

            for (;;)
            {
                char cline[512];
                if (!std::cin.getline(cline, sizeof(cline))) {
                    break;
                }
                std::string line = cline;
                if (line.empty()) {
                    break;
                }

                // Send ping
                uint8_t ping[kPongBytes] = {
                    ID_Advertise_Ping, 1, 2, 3, 4
                };
                tonk::SDKResult result = server.Advertise(TONK_BROADCAST, kUDPServerPort_Server, ping, kPongBytes);
                if (result) {
                    Logger.Info("Sent advertisement");
                }
                else
                {
                    TONK_CPP_SDK_DEBUG_BREAK();
                    Logger.Error("Advertise failed: ", result.ToString());
                }
            }

            Logger.Debug("...ENTER press detected.  Stopping..");
        }
    }

    Logger.Info("Test complete.  *** Timing results:");

    t_tonk_socket_create.Print();
    t_tonk_socket_destroy.Print();
    t_tonk_send.Print();
    t_tonk_flush.Print();
    t_tonk_free.Print();

    Logger.Debug("Press ENTER key to terminate");

    logger::Flush();

    ::getchar();
    Logger.Debug("...Key press detected.  Terminating..");

    logger::Flush();

    return 0;
}
