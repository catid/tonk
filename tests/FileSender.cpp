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

/**
    FileSender

    This sends files to the FileReceiver.  It sends the file to each client
    that connects.

    It is incompatible with P2PFileReceiver.
*/

#include "tonk.h"
#include "tonk_file_transfer.h"
#include "TonkCppSDK.h"
#include "Logger.h"

#include <stdio.h> // getchar

static logger::Channel Logger("FileSender", logger::Level::Debug);


// Port for FileSender server to listen on
static const uint16_t kFileSenderServerPort = 10333;

// Size of a file chunk in bytes
static const unsigned kFileChunkBytes = 32768;


class MyServer;

class MyServerConnection : public tonk::SDKConnection
{
    MyServer* Server = nullptr;

    TonkFile File = nullptr;
    uint64_t LastReportUsec = 0;

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

    void StartSending();
};

class MyServer : public tonk::SDKSocket
{
public:
    std::string FileSendPath, FileSendName;

    bool Initialize(
        const std::string& fileSendPath,
        const std::string& fileSendName);

    // SDKSocket:
    tonk::SDKConnection* OnIncomingConnection(const TonkAddress& address) override;
    void OnAdvertisement(
        const std::string& ipString, ///< Source IP address of advertisement
        uint16_t               port, ///< Source port address of advertisement
        const uint8_t*         data,  ///< Pointer to a buffer containing the message data
        uint32_t              bytes) override;  ///< Number of bytes in the message
};

void MyServerConnection::StartSending()
{
    File = tonk_file_from_disk(
        GetRawConnection(),
        TonkChannel_LowPri0,
        Server->FileSendName.c_str(),
        Server->FileSendPath.c_str());

    if (File == nullptr) {
        Logger.Error("Failed to map source file ", Server->FileSendPath);
        return;
    }

    Logger.Debug("Successfully mapped ", Server->FileSendPath, " for file data");

    tonk_file_send(File);
}

void MyServerConnection::OnConnect()
{
    auto status = GetStatusEx();

    Logger.Info("Connected to ", status.Remote.NetworkString, " : ", status.Remote.UDPPort);

    StartSending();
}

void MyServerConnection::OnData(
    uint32_t          channel,  // Channel number attached to each message by sender
    const uint8_t*       data,  // Pointer to a buffer containing the message data
    uint32_t            bytes   // Number of bytes in the message
)
{
    if (channel == TonkChannel_Reliable1 && bytes == 9)
    {
        uint8_t pong_message[1 + 8 + 3];
        pong_message[0] = 78;
        for (int i = 0; i < 8; ++i) {
            pong_message[i + 1] = data[i + 1];
        }
        uint32_t remoteTS23 = ToRemoteTime23(tonk_time());
        tonk::WriteU24_LE(pong_message + 1 + 8, remoteTS23);
        Send(pong_message, 1 + 8 + 3, channel);
    }
}

void MyServerConnection::OnTick(
    uint64_t          nowUsec   // Current timestamp in microseconds
)
{
    if (File)
    {
        tonk_file_send(File);

        // If transfer is done we need to call tonk_file_free().
        if (File->Flags & TonkFileFlags_Done)
        {
            if (File->Flags & TonkFileFlags_Failed) {
                Logger.Error("File transfer failed");
            }
            else {
                Logger.Info("File transfer completed");
            }

            tonk_file_free(File);
            File = nullptr;

            // Start sending the next file
            //StartSending();
        }

        // Periodically report progress
        if (File && nowUsec - LastReportUsec > 1000000)
        {
            LastReportUsec = nowUsec;

            Logger.Info("File(", File->Name, ") progress: ", File->ProgressBytes, " / ", File->TotalBytes,
                " Bytes (", File->ProgressBytes * 100 / (float)File->TotalBytes,
                "%).  Received by peer at ", GetStatusEx().PeerSeenBPS / 1000.0, " KBPS");
        }
    }
}

void MyServerConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    auto status = GetStatusEx();

    tonk_file_free(File);

    Logger.Info("Disconnected from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort, " - Reason = ", reason.ToString());
}


bool MyServer::Initialize(
    const std::string& fileSendPath,
    const std::string& fileSendName)
{
    FileSendPath = fileSendPath;
    FileSendName = fileSendName;

    // Set configuration
    Config.UDPListenPort = kFileSenderServerPort;
    Config.MaximumClients = 10;
    //Config.EnableFEC = 0;
    //Config.InterfaceAddress = "127.0.0.1";
    Config.BandwidthLimitBPS = 1000 * 1000 * 1000;

    tonk::SDKJsonResult result = Create();
    if (!result) {
        Logger.Error("Unable to create socket: ", result.ToString());
        return false;
    }

    return true;
}

tonk::SDKConnection* MyServer::OnIncomingConnection(
    const TonkAddress& address ///< Address of the client requesting a connection
)
{
    auto ptr = std::make_shared<MyServerConnection>(this);

    // Keep object alive
    ptr->SetSelfReference();

    return ptr.get();
}

void MyServer::OnAdvertisement(
    const std::string& ipString, ///< Source IP address of advertisement
    uint16_t               port, ///< Source port address of advertisement
    const uint8_t*         data,  ///< Pointer to a buffer containing the message data
    uint32_t              bytes)  ///< Number of bytes in the message
{
    if (bytes != 1) {
        return;
    }

    if (data[0] == 100)
    {
        Logger.Info("Advertisement PING received from ", ipString, ":", port, " - ", bytes, " bytes");

        uint8_t pong[1] = {
            200
        };
        Advertise(ipString, port, pong, 1);
        return;
    }

    if (data[0] == 200)
    {
        Logger.Info("Advertisement PONG received from ", ipString, ":", port, " - ", bytes, " bytes");
        return;
    }
}


// Binary file to send
static const char* kFileSendPath = "sendfile.bin";

// Name at the receiver side
static const char* kFileSendName = "download.bin";

int main(int argc, char** argv)
{
    std::string fileSendPath = kFileSendPath, fileSendName = kFileSendName;

    if (argc >= 2) {
        fileSendPath = argv[1];
        Logger.Info("Using file send path: ", fileSendPath);
    }
    if (argc >= 3) {
        fileSendName = argv[2];
        Logger.Info("Using file send name: ", fileSendName);
    }

    {
        MyServer server;

        if (server.Initialize(fileSendPath, fileSendName)) {
            Logger.Debug("File sending server started on port ", kFileSenderServerPort,
                ".  Now start the file receiver client to receive a file");
        }
        else {
            Logger.Debug("File sending server failed to start");
        }

        Logger.Debug("Press ENTER key to stop the server.");

        ::getchar();
    }

    Logger.Debug("Press ENTER key to terminate.");

    ::getchar();

    Logger.Debug("...Key press detected.  Terminating..");

    return 0;
}
