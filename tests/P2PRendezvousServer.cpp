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
    P2PRendezvousServer

    Rendezvous server for the P2PFileSender and P2PFileReceiver.
*/

#include "tonk.h"
#include "tonk_file_transfer.h"
#include "TonkCppSDK.h"
#include "Logger.h"

#include <stdio.h> // getchar

static logger::Channel Logger("P2PRendezvousServer", logger::Level::Debug);


// Port for P2PFileRendezvousServer to listen on
static const uint16_t kFileRendezvousServerPort = 10334;


//------------------------------------------------------------------------------
// Classes

class MyServer;

class MyServerConnection : public tonk::SDKConnection
{
    MyServer* Server = nullptr;

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
    void OnClose(
        const tonk::SDKJsonResult& reason
    ) override;

    std::atomic<bool> IsReceiver = ATOMIC_VAR_INIT(false);
    std::atomic<bool> IsSender = ATOMIC_VAR_INIT(false);
};

class MyServer : public tonk::SDKSocket
{
public:
    bool Initialize();

    // SDKSocket:
    tonk::SDKConnection* OnIncomingConnection(const TonkAddress& address) override;

    // List of connections
    tonk::SDKConnectionList<MyServerConnection> ConnectionList;
};


//------------------------------------------------------------------------------
// MyServerConnection

void MyServerConnection::OnConnect()
{
    auto status = GetStatusEx();

    Logger.Info("Client connected from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort);
}

void MyServerConnection::OnData(
    uint32_t          channel,  // Channel number attached to each message by sender
    const uint8_t*       data,  // Pointer to a buffer containing the message data
    uint32_t            bytes   // Number of bytes in the message
)
{
    if (channel == TonkChannel_Reliable0 && bytes == 1)
    {
        auto status = GetStatusEx();

        if (data[0] == 1) {
            Logger.Info("Receiver from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort);
            IsReceiver = true;
        }
        else if (data[0] == 0) {
            Logger.Info("Sender from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort);
            IsSender = true;
        }

        std::shared_ptr<MyServerConnection> sender, receiver;

        auto connections = Server->ConnectionList.GetList();
        for (auto& connection : connections)
        {
            if (connection->IsReceiver) {
                receiver = connection;
            }
            else if (connection->IsSender) {
                sender = connection;
            }
        }

        if (sender && receiver)
        {
            auto result = receiver->P2PConnect(sender.get());

            if (result.Good())
            {
                Logger.Info("P2P connection started between ",
                    sender->GetStatusEx().Remote.NetworkString, " : ",
                    sender->GetStatusEx().Remote.UDPPort, " <-> ",
                    receiver->GetStatusEx().Remote.NetworkString, " : ",
                    receiver->GetStatusEx().Remote.UDPPort);
            }
            else
            {
                Logger.Warning("P2P connection failed: ", result.ToString());
            }
        }
    }
}

void MyServerConnection::OnClose(
    const tonk::SDKJsonResult& reason
)
{
    auto status = GetStatusEx();

    Logger.Info("Client disconnected from ", status.Remote.NetworkString, " : ", status.Remote.UDPPort, " - Reason = ", reason.ToString());

    Server->ConnectionList.Remove(this);
}


//------------------------------------------------------------------------------
// MyServer

bool MyServer::Initialize()
{
    // Set configuration
    Config.UDPListenPort = kFileRendezvousServerPort;
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

    // Insert into connection list to prevent it from going out of scope
    ConnectionList.Insert(ptr.get());

    return ptr.get();
}


//------------------------------------------------------------------------------
// Entrypoint

int main(int argc, char** argv)
{
    {
        MyServer server;

        if (server.Initialize()) {
            Logger.Debug("File rendezvous server started on port ", kFileRendezvousServerPort,
                ".  Now start the file sender/receiver client to receive a file");
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
