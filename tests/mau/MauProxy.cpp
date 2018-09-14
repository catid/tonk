/** \file
    \brief Mau Implementation: Proxy Session
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Mau nor the names of its contributors may be
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

#include "MauProxy.h"

namespace mau {

static logger::Channel ModuleLogger("Proxy", MinimumLogLevel);


//------------------------------------------------------------------------------
// PacketQueue

void PacketQueue::InsertSorted(QueueNode* node)
{
    const uint64_t deliveryUsec = node->DeliveryUsec;

    QueueNode* next = nullptr;

    // Insert at front or middle (based on target delivery time):
    for (QueueNode* prev = Tail; prev; next = prev, prev = prev->Prev)
    {
        // If we should insert after prev:
        if (deliveryUsec >= prev->DeliveryUsec)
        {
            if (prev) {
                prev->Next = node;
            }
            else {
                Head = node;
            }
            if (next) {
                next->Prev = node;
            }
            else {
                Tail = node;
            }
            node->Next = next;
            node->Prev = prev;

            return;
        }
    }

    // Insert at the front:
    Head = node;
    if (next) {
        next->Prev = node;
    }
    else {
        Tail = node;
    }
    node->Next = next;
    node->Prev = nullptr;
}


//------------------------------------------------------------------------------
// DeliveryChannel

bool DeliveryChannel::Initialize(DeliveryCommonData* common)
{
    Common = common;

    uint64_t seed = Common->ChannelConfig.Get().RNGSeed;
    if (seed == 0) {
        seed = GetRandomSeed();
    }
    LossRNG.Seed(seed);

    DeliveryTimer = MakeUniqueNoThrow<asio::steady_timer>(*common->Context);
    if (!DeliveryTimer) {
        return false;
    }

    return true;
}

void DeliveryChannel::Shutdown()
{
    if (DeliveryTimer) {
        DeliveryTimer->cancel();
    }
    DeliveryTimer = nullptr;
}

void DeliveryChannel::SetDeliveryAddress(const UDPAddress& addr)
{
    DeliveryAddress.Set(addr);
}

void DeliveryChannel::InsertQueueNode(QueueNode* node)
{
    /*
        Client <-> Internet Delay <-> ISP Router Queue <-> Server

        Most queues are going to be on the receiver ISP side, for example
        on the cellphone network or cable ISP.  The Internet transmission
        delay comes before that.  Since this is the most common case for
        the protocols we are testing, we use the simplified model above.

        In this model, packets put into the queue are subjected to:
        (1) Internet Delay
        (2) ISP Router Queue

        The Internet Delay may vary on route changes.

        The Internet Service Provider (ISP) Router Queue causes
        delays and packet loss if the queue is too full.
    */

    MauChannelConfig config = Common->ChannelConfig.Get();
    bool dupe = false;

    // Hold InsertLock while processing insert
    {
        Locker locker(InsertLock);

        const uint64_t nowUsec = GetTimeUsec();

        const unsigned routerQueueUsec = config.Router_QueueMsec * 1000;
        const uint64_t routerQueueStartUsec = nowUsec + config.LightSpeedMsec * 1000;
        const unsigned dataDelayUsec = (unsigned)(node->Bytes / config.Router_MBPS);

        uint64_t deliveryUsec = 0;
        float queueFraction = 0.f;

        // Calculate delivery time based on router queue
        if (NextQueueSlotUsec < routerQueueStartUsec) {
            deliveryUsec = routerQueueStartUsec + dataDelayUsec;
        }
        else {
            deliveryUsec = NextQueueSlotUsec + dataDelayUsec;

            // DropTail if packet does not fit in router queue
            if (deliveryUsec >= routerQueueStartUsec + routerQueueUsec) {
                Common->ReadBufferAllocator.Free(node);
                return;
            }

            queueFraction = (deliveryUsec - routerQueueStartUsec) / (float)routerQueueUsec;
        }

        // If RED is triggered:
        if (config.Router_RED_QueueFraction != 0.f && queueFraction > config.Router_RED_QueueFraction)
        {
            // Rescale:
            const float red_plr = config.Router_RED_MaxPLR * (queueFraction - config.Router_RED_QueueFraction) / (1.f - config.Router_RED_QueueFraction);

            // If RED drop is needed:
            if (LossRNG.Next() < (uint32_t)(0xffffffff * red_plr)) {
                Common->ReadBufferAllocator.Free(node);
                return;
            }
        }

        // If duplicating:
        if (LossRNG.Next() < (uint32_t)(0xffffffff * config.DuplicateRate)) {
            dupe = true;
        }

        float reorderRate;
        if (InBurstReorder) {
            reorderRate = 1.f - config.OrderRate;
        }
        else {
            reorderRate = config.ReorderRate;
        }

        // If this packet will be reordered:
        unsigned reorderMsec = 0;
        if (LossRNG.Next() < (uint32_t)(0xffffffff * reorderRate))
        {
            InBurstReorder = true;
            reorderMsec = config.ReorderMinimumLatencyMsec;
            const unsigned range = config.ReorderMaximumLatencyMsec - config.ReorderMinimumLatencyMsec;
            if (range > 0) {
                reorderMsec += LossRNG.Next() % range;
            }
        }
        else {
            InBurstReorder = false;
        }

        // Select loss rate for this packet due to Gilbert-Elliott channel model
        float lossRate;
        if (InBurstLoss) {
            lossRate = 1.f - config.DeliveryRate;
        }
        else {
            lossRate = config.LossRate;
        }

        // If this packet will be lost:
        if (LossRNG.Next() < (uint32_t)(0xffffffff * lossRate)) {
            InBurstLoss = true;
            Common->ReadBufferAllocator.Free(node);
            return;
        }
        InBurstLoss = false; // Burst loss is done

        // If this packet will be corrupted:
        if (LossRNG.Next() < (uint32_t)(0xffffffff * config.CorruptionRate)) {
            node->Data[LossRNG.Next() % node->Bytes] ^= 8;
        }

        // If re-ordered:
        if (reorderMsec > 0) {
            // Add reorder time to it
            // Note: Bypasses queue so will not be counted towards channel bandwidth
            deliveryUsec += reorderMsec * 1000;
        }
        else {
            // It takes up a queue slot, so advance the queue
            NextQueueSlotUsec = deliveryUsec;
        }

        node->DeliveryUsec = deliveryUsec;
    }

    // Hold QueueLock during queue insert
    {
        Locker locker(QueueLock);
        Queue.InsertSorted(node);
    }

    if (dupe)
    {
        // Allocate space for the duplicate
        uint8_t* readBuffer = Common->ReadBufferAllocator.Allocate(kQueueHeaderSize + node->Bytes);

        QueueNode* dupeNode = reinterpret_cast<QueueNode*>(readBuffer);

        // Copy the data
        dupeNode->Bytes = node->Bytes;
        memcpy(&dupeNode->Data[0], &node->Data[0], node->Bytes);

        InsertQueueNode(dupeNode);
    }

    postNextTimer();
}

