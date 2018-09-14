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

#pragma once

#include "mau.h"
#include "MauTools.h"

namespace mau {


//------------------------------------------------------------------------------
// PacketQueue

#pragma pack(push, 1)
struct QueueNode
{
    // Microsecond delivery time
    uint64_t DeliveryUsec;

    // Filled in by PacketQueue:
    QueueNode* Next;
    QueueNode* Prev;

    // Number of bytes of data
    uint32_t Bytes;

    // Start of data
    uint8_t Data[1];
};
#pragma pack(pop)

/// Number of bytes in the QueueHeader (bytes to overallocate)
static const uint32_t kQueueHeaderSize = static_cast<uint32_t>(sizeof(QueueNode)) - 1;

class PacketQueue
{
public:
    // Insert into the queue sorted by delivery time
    void InsertSorted(QueueNode* node);

    QueueNode* Peek() const
    {
        return Head;
    }

    void Pop()
    {
        if (!Head) {
            return;
        }

        Head = Head->Next;
        if (!Head) {
            Tail = nullptr;
        }
        else {
            Head->Prev = nullptr;
        }
    }

protected:
    // Head is the next one to send
    QueueNode* Head = nullptr;
    QueueNode* Tail = nullptr;
};


//------------------------------------------------------------------------------
// LockedValue

template<class T>
class LockedValue
{
public:
    void Set(const T& value)
    {
        Locker locker(TheLock);
        Value = value;
    }
    T Get() const
    {
        Locker locker(TheLock);
        return Value;
    }

protected:
    /// Lock guarding value
    mutable Lock TheLock;

    /// Value guarded by Lock
    T Value;
};


//------------------------------------------------------------------------------
// DeliveryCommonData

struct DeliveryCommonData
{
    /// Logging channel
    logger::Channel Logger;

    /// Asio context
    std::shared_ptr<asio::io_context> Context;

    /// UDP socket
    std::unique_ptr<asio::ip::udp::socket> Socket;

    /// Allocator for read buffers
    BufferAllocator ReadBufferAllocator;

    /// Set to a failure code if anything goes wrong
    std::atomic<MauResult> LastResult = ATOMIC_VAR_INIT(Mau_Success);

    /// Protected by ConfigLock. Configuration provided via Initialize()
    LockedValue<MauChannelConfig> ChannelConfig;

    /// Configuration for the proxy
    MauProxyConfig ProxyConfig;


    DeliveryCommonData();
};


//------------------------------------------------------------------------------
// DeliveryChannel

class DeliveryChannel
{
public:
    /// Initialize the delivery channel
    bool Initialize(DeliveryCommonData* common);

    /// Set the delivery address
    void SetDeliveryAddress(const UDPAddress& addr);

    /// Push a new queue node into the channel
    void InsertQueueNode(QueueNode* node);

    /// Shutdown the delivery channel
    void Shutdown();

protected:
    /// Delivery timer
    std::unique_ptr<asio::steady_timer> DeliveryTimer;

    /// Common data
    DeliveryCommonData* Common = nullptr;

    /// Delivery address
    LockedValue<UDPAddress> DeliveryAddress;


    //--------------------------------------------------------------------------
    // Protected by QueueLock

    /// Lock protecting the timer setup and DeliveryQueue
    Lock QueueLock;

    /// Packet queue (with sorting) for delivery: 
    PacketQueue Queue;

    /// Next time that timer is waking up
    uint64_t NextTimerWakeUsec = 0;


    //--------------------------------------------------------------------------
    // Protected by InsertLock

    /// Lock to serialize insertions - Does not guard members
    Lock InsertLock;

    /// In a burst loss?
    bool InBurstLoss = false;

    /// In a reorder burst?
    bool InBurstReorder = false;

    /// Last time that an in-order packet was scheduled in microseconds
    uint64_t NextQueueSlotUsec = 0;

    /// Random number generator protected by DeliveryLock
    PCGRandom LossRNG;


    /// Post timer for next delivery
    void postNextTimer();
};


//------------------------------------------------------------------------------
// ProxySession

class ProxySession : protected DeliveryCommonData
{
public:
    MauResult Initialize(
        const char* serverHostname,
        uint16_t serverPort,
        const MauProxyConfig& proxyConfig,
        const MauChannelConfig& channelConfig);
    void Shutdown();

    void SetChannelConfig(const MauChannelConfig& channelConfig)
    {
        ChannelConfig.Set(channelConfig);
    }

    MauResult GetLastResult() const
    {
        return LastResult;
    }

    MauResult Inject(
        uint16_t sourcePort, ///< [in] Source port
        const void* datagram, ///< [in] Datagram buffer
        unsigned bytes); ///< [in] Datagram bytes

protected:
    std::string ServerHostname;
    uint16_t ServerPort = 0;

    /// Address associated with the last packet we received (maybe not our peer)
    UDPAddress SourceAddress;

    /// Mutex to prevent API calls from being made concurrently
    Lock APILock;

    /// Should worker thread be terminated?
    std::atomic<bool> Terminated = ATOMIC_VAR_INIT(false);

    /// Worker thread
    std::unique_ptr<std::thread> Thread;

    /// Client address
    LockedValue<UDPAddress> ClientAddress;

    /// Server address
    LockedValue<UDPAddress> ServerAddress;

    /// Bi-directional delivery channels
    DeliveryChannel C2S, S2C;

    /// Asio resolve object
    std::unique_ptr<asio::ip::tcp::resolver> Resolver;

    /// Timer for retries and timeouts (and to avoid using 100% CPU)
    std::unique_ptr<asio::steady_timer> Ticker;


    /// Worker thread loop
    void workerLoop();

    /// Post a timer tick event
    void postNextTimer();

    /// Called whenever the ticker ticks
    void onTick();

    /// Post next asynchronous read request
    void postNextRead(uint8_t* readBuffer);
};


} // namespace mau
