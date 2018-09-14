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
    P2PFileSender

    This sends files to the P2PFileReceiver by connecting to the
    P2PFileRendezvousServer, and then creating a peer2peer connection
    with the P2PFileReceiver.  It sends the file to each client
    that connects.

    It is incompatible with FileReceiver.
*/

#include "tonk.h"
#include "tonk_file_transfer.h"
#include "TonkCppSDK.h"
#include "Logger.h"

#include <stdio.h> // getchar

static logger::Channel Logger("P2PFileSender", logger::Level::Debug);


// Port for P2PFileRendezvousServer to listen on
static const uint16_t kFileRendezvousServerPort = 10334;

// Size of a file chunk in bytes
static const unsigned kFileChunkBytes = 32768;


//------------------------------------------------------------------------------
// Classes

class P2PFileSender;

class FileSenderServerConnection : public tonk::SDKConnection
{
    P2PFileSender* Sender = nullptr;

public:
    FileSenderServerConnection(P2PFileSender* sender)
        : Sender(sender)
    {}

    void OnConnect() override;
    void OnClose(
        const tonk::SDKJsonResult& reason
    ) override;
};

class P2PFileSenderConnection : public tonk::SDKConnection
{
    P2PFileSender* Sender = nullptr;

    TonkFile File = nullptr;
    uint64_t LastReportUsec = 0;

public:
    P2PFileSenderConnection(P2PFileSender* sender)
        : Sender(sender)
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

class P2PFileSender : public tonk::SDKSocket
{
public:
    std::string FileSendPath, FileSendName;

    bool Initialize(
        const std::string& fileSendPath,
        const std::string& fileSendName);

    // SDKSocket:
    virtual tonk::SDKConnection* OnP2PConnectionStart(
        const TonkAddress& address ///< Address of the other peer we are to connect to
    ) override;
};


//------------------------------------------------------------------------------
// FileSenderServerConnection

void FileSenderServerConnection::OnConnect()
{
    Logger.Info("Rendezvous server connected: ", GetStatusEx().Remote.NetworkString);

    uint8_t msg[1];
    msg[0] = 0; // I'm a sender
    Send(msg, 1, TonkChannel_Reliable0);
}

void FileSenderServerConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    Logger.Info("Rendezvous server connection closed: ",
        GetStatusEx().Remote.NetworkString, ", Reason: ", reason.ToString());
}


//------------------------------------------------------------------------------
// P2PFileSenderConnection

void P2PFileSenderConnection::StartSending()
{
    File = tonk_file_from_disk(
        GetRawConnection(),
        TonkChannel_LowPri0,
        Sender->FileSendName.c_str(),
        Sender->FileSendPath.c_str());

    if (File == nullptr) {
        Logger.Error("Failed to map source file ", Sender->FileSendPath);
        return;
    }

    Logger.Debug("Successfully mapped ", Sender->FileSendPath, " for file data");

    tonk_file_send(File);
}

void P2PFileSenderConnection::OnConnect()
{
    auto status = GetStatusEx();

    Logger.Info("P2P Connected to ", status.Remote.NetworkString, " : ", status.Remote.UDPPort);

    StartSending();
}

void P2PFileSenderConnection::OnData(
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

void P2PFileSenderConnection::OnTick(
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

void P2PFileSenderConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    auto status = GetStatusEx();

    tonk_file_free(File);

    Logger.Info("P2P Disconnected from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort, " - Reason = ", reason.ToString());
}


//------------------------------------------------------------------------------
// P2PFileSender

bool P2PFileSender::Initialize(
    const std::string& fileSendPath,
    const std::string& fileSendName)
{
    FileSendPath = fileSendPath;
    FileSendName = fileSendName;

    // Set configuration
    //Config.UDPListenPort = kFileSenderServerPort;
    Config.MaximumClients = 10;
    Config.Flags = TONK_FLAGS_ENABLE_UPNP;
    //Config.EnableFEC = 0;
    //Config.InterfaceAddress = "127.0.0.1";
    //Config.BandwidthLimitBPS = 1000 * 1000 * 1000;

    tonk::SDKJsonResult result = Create();
    if (!result) {
        Logger.Error("Unable to create socket: ", result.ToString());
        return false;
    }

    return true;
}

tonk::SDKConnection* P2PFileSender::OnP2PConnectionStart(
    const TonkAddress& address ///< Address of the other peer we are to connect to
)
{
    auto ptr = std::make_shared<P2PFileSenderConnection>(this);

    // Keep object alive
    ptr->SetSelfReference();

    return ptr.get();
}


//------------------------------------------------------------------------------
// Entrypoint

// Binary file to send
static const char* kFileSendPath = "sendfile.bin";

// Name at the receiver side
static const char* kFileSendName = "download.bin";

int main(int argc, char** argv)
{
    std::string serverHostname = "127.0.0.1", fileSendPath = kFileSendPath, fileSendName = kFileSendName;

    if (argc >= 2) {
        serverHostname = argv[1];
        Logger.Info("Using server hostname: ", serverHostname);
    }
    if (argc >= 3) {
        fileSendPath = argv[2];
        Logger.Info("Using file send path: ", fileSendPath);
    }
    if (argc >= 4) {
        fileSendName = argv[3];
        Logger.Info("Using file send name: ", fileSendName);
    }
    else
    {
        const char* exe = (argc > 0 ? argv[0] : "P2PFileSender");
        Logger.Info("Usage: ", exe, " <Rendezvous Server Hostname> <File to Send> <Remote File Name>");
    }

    {
        P2PFileSender sender;

        auto connection = std::make_shared<FileSenderServerConnection>(&sender);

        if (sender.Initialize(fileSendPath, fileSendName)) {
            Logger.Debug("File sending server started on port ", kFileRendezvousServerPort,
                ".  Now start the file receiver client to receive a file");

            auto result = sender.Connect(connection.get(), serverHostname, kFileRendezvousServerPort);

            if (result.Good())
            {
                Logger.Info("Connection started");
            }
            else {
                Logger.Warning("Connection failed to start: ", result.ToString());
            }
        }
        else {
            Logger.Warning("File sending server failed to start");
        }

        Logger.Debug("Press ENTER key to stop the server.");

        ::getchar();
    }

    Logger.Debug("Press ENTER key to terminate.");

    ::getchar();

    Logger.Debug("...Key press detected.  Terminating..");

    return 0;
}