void DeliveryChannel::postNextTimer()
{
    uint64_t nextTimerWakeUsec = 0;

    {
        Locker locker(QueueLock);

        // Read next target wake time
        QueueNode* frontNode = Queue.Peek();
        if (!frontNode) {
            // No data in the queue
            return;
        }

        nextTimerWakeUsec = frontNode->DeliveryUsec;

        // If we are already waiting for an earlier (or same) time:
        if (NextTimerWakeUsec != 0 &&
            (int64_t)(nextTimerWakeUsec - NextTimerWakeUsec) >= 0)
        {
            // Current wake-up time is sooner
            return;
        }

        NextTimerWakeUsec = nextTimerWakeUsec;
    }

    // Calculate when the timer should fire
    uint64_t aheadUsec = 0;
    const uint64_t nowUsec = GetTimeUsec();
    const int64_t deltaUsec = (int64_t)(nextTimerWakeUsec - nowUsec);
    if (deltaUsec > 0) {
        aheadUsec = deltaUsec;
    }

    //Common->Logger.Info("Waiting for ", aheadUsec, " usec");

    // Update timer to new time
    DeliveryTimer->expires_after(std::chrono::microseconds(aheadUsec));

    DeliveryTimer->async_wait(([this](const asio::error_code& error)
    {
        // Clear the next wakeup time to allow the next one to be set
        // concurrently (see above)
        {
            Locker locker(QueueLock);
            NextTimerWakeUsec = 0;
        }

        if (error)
        {
            // Do not report errors here.
            // They happen normally as we cancel and restart the timers
            // to wake them up sooner
            //ModuleLogger.Warning("DeliveryChannel timer error: ", error.message());
            return;
        }

        UDPAddress destAddress = DeliveryAddress.Get();
        if (destAddress.address().is_unspecified())
        {
            Common->Logger.Info("Delivery address unspecified - Abort");
            return; // Still waiting for hostname resolution
        }

        QueueNode* node;
        const uint64_t nowUsec = GetTimeUsec();

        for (;;)
        {
            {
                Locker locker(QueueLock);

                node = Queue.Peek();
                if (!node) {
                    break;
                }

                if ((int64_t)(nowUsec - node->DeliveryUsec) < 0) {
                    break;
                }

                Queue.Pop();
            }

            //Common->Logger.Info("Forwarding at ", node->TargetDeliveryUsec, " off by ", (int64_t)(nowUsec - node->TargetDeliveryUsec));

            void* nodeData = &node->Data[0];

            if (Common->ProxyConfig.SendHook)
            {
                Common->ProxyConfig.SendHook(
                    Common->ProxyConfig.SendHookContext,
                    destAddress.port(),
                    nodeData,
                    node->Bytes
                );

                Common->ReadBufferAllocator.Free(node);
            }
            else
            {
                Common->Socket->async_send_to(
                    asio::buffer(nodeData, node->Bytes),
                    destAddress, [this, node](const asio::error_code&, std::size_t)
                {
                    Common->ReadBufferAllocator.Free(node);
                });
            }
        }

        postNextTimer();
    }));
}


