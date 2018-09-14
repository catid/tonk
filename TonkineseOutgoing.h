/** \file
    \brief Tonk Implementation: Outgoing Message Queue
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

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

#include "TonkineseTools.h"
#include "TonkineseProtocol.h"
#include "TonkineseBandwidth.h"
#include "TonkineseInterfaces.h"
#include "PacketCompression.h"
#include "SimpleCipher.h"

#include "siamese.h"

#include <atomic>

namespace tonk {


//------------------------------------------------------------------------------
// Constants

/// Random padding adds extra overhead (about 8 bytes on average) in order to
/// obscure the purpose of the encrypted datagrams.
#define TONK_ENABLE_RANDOM_PADDING

/// This will enable reliable packet sequence number compression
#define TONK_ENABLE_SEQNO_COMPRESSION

/// Padding seed domain for PCGRandom
static const uint64_t kPaddingSeedDomain = 123456;


//------------------------------------------------------------------------------
// OutgoingQueuedDatagram

/**
    This datagram object is allocated from the SessionOutgoing::WriteAllocator
    It has variable size based on the maximum amount of data that can be sent
    in a single datagram.  After the datagram has been topped off with messages
    to deliver, not to exceed MaxMessageSectionBytes, then the allocated region
    for this datagram is shrunk to just the number of bytes needed, and it is
    put on an outgoing queue to be delivered later.

    This datastructure also keeps track of the start and end of any reliable
    messages that are stored in the datagram, so that just that part can be
    stored for retransmission later or involved in FEC-based recovery.
*/
struct OutgoingQueuedDatagram
{
#ifdef TONK_DETAILED_STATS
    uint32_t Stats[Stats_QueueCount];

    void ZeroStats();
    void AddMessageStats(unsigned messageType, unsigned messageBytes);
    void AccumulateStats(OutgoingQueuedDatagram* other);
#endif // TONK_DETAILED_STATS

    /// Next datagram in the queue of unsent data
    OutgoingQueuedDatagram* Next;

    /// Next write offset
    unsigned NextWriteOffset; ///< in bytes

    /// Number of bytes saved by compression
    unsigned CompressionSavings; ///< in bytes

    /// Start of the packet data
    uint8_t Data[1];


    /// Set initial values for the members
    TONK_FORCE_INLINE void Initialize()
    {
        Next = nullptr;
        NextWriteOffset = 0;
        CompressionSavings = 0;
#ifdef TONK_DETAILED_STATS
        ZeroStats();
#endif // TONK_DETAILED_STATS
    }
};

/// Size of header structure
static const unsigned kQueueHeaderBytes = static_cast<unsigned>(
    sizeof(OutgoingQueuedDatagram));

/// This is the number of bytes we need to keep allocated on top of the
/// datagram payload
static const unsigned kAllocatedOverheadBytes = kQueueHeaderBytes \
    + protocol::kMaxOverheadBytes \
    + protocol::kMaxRandomPaddingBytes;


//------------------------------------------------------------------------------
// OutgoingQueue

/// Data shared by all OutgoingQueue objects
struct OutgoingQueueCommon
{
    /// Allocator used for data queued up for delivery
    /// Data is allocated as packets are constructed and
    /// data is freed after the Asio Send() call completes.
    BufferAllocator WriteAllocator;

    /// Maximum bytes in message section = MTU - (Max overhead)
    unsigned MaxMessageSectionBytes = 0;

    /// PrivateLock: Maximum datagram bytes allowed based on IPv6/IPv4 overhead
    unsigned UDPMaxDatagramBytes = 0;
};

/// OutgoingQueue represents one of the outgoing queues for SessionOutgoing.
class OutgoingQueue
{
public:
    /// Initialize the queue
    TONK_FORCE_INLINE void SetCommon(OutgoingQueueCommon* common)
    {
        Common = common;
    }

    /**
        Attempt to append a message to the current datagram being built.

        Supports 0-length messages.

        This will fail if the message is too larger to fit into the datagram,
        returning false if there was not enough room.

        Returns true if the message was appended successfully.
    */
    bool Append(
        unsigned messageType,
        const uint8_t* messageData,
        unsigned messageBytes);