//------------------------------------------------------------------------------
// ProxySession

DeliveryCommonData::DeliveryCommonData()
    : Logger("MauProxy", MinimumLogLevel)
{
}

static asio::ip::address firstIPv4AddressInResults(
    const asio::ip::tcp::resolver::results_type& results)
{
    for (auto& result : results)
    {
        auto addr = result.endpoint().address();
        if (addr.is_v4()) {
            return addr;
        }

        try
        {
            // Workaround fix: IPv6 loopback is not handled properly by asio to_v4()
            if (addr.is_loopback()) {
                return asio::ip::address_v4::loopback();
            }

            return addr.to_v4();
        }
        catch (...)
        {
        }
    }

    return asio::ip::address();
}

MauResult ProxySession::Initialize(
    const char* serverHostname,
    uint16_t serverPort,
    const MauProxyConfig& proxyConfig,
    const MauChannelConfig& channelConfig)
{
    MauResult result = Mau_Success;

    try
    {
        ProxyConfig = proxyConfig;
        ChannelConfig.Set(channelConfig);
        ServerHostname = serverHostname;
        ServerPort = serverPort;

        // Set logger prefix
        std::ostringstream oss;
        oss << "[Port " << std::to_string(proxyConfig.UDPListenPort) << "] ";
        Logger.SetPrefix(oss.str());

        Context = MakeSharedNoThrow<asio::io_context>();
        if (!Context)
        {
            result = Mau_OOM;
            MAU_DEBUG_BREAK();
            goto OnError;
        }
        Context->restart();

        // Create Asio objects
        Ticker = MakeUniqueNoThrow<asio::steady_timer>(*Context);
        Socket = MakeUniqueNoThrow<asio::ip::udp::socket>(*Context);
        Resolver = MakeUniqueNoThrow<asio::ip::tcp::resolver>(*Context);
        if (!Socket || !Resolver || !Ticker)
        {
            result = Mau_OOM;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        postNextTimer();

        // Initialize S2C channel
        if (!S2C.Initialize(this))
        {
            result = Mau_OOM;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        // Initialize C2S channel
        if (!C2S.Initialize(this))
        {
            result = Mau_OOM;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        // Create an IPv4/IPv6 socket based on the server address type
        const UDPAddress bindAddress(asio::ip::udp::v4(), ProxyConfig.UDPListenPort);

        asio::error_code error;
        Socket->open(bindAddress.protocol(), error);
        if (error)
        {
            result = Mau_NetworkFailed;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        Socket->bind(bindAddress, error);
        if (error)
        {
            result = Mau_PortInUse;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        if (!SetSocketOptions(
            Socket,
            ProxyConfig.UDPSendBufferSizeBytes,
            ProxyConfig.UDPRecvBufferSizeBytes))
        {
            result = Mau_SocketCreation;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        Thread = MakeUniqueNoThrow<std::thread>(&ProxySession::workerLoop, this);
        if (!Thread)
        {
            result = Mau_OOM;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        uint8_t* readBuffer = ReadBufferAllocator.Allocate(kQueueHeaderSize + ProxyConfig.MaxDatagramBytes);
        if (!readBuffer)
        {
            result = Mau_OOM;
            MAU_DEBUG_BREAK();
            goto OnError;
        }

        postNextRead(readBuffer);

        // Allow any protocol
        static const std::string kAnyProtocolStr = "";

        Resolver->async_resolve(std::string(serverHostname), kAnyProtocolStr,
            [this](const asio::error_code& error, const asio::ip::tcp::resolver::results_type& results)
        {
            MAU_UNUSED(error);
            asio::ip::address addr = firstIPv4AddressInResults(results);

            if (addr.is_unspecified())
            {
                Logger.Error("Unable to resolve server hostname: ", ServerHostname);
                LastResult = Mau_HostnameLookup;
            }
            else
            {
                const UDPAddress serverAddress(addr, ServerPort);
                ServerAddress.Set(serverAddress);
                C2S.SetDeliveryAddress(serverAddress);

                Logger.Info("Resolved server address to ", serverAddress.address().to_string(), " : ", serverAddress.port());
            }
        });
    }
    catch (...)
    {
        result = Mau_Error;
        goto OnError;
    }

OnError:
    LastResult = result;
    if (MAU_FAILED(result))
    {
        Logger.Error("ProxySession::Initialize failed: ", result);
        Shutdown();
    }

    return result;
}

void ProxySession::postNextTimer()
{
    static const unsigned kTickIntervalUsec = 1000 * 1000; /// 1 second
    Ticker->expires_after(std::chrono::microseconds(kTickIntervalUsec));

    Ticker->async_wait(([this](const asio::error_code& error)
    {
        if (!error)
        {
            onTick();
            postNextTimer();
        }
    }));
}

void ProxySession::onTick()
{
}

MauResult ProxySession::Inject(
    uint16_t sourcePort, ///< [in] Source port
    const void* datagram, ///< [in] Datagram buffer
    unsigned bytes) ///< [in] Datagram bytes
{
    Locker locker(APILock);

    if (Terminated) {
        return Mau_Success;
    }

    if (bytes > ProxyConfig.MaxDatagramBytes) {
        MAU_DEBUG_BREAK();
        return Mau_InvalidInput;
    }
    uint8_t* readBuffer = ReadBufferAllocator.Allocate(kQueueHeaderSize + bytes);

    QueueNode* queueNode = reinterpret_cast<QueueNode*>(readBuffer);
    queueNode->Bytes = bytes;
    memcpy(&queueNode->Data[0], datagram, bytes);

    const UDPAddress serverAddress = ServerAddress.Get();
    if (sourcePort == serverAddress.port()) {
        S2C.InsertQueueNode(queueNode);
    }
    else
    {
        UDPAddress clientAddress = ClientAddress.Get();
        if (clientAddress.port() == 0)
        {
            clientAddress = UDPAddress(asio::ip::address_v4::loopback(), sourcePort);
            ClientAddress.Set(clientAddress);
            S2C.SetDeliveryAddress(clientAddress);

            C2S.InsertQueueNode(queueNode);

            Logger.Debug("Inject: Set client address to ", clientAddress.address().to_string(), " : ", clientAddress.port());
        }
        else if (clientAddress.port() == sourcePort) {
            C2S.InsertQueueNode(queueNode);
        }
        else {
            Logger.Warning("Inject: Ignored data from unrecognized source address ", clientAddress.address().to_string(), " : ", clientAddress.port());
        }
    }

    return Mau_Success;
}

void ProxySession::postNextRead(uint8_t* readBuffer)
{
    // Clear source address to allow us to check if it's filled later
    // during error handling
    SourceAddress = UDPAddress();

    // Dispatch asynchronous recvfrom()
    Socket->async_receive_from(
        asio::buffer(readBuffer + kQueueHeaderSize, ProxyConfig.MaxDatagramBytes),
        SourceAddress,
        [this, readBuffer](const asio::error_code& error, size_t bytes_transferred)
    {
        uint8_t* nextReadBuffer = readBuffer;

        // If there was an error reported:
        if (error)
        {
            std::string fromStr = "unrecognized";

            // If no source address was provided:
            if (SourceAddress.address().is_unspecified())
            {
                Logger.Warning("Socket broken based on recvfrom with no source address: ", error.message());
                LastResult = Mau_NetworkFailed;
                return; // Stop here
            }

            // Otherwise we assume it is due to ICMP message from a peer:

            if (SourceAddress == ClientAddress.Get())
            {
                Logger.Debug("recvfrom failed (ICMP from client): ", error.message());
                ClientAddress.Set(UDPAddress());
            }
            else if (SourceAddress == ServerAddress.Get())
            {
                Logger.Debug("recvfrom failed (ICMP from server): ", error.message());
                ClientAddress.Set(UDPAddress());
            }
            else {
                Logger.Debug("recvfrom failed (ICMP unrecognized source): ", error.message());
            }

            // Otherwise we assume it is due to ICMP message from a peer.
            // Fall-thru to continue reading datagrams
        }
        else if (bytes_transferred <= 0)
        {
            Logger.Warning("Socket closed");
            LastResult = Mau_NetworkFailed;
            return; // Stop here
        }
        else if (!Terminated)
        {
            MAU_DEBUG_ASSERT(bytes_transferred <= ProxyConfig.MaxDatagramBytes);

            QueueNode* queueNode = reinterpret_cast<QueueNode*>(readBuffer);
            queueNode->Bytes = static_cast<unsigned>(bytes_transferred);

            if (SourceAddress == ServerAddress.Get())
            {
                S2C.InsertQueueNode(queueNode);
                nextReadBuffer = ReadBufferAllocator.Allocate(kQueueHeaderSize + ProxyConfig.MaxDatagramBytes);
            }
            else
            {
                const UDPAddress clientAddress = ClientAddress.Get();

                if (clientAddress.address().is_unspecified())
                {
                    ClientAddress.Set(SourceAddress);
                    S2C.SetDeliveryAddress(SourceAddress);

                    C2S.InsertQueueNode(queueNode);
                    nextReadBuffer = ReadBufferAllocator.Allocate(kQueueHeaderSize + ProxyConfig.MaxDatagramBytes);

                    Logger.Info("Set client address to ", SourceAddress.address().to_string(), " : ", SourceAddress.port());
                }
                else if (SourceAddress == clientAddress)
                {
                    C2S.InsertQueueNode(queueNode);
                    nextReadBuffer = ReadBufferAllocator.Allocate(kQueueHeaderSize + ProxyConfig.MaxDatagramBytes);
                }
                else {
                    Logger.Warning("Ignored data from unrecognized source address ", SourceAddress.address().to_string(), " : ", SourceAddress.port());
                }
            }
        }

        // Post next read buffer
        postNextRead(nextReadBuffer);
    });
}

void ProxySession::workerLoop()
{
    SetCurrentThreadName("ProxySession:Worker");

    while (!Terminated) {
        Context->run();
    }
}

void ProxySession::Shutdown()
{
    Locker locker(APILock);

    if (Context) {
        // Note there is no need to stop or cancel timer ticks or sockets
        Context->stop();
    }

    // Allow worker threads to stop.
    // Note this has to be done after cancel/close of sockets above because
    // otherwise Asio can hang waiting for any outstanding callbacks.
    Terminated = true;

    // Wait for thread to stop
    if (Thread)
    {
        try
        {
            if (Thread->joinable())
                Thread->join();
        }
        catch (std::system_error& err)
        {
            Logger.Warning("Exception while joining thread: ", err.what());
        }
        Thread = nullptr;
    }

    C2S.Shutdown();
    S2C.Shutdown();

    // Destroy Asio objects
    Resolver = nullptr;
    Ticker = nullptr;
    Socket = nullptr;
    Context = nullptr;

    Logger.Info("Queue memory used: ", ReadBufferAllocator.GetUsedMemory() / 1000, " KB");
}


} // namespace mau