    /// Returns the number of bytes written so far.
    /// Returns 0 if nothing could be written.
    /// Returns messageBytes if all bytes were written
    size_t AppendSplitReliable(
        unsigned messageType,
        const uint8_t* messageData,
        size_t messageBytes);

    /// Push current buffer onto the end of the queue and replace it with a
    /// fresh one.  If the buffer is empty it will do nothing
    Result PushAndGetFreshBuffer();

    /// Return the next datagram to send, or OutBuffer if it is not empty.
    /// Returns nullptr if there is no data to flush
    OutgoingQueuedDatagram* FlushPeek();

    /// Pop the front of the queue (removing it)
    /// Returns true if queue is now empty, else returns false
    bool PopIsEmpty();

    /// Check if the buffer is empty
    TONK_FORCE_INLINE bool IsEmpty() const
    {
        return QueuedBytes <= 0;
    }

    /// Get queue depth in bytes
    TONK_FORCE_INLINE unsigned GetQueuedBytes() const
    {
        return QueuedBytes;
    }

protected:
    /// Common data shared by all OutgoingQueues
    OutgoingQueueCommon* Common = nullptr;

    /// Next datagram to add to queue
    OutgoingQueuedDatagram* OutBuffer = nullptr;

    /// Singly-linked list from newest to oldest (FIFO)
    OutgoingQueuedDatagram* OldestQueued = nullptr;
    OutgoingQueuedDatagram* NewestQueued = nullptr;

    /// Depth of queue in bytes
    unsigned QueuedBytes = 0;
};


//------------------------------------------------------------------------------
// SessionOutgoing

class SessionOutgoing
    : public IIncomingToOutgoing
    , protected OutgoingQueueCommon
{
public:
    virtual ~SessionOutgoing() = default;

    struct Dependencies
    {
        bool EnableCompression;
        logger::Channel* Logger;
        IUDPSender* UDPSender;
        RefCounter* ConnectionRef;
#ifdef TONK_DETAILED_STATS
        DetailStatsMemory* Deets;
#endif // TONK_DETAILED_STATS
        SenderBandwidthControl* SenderControl;
    };

    Result Initialize(
        const Dependencies& deps,
        uint64_t key);
    void ChangeEncryptionKey(uint64_t key);
    void Shutdown();

    TONK_FORCE_INLINE void SetUDPSender(IUDPSender* sender)
    {
        Deps.UDPSender = sender;
    }

    TONK_FORCE_INLINE BufferAllocator* GetWriteAllocator()
    {
        return &WriteAllocator;
    }


    //--------------------------------------------------------------------------
    // These functions can be called from the public API so need extra locking
    // to prevent thread safety issues with the timer tick or receive events.

    /// Fill TonkStatus with current state
    void tonk_status(TonkStatus& statusOut) const;

    /// Fill TonkStatusEx with current state
    void tonk_status_ex(TonkStatusEx& statusExOut) const;

    /// Queue a message for sending.  This only supports message types that will
    /// not split into multiple datagrams, so it excludes Reliable/LowPri types.
    Result QueueUnreliable(
        protocol::MessageType messageType,
        const uint8_t* data,
        size_t bytes) override;

    /// Queue an unordered reliable message for sending.  Does not split messages
    Result QueueUnordered(
        const uint8_t* data,
        size_t bytes) override;

    /// Queue a Control reliable message for sending.  Does not split messages
    Result QueueControl(
        const uint8_t* data,
        size_t bytes) override;

    /// Queue a reliable message for sending.  This will split up messages
    Result QueueReliable(
        unsigned messageType,
        const uint8_t* data,
        size_t bytes) override;

    /// Pass acknowledgement data from peer
    Result OnAcknowledgements(
        const uint8_t* data,
        unsigned bytes,
        Counter64 ackNonce,
        uint64_t receiveUsec) override;

    // End of public API calls
    //--------------------------------------------------------------------------

    /// Flush immediately if possible to reduce latency
    Result Flush();

    /// Send a Recovery message.
    /// Does not respect probe nor app bandwidth limits.
    /// Returns bytesSentOut=0 if no message was sent
    /// If no message was sent, an error may or may not have occurred,
    /// which can be checked by checking the returned Result.
    Result PostRecovery(int& bytesSentOut);

    /// Send a retransmitted reliable message.
    /// Respects app bandwidth limit.
    /// Returns messageSent=false if no message was sent
    /// If no message was sent, an error may or may not have occurred,
    /// which can be checked by checking the returned Result.
    Result PostRetransmit(bool& messageSent);

    /// Send a dummy datagram with a payload that is 1300 bytes long.
    /// Respects probe bandwidth limit.
    /// Returns true if a datagram was sent
    /// Returns false if no datagram was sent
    /// If no message was sent, an error may or may not have occurred,
    /// which can be checked by checking the returned Result.
    bool PostDummyDatagram();

    /// Update the peer UDP address
    void SetPeerUDPAddress(const UDPAddress& addr);

    /// It is normal for this to be accessed when peer address is invalid.
    /// The expected return is an empty address object in this case.
    TONK_FORCE_INLINE UDPAddress GetPeerUDPAddress() const
    {
        Locker locker(PrivateLock);
        return PeerUDPAddress;
    }

    /// Return true if peer address has been set
    TONK_FORCE_INLINE bool IsPeerAddressValid() const
    {
        return HasPeerAddress;
    }

    /// Return the last time that an acknowledgement was received
    TONK_FORCE_INLINE uint64_t GetLastAckReceiveUsec() const
    {
        return LastAck.ReceiveUsec;
    }

    /// Return true if peer is caught up.
    /// In initial state before sending any data this also returns true
    TONK_FORCE_INLINE bool PeerCaughtUp() const
    {
        return LastAck.PeerNextExpectedSeqNum == NextSequenceNumber;
    }

    /// Handshake controls
    void StartHandshakeC2SConnect(uint64_t key);
    void StartHandshakePeer2PeerConnect(uint64_t key);
    void StartHandshakeC2SUpdateSourceAddress(uint64_t key, uint32_t id);
    void StopHandshake();

    /// Update the next expected nonce from time sync instead of ack
    void UpdateNextExpectedNonceFromTimeSync(Counter24 nextExpectedNonce24);

protected:
    Dependencies Deps;

    /// Subsystems
    security::SimpleCipher Encryptor;

    /// This protects the outgoing queues as they are modified directly from the
    /// public API via the Queue*() API functions.
    mutable Lock OutgoingQueueLock;

    /// This lock protects the following few members
    mutable Lock PrivateLock;

    /// PrivateLock: Peer UDP/IP address
    UDPAddress PeerUDPAddress;

    /// PrivateLock: Do we have the peer address?
    std::atomic<bool> HasPeerAddress = ATOMIC_VAR_INIT(false);

    /// PrivateLock: Cached status to speed up tonk_status()
    TonkAddress CachedRemoteAddress;

    /// FEC encoder
    SiameseEncoder FECEncoder = nullptr;

    /// Handshake footer field
    std::atomic<bool> ShouldAttachHandshake = ATOMIC_VAR_INIT(false);
    uint8_t Handshake[protocol::kHandshakeBytes];

    /// Information provided by the last acknowledgement from peer
    struct AckInfo
    {
        /// Smallest acceptable nonce on the next accepted ack message.
        /// This avoids processing ack messages out of order
        Counter64 NextAcceptableNonce = 0;

        /// Time of last acknowledgement message
        uint64_t ReceiveUsec = 0;

        /// Peer's next expected nonce
        Counter64 PeerNextExpectedNonce = 0;

        /// Peer's next expected reliable packet sequence number
        unsigned PeerNextExpectedSeqNum = 0;
    };
    AckInfo LastAck;

    /// Next reliable packet sequence number to send
    unsigned NextSequenceNumber = 0;

    /// Next nonce to generate
    NonceT NextNonce = 0;

    /**
        Outgoing Queues : Tonk Quality of Service (QoS) Heuristics

        The purpose of Tonk's QoS is simply to prevent file transfers and other
        high-rate traffic from stomping on realtime traffic.  More complicated
        queuing would require additional copies and more complicated code to
        interleave the messages.  Unreliable data is considered most important,
        followed by Reliable data, with (reliable) LowPri data sent last.

        The Unreliable data will not adhere to bandwidth limits and will be sent
        first.  The Reliable and LowPri data are compressed and protected by FEC
        to improve overall latency.

        There is an Unreliable queue, a Reliable queue, and a LowPri queue.
        The application and Tonk submit framed data into these queues.
        The queues are made up of roughly MTU-sized chunks of data.

        When Tonk decides to flush data in a background thread, it first writes
        unreliable data to the outgoing packets.  Once the unreliable data is
        fully flushed, then the reliable data is compressed one block at a time.
        The compressed data is framed and appended to any unreliable data.
        Packets containing compressed reliable data will be given a sequence
        number and the compressed data will be submitted to Siamese for sending

        Unordered reliable messages and control messages are not compressed
        because they may be received in any order.  These are also sent before
        other reliable messages because they tend to be more urgent.  In line
        with that urgency, no rate limiting is applied.
    */
    enum QueueTypes
    {
        // Unreliable types - No rate limiting
        Queue_Unreliable = 0,

        // Unordered and Control types - No compression or rate limiting
        Queue_Unmetered = 1,

        // Reliable streams - Compression, message splitting, and rate limiting
        Queue_Reliable = 2,

        // LowPri streams - Compression, message splitting, and rate limiting
        Queue_LowPri = 3,

        Queue_Count
    };
    OutgoingQueue Queues[Queue_Count];

#ifdef TONK_ENABLE_RANDOM_PADDING

    /// PRNG used for padding
    siamese::PCGRandom PaddingPRNG;

#endif // TONK_ENABLE_RANDOM_PADDING

    /// Message compression
    MessageCompressor Compressor;


    /// Send a queued datagram- Called by two functions below
    void sendQueuedDatagram(
        OutgoingQueuedDatagram* datagram,
        unsigned sequenceNumberBytes,
        unsigned sequenceNumber);

    /// Send a queued unreliable datagram
    TONK_FORCE_INLINE void sendQueuedUnreliable(OutgoingQueuedDatagram* datagram)
    {
        sendQueuedDatagram(datagram, 0, 0);
    }

    /// Shrink-wrap a reliable datagram to only the bytes needed and send it
    TONK_FORCE_INLINE Result shrinkWrapAndSendReliable(OutgoingQueuedDatagram* datagram, unsigned reliableOffset)
    {
        // Shrink-wrap the datagram
        const unsigned neededBytes = datagram->NextWriteOffset + kAllocatedOverheadBytes;
        WriteAllocator.Shrink(datagram, neededBytes);

        return sendQueuedReliable(datagram, reliableOffset);
    }

    /// Send a queued datagram containing reliable data after some offset
    Result sendQueuedReliable(OutgoingQueuedDatagram* datagram, unsigned reliableOffset);

    /// Get number of bytes used to represent the given packet number in our protocol
    /// taking into account the peer's next expected sequence number
    unsigned getPacketNumBytes(int32_t packetNum) const;

    /// Assign the next nonce in sequence and write it to the footer.
    /// Returns the written nonce, and the number of bytes written.
    /// Writes between 1 ... kNonceBytes bytes.
    NonceT writeNonce(
        uint8_t* footer,           ///< Pointer to the first byte to write
        unsigned& footerBytesOut); ///< Number of bytes written

    /// Queue a message on a given queue index.
    /// Internal function to implement the public API
    Result queueMessage(
        unsigned queueIndex,
        protocol::MessageType messageType,
        const uint8_t* data,
        size_t bytes);
};


} // namespace tonk
