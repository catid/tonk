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

#include "TonkineseOutgoing.h"

namespace tonk {

static logger::Channel ModuleLogger("Outgoing", MinimumLogLevel);

#ifdef TONK_ENABLE_VERBOSE_OUTGOING
    #define TONK_VERBOSE_OUTGOING_LOG(...) ModuleLogger.Info(__VA_ARGS__);

// Statistics for recovery send rate
static uint64_t m_Recovery = 0;
static uint64_t m_Reliable = 0;
#else
    #define TONK_VERBOSE_OUTGOING_LOG(...) ;
#endif


//------------------------------------------------------------------------------
// Constants

/// Padding seed domain for PCGRandom
static const uint64_t kPaddingSeedDomain = 123456;


//------------------------------------------------------------------------------
// Tools

NonceT SessionOutgoing::writeNonce(uint8_t* footer, unsigned& footerBytesOut)
{
    // Note nonce can roll over here back to 0, and that is okay because this
    // is not intended to provide real data security.
    NonceT nonce = NextNonce++;

    // Choose the number of bytes to send:
    footerBytesOut = 3;

#ifdef TONK_ENABLE_NONCE_COMPRESSION
    if (ShouldCompressSequenceNumbers)
    {
        // Use the magnitude of the difference between this nonce and the peer's
        // next expected nonce to decide how many bytes to send for the field.
        int32_t mag = (int32_t)nonce - (int32_t)LastAck.PeerNextExpectedNonce.ToUnsigned();

#if 1
        if (mag == 1) {
            footerBytesOut = 0;
        }
        else
#endif
        {
            if (mag < 0) {
                TONK_DEBUG_BREAK(); // Should never happen
                mag = -mag;
            }
            ++mag;
            TONK_DEBUG_ASSERT(mag < 0x800000);

            if (mag < 0x80) {
                footerBytesOut = 1;
            }
            else if (mag < 0x8000) {
                footerBytesOut = 2;
            }
        }
    }
#endif

    protocol::WriteFooterField(footer, (uint32_t)nonce, footerBytesOut);

    return nonce;
}
/// Get number of bytes used to represent the given packet number in our protocol
/// taking into account the peer's next expected sequence number
unsigned SessionOutgoing::getPacketNumBytes(int32_t packetNum) const
{
    TONK_UNUSED(packetNum);

#ifdef TONK_ENABLE_SEQNO_COMPRESSION
    if (ShouldCompressSequenceNumbers)
    {
        // The peer uses its next expected sequence number to decompress the
        // numbers we send.  By the time a new datagram arrives, that number
        // may have advanced from that last one acknowledged through the last
        // one that was sent.  When sending a new sequence number, include
        // enough bits so that any possible number can decode the new one.
        // So, take the larger of the distances from each end.
        int32_t mag = (int32_t)packetNum - (int32_t)LastAck.PeerNextExpectedSeqNum;
        if (mag < 0) {
            mag = -mag;
        }
        TONK_DEBUG_ASSERT(mag < 0x800000);
        int32_t mag2 = (int32_t)packetNum - (int32_t)NextSequenceNumber;
        if (mag2 < 0) {
            mag2 = -mag2;
        }
        TONK_DEBUG_ASSERT(mag2 < 0x800000);
        if (mag < mag2) {
            mag = mag2;
        }
        // Add one to handle ambiguity due to two's complement negatives
        // having one more value of precision than positive numbers
        ++mag;

        // Choose the number of bytes to send:
        if (mag < 0x80) {
            return 1;
        }
        else if (mag < 0x8000) {
            return 2;
        }
    }
#endif // TONK_ENABLE_SEQNO_COMPRESSION

    return 3;
}


//------------------------------------------------------------------------------
// OutgoingQueuedDatagram

#ifdef TONK_DETAILED_STATS

void OutgoingQueuedDatagram::AddMessageStats(
    unsigned messageType,
    unsigned messageBytes)
{
    Stats[Stats_FramingBytesSent] += protocol::kMessageFrameBytes;
    if (messageType == protocol::MessageType_AckAck ||
        messageType == protocol::MessageType_Acknowledgements)
    {
        Stats[Stats_AcksBytesSent] += messageBytes;
    }
    else if (messageType == protocol::MessageType_Unreliable) {
        Stats[Stats_UnreliableBytesSent] += messageBytes;
    }
    else if (messageType == protocol::MessageType_Unordered) {
        Stats[Stats_UnorderedBytesSent] += messageBytes;
    }
    else if (messageType == protocol::MessageType_TimeSync ||
        messageType == protocol::MessageType_Control ||
        messageType == protocol::MessageType_Padding)
    {
        Stats[Stats_ControlBytesSent] += messageBytes;
    }
    else if (messageType >= protocol::MessageType_LowPri) {
        Stats[Stats_LowPriBytesSent] += messageBytes;
    }
    else if (messageType >= protocol::MessageType_Reliable) {
        Stats[Stats_ReliableBytesSent] += messageBytes;
    }
    TONK_DEBUG_ASSERT(messageType != protocol::MessageType_Compressed);
}

void OutgoingQueuedDatagram::ZeroStats()
{
    for (int i = 0; i < Stats_QueueCount; ++i) {
        Stats[i] = 0;
    }
}

void OutgoingQueuedDatagram::AccumulateStats(OutgoingQueuedDatagram* other)
{
    for (int i = 0; i < Stats_QueueCount; ++i) {
        Stats[i] += other->Stats[i];
    }
}

#endif // TONK_DETAILED_STATS


//------------------------------------------------------------------------------
// OutgoingQueue

bool OutgoingQueue::Append(
    unsigned messageType,
    const uint8_t* messageData,
    unsigned messageBytes)
{
    TONK_VERBOSE_OUTGOING_LOG("OutgoingQueue::Append messageType=", messageType,
        " messageBytes=", messageBytes, " OutBuffer=", OutBuffer);

    OutgoingQueuedDatagram* datagram = OutBuffer;
    if (!datagram)
    {
        TONK_VERBOSE_OUTGOING_LOG("Append: OutBuffer is null - abort");
        return false;
    }

    const unsigned writeBytes = static_cast<unsigned>(
        protocol::kMessageFrameBytes + messageBytes);
    const unsigned nextWriteOffset = datagram->NextWriteOffset;

    if (nextWriteOffset + writeBytes > Common->MaxMessageSectionBytes)
    {
        TONK_VERBOSE_OUTGOING_LOG("Append: nextWriteOffset=", nextWriteOffset,
            " + writeBytes=", writeBytes, " > ", Common->MaxMessageSectionBytes);
        return false;
    }

    uint8_t* messagePtr = datagram->Data + nextWriteOffset;

    // Write 2-byte message frame header
    protocol::WriteMessageFrameHeader(messagePtr, messageType, messageBytes);

    // Copy message into place
    memcpy(messagePtr + protocol::kMessageFrameBytes, messageData, messageBytes);

    // Record that the data was written
    datagram->NextWriteOffset = nextWriteOffset + writeBytes;

    // Update queued bytes
    QueuedBytes += writeBytes;

    TONK_VERBOSE_OUTGOING_LOG("Append: WROTE TO DATAGRAM datagram->NextWriteOffset=",
        datagram->NextWriteOffset, " + writeBytes=", writeBytes, " MaxMessageSectionBytes=",
        Common->MaxMessageSectionBytes, " QueuedBytes=", QueuedBytes);

#ifdef TONK_DETAILED_STATS
    datagram->AddMessageStats(messageType, messageBytes);
#endif // TONK_DETAILED_STATS

    return true;
}

size_t OutgoingQueue::AppendSplitReliable(
    unsigned messageType,
    const uint8_t* messageData,
    size_t messageBytes)
{
    TONK_VERBOSE_OUTGOING_LOG("OutgoingQueue::AppendSplitReliable messageType=", messageType,
        " messageBytes=", messageBytes, " OutBuffer=", OutBuffer);

    OutgoingQueuedDatagram* datagram = OutBuffer;
    if (!datagram)
    {
        TONK_VERBOSE_OUTGOING_LOG("AppendSplitReliable: OutBuffer is null - abort");
        return 0;
    }

    const unsigned nextWriteOffset = datagram->NextWriteOffset;
    const unsigned bufferBytes = Common->MaxMessageSectionBytes;

    // If there is not enough room left in the datagram then return false
    if (nextWriteOffset + protocol::kMessageFrameBytes + protocol::kMessageSplitMinimumBytes > bufferBytes)
    {
        TONK_VERBOSE_OUTGOING_LOG("AppendSplitReliable: nextWriteOffset=", nextWriteOffset,
            " + overhead=", (protocol::kMessageFrameBytes + protocol::kMessageSplitMinimumBytes),
            " > bufferBytes=", bufferBytes);
        return 0;
    }

    unsigned writeBytes = static_cast<unsigned>(protocol::kMessageFrameBytes + messageBytes);

    // If there are more bytes to write than space available:
    if (nextWriteOffset + writeBytes > bufferBytes)
    {
        TONK_VERBOSE_OUTGOING_LOG("AppendSplitReliable: nextWriteOffset=", nextWriteOffset,
            " + writeBytes=", writeBytes, " > bufferBytes=", bufferBytes);

        TONK_DEBUG_ASSERT(bufferBytes >= nextWriteOffset);
        writeBytes = bufferBytes - nextWriteOffset;

        TONK_DEBUG_ASSERT(writeBytes >= protocol::kMessageFrameBytes);
        messageBytes = static_cast<size_t>(writeBytes - protocol::kMessageFrameBytes);

        TONK_VERBOSE_OUTGOING_LOG("AppendSplitReliable: Now writeBytes=", writeBytes,
            " messageBytes=", messageBytes);
    }
    else
    {
        TONK_VERBOSE_OUTGOING_LOG("AppendSplitReliable: FINAL MESSAGE");

        messageType += TonkChannel_Count; // Mark as final message
    }

    uint8_t* messagePtr = datagram->Data + nextWriteOffset;

    // Write 2-byte message frame header
    protocol::WriteMessageFrameHeader(
        messagePtr,
        messageType,
        static_cast<unsigned>(messageBytes));

    // Copy message into place
    memcpy(messagePtr + protocol::kMessageFrameBytes, messageData, messageBytes);

    TONK_DEBUG_ASSERT(messageType >= protocol::MessageType_Reliable);

    // Record that the data was written
    datagram->NextWriteOffset = nextWriteOffset + writeBytes;

    // Update queued bytes
    QueuedBytes += writeBytes;

    TONK_VERBOSE_OUTGOING_LOG("AppendSplitReliable: WROTE TO DATAGRAM datagram->NextWriteOffset=",
        datagram->NextWriteOffset, " + writeBytes=", writeBytes, " MaxMessageSectionBytes=",
        Common->MaxMessageSectionBytes, " QueuedBytes=", QueuedBytes);

#ifdef TONK_DETAILED_STATS
    datagram->AddMessageStats(messageType, static_cast<unsigned>(messageBytes));
#endif // TONK_DETAILED_STATS

    return messageBytes;
}

Result OutgoingQueue::PushAndGetFreshBuffer()
{
    if (OutBuffer)
    {
        // If there was no data written yet:
        if (OutBuffer->NextWriteOffset <= 0) {
            return Result::Success();
        }

        const unsigned neededBytes = OutBuffer->NextWriteOffset + kAllocatedOverheadBytes;

        // Reduce memory usage
        Common->WriteAllocator.Shrink(OutBuffer,
            neededBytes);

        // Link at end of list (After NewestQueued)
        if (NewestQueued) {
            NewestQueued->Next = OutBuffer;
        }
        else {
            OldestQueued = OutBuffer;
        }
        NewestQueued = OutBuffer;
        TONK_DEBUG_ASSERT(OutBuffer->Next == nullptr);

        TONK_VERBOSE_OUTGOING_LOG("PushAndGetFreshBuffer : Flushed OutBuffer = ", OutBuffer);
    }

    OutBuffer = (OutgoingQueuedDatagram*)Common->WriteAllocator.Allocate(
        kQueueHeaderBytes + Common->UDPMaxDatagramBytes);

    if (!OutBuffer) {
        return Result::OutOfMemory();
    }

    OutBuffer->Initialize();

    TONK_VERBOSE_OUTGOING_LOG("PushAndGetFreshBuffer : Created new OutBuffer = ", OutBuffer);

    return Result::Success();
}

OutgoingQueuedDatagram* OutgoingQueue::FlushPeek()
{
    // If there is no data in the queue:
    if (QueuedBytes <= 0) {
        return nullptr;
    }

    // Get next datagram from the queue
    OutgoingQueuedDatagram* datagram = OldestQueued;
    if (datagram) {
        TONK_VERBOSE_OUTGOING_LOG("FlushPeek -> OldestQueued = ", datagram);
        return datagram;
    }

    // TBD: To simplify this code path, we do not handle OOM properly here.
    // This function actually returns a Result object, but it only fails
    // if we run out of memory.  Since this is a critical code path it seems
    // okay to leave it out.
    PushAndGetFreshBuffer();

    // Return the oldest queued datagram in the list
    datagram = OldestQueued;
    TONK_DEBUG_ASSERT(datagram != nullptr); // Should never happen

    TONK_VERBOSE_OUTGOING_LOG("FlushPeek (Pushed) -> OldestQueued = ", datagram);

    return datagram;
}

bool OutgoingQueue::PopIsEmpty()
{
    OutgoingQueuedDatagram* datagram = OldestQueued;
    TONK_DEBUG_ASSERT(datagram != nullptr);

    // Advance the list head
    OldestQueued = datagram->Next;

    // Update queued bytes
    TONK_DEBUG_ASSERT(QueuedBytes >= datagram->NextWriteOffset);
    QueuedBytes -= datagram->NextWriteOffset;

    // If this was the end of the list:
    if (!OldestQueued)
    {
        NewestQueued = nullptr;
        return true;
    }

    return false;
}


//------------------------------------------------------------------------------
// SessionOutgoing

Result SessionOutgoing::Initialize(
    const Dependencies& deps,
    uint64_t key)
{
    Deps = deps;

    for (unsigned i = 0; i < Queue_Count; ++i) {
        Queues[i].SetCommon(this);
    }

    // Initially assume worst-case MTU available
    UDPMaxDatagramBytes = protocol::kMinPossibleDatagramByteLimit;
    MaxMessageSectionBytes = UDPMaxDatagramBytes - protocol::kMaxOverheadBytes;

    Encryptor.Initialize(key);

    // Clear remote address
    memset(&CachedRemoteAddress, 0, sizeof(CachedRemoteAddress));

    // Create the FEC encoder object
    FECEncoder = siamese_encoder_create();
    if (!FECEncoder) {
        return Result("SessionOutgoing::Initialize", "siamese_encoder_create failed", ErrorType::Siamese);
    }

#ifdef TONK_ENABLE_RANDOM_PADDING
    // Seed the padding PRNG
    PaddingPRNG.Seed(key, kPaddingSeedDomain);
#endif // TONK_ENABLE_RANDOM_PADDING

    return Compressor.Initialize(protocol::kCompressionAllocateBytes);
}

void SessionOutgoing::ChangeEncryptionKey(uint64_t key)
{
    Encryptor.Initialize(key);
}

void SessionOutgoing::Shutdown()
{
    uint64_t stats[SiameseEncoderStats_Count];
    SiameseResult result = siamese_encoder_stats(FECEncoder, stats, SiameseEncoderStats_Count);
    if (result != Siamese_Success) {
        Deps.Logger->Error("Unable to retrieve siamese encoder stats");
    }
    else {
        Deps.Logger->Debug("Siamese encoder stats:" \
            " Memory used = ", stats[SiameseEncoderStats_MemoryUsed] / 1000, " KB" \
            ", Originals sent = ", stats[SiameseEncoderStats_OriginalCount],
            ", Recoveries sent = ", stats[SiameseEncoderStats_RecoveryCount]);
    }

    Deps.Logger->Debug("Write allocator memory used: ",
        WriteAllocator.GetUsedMemory() / 1000, " KB");

    siamese_encoder_free(FECEncoder);

    // Note: Allocations are freed automatically by FECEncoder object
}

#ifdef TONK_ENABLE_RANDOM_PADDING

// LUT for unif->exp RV with a mean of ~8 bytes
// Hand-adjusted to not spike quite so high...
// Values must not exceed kMaxRandomPaddingBytes
static const uint8_t TONK_RAND_PAD_EXP[256] = {
    15, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12,
    15, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12,
    15, 14, 14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12,
    11, 11, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0
};

#endif // TONK_ENABLE_RANDOM_PADDING

Result SessionOutgoing::sendQueuedReliable(
    OutgoingQueuedDatagram* datagram,
    unsigned reliableOffset)
{
    TONK_DEBUG_ASSERT(datagram->NextWriteOffset >= reliableOffset);
    if (datagram->NextWriteOffset <= reliableOffset) {
        sendQueuedDatagram(datagram, 0, 0);
        return Result::Success();
    }

    SiameseOriginalPacket original;
    original.Data = datagram->Data + reliableOffset;
    original.DataBytes = datagram->NextWriteOffset - reliableOffset;
    original.PacketNum = 0;

    // Add reliable data to the Siamese FEC encoder
    const int addResult = siamese_encoder_add(FECEncoder, &original);

    if (addResult != Siamese_Success) {
        TONK_DEBUG_BREAK(); // Should never happen
        return Result("SessionOutgoing::Flush", "siamese_encoder_add failed", ErrorType::Siamese, addResult);
    }

    const unsigned packetNum = original.PacketNum;
    const unsigned packetNumBytes = getPacketNumBytes(packetNum);

    // Update last sent packet sequence number.
    // Used for AckAck generation and sequence number compression
    TONK_DEBUG_ASSERT(NextSequenceNumber == packetNum);
    NextSequenceNumber = SIAMESE_PACKET_NUM_INC(packetNum);

    sendQueuedDatagram(datagram, packetNumBytes, packetNum);

#ifdef TONK_ENABLE_VERBOSE_OUTGOING
    m_Reliable++;
#endif // TONK_ENABLE_VERBOSE_OUTGOING

    return Result::Success();
}

void SessionOutgoing::sendQueuedDatagram(
    OutgoingQueuedDatagram* datagram,
    unsigned sequenceNumberBytes,
    unsigned sequenceNumber)
{
    // If this function changes, also update PostRecovery() and PostDummyDatagram() to keep it in sync.

    uint8_t* datagramData = datagram->Data;
    unsigned messageBytes = datagram->NextWriteOffset;

    TONK_DEBUG_ASSERT(messageBytes > 0);

#ifdef TONK_ENABLE_RANDOM_PADDING
    unsigned paddingBytes = 0;

    // If there is room for one more message in the datagram:
    if (Deps.EnablePadding &&
        messageBytes + protocol::kMessageFrameBytes <= MaxMessageSectionBytes)
    {
        // Select the number of bytes to pad
        unsigned targetPadBytes = TONK_RAND_PAD_EXP[PaddingPRNG.Next() % 256];

        TONK_DEBUG_ASSERT(targetPadBytes <= protocol::kMaxRandomPaddingBytes);
        if (messageBytes + targetPadBytes > MaxMessageSectionBytes) {
            targetPadBytes = MaxMessageSectionBytes - messageBytes;
        }

        // If there is at least a frame worth of padding:
        if (targetPadBytes >= protocol::kMessageFrameBytes)
        {
            // Write frame header
            protocol::WriteMessageFrameHeader(
                datagramData + messageBytes,
                protocol::MessageType_Padding,
                targetPadBytes - protocol::kMessageFrameBytes);

            // Zero the padding bytes to avoid sending confidential data
            memset(
                datagramData + messageBytes + protocol::kMessageFrameBytes,
                0,
                targetPadBytes - protocol::kMessageFrameBytes);

            paddingBytes += targetPadBytes;
            messageBytes += targetPadBytes;
        }
    }
#endif // TONK_ENABLE_RANDOM_PADDING

    uint8_t* footer = datagramData + messageBytes;
    uint8_t flags = protocol::kConnectionMask;
    if (ShouldCompressSequenceNumbers) {
        flags |= protocol::kSeqCompMask;
    }

    if (sequenceNumberBytes > 0)
    {
        flags |= sequenceNumberBytes;
        protocol::WriteFooterField(footer, sequenceNumber, sequenceNumberBytes);
        footer += sequenceNumberBytes;
    }

    // Write packet nonce to header
    unsigned nonceBytes;
    const NonceT nonce = writeNonce(footer, nonceBytes);
    flags |= nonceBytes << 2;
    footer += nonceBytes;

    if (ShouldAttachHandshake)
    {
        flags |= protocol::kHandshakeMask;
        const uint32_t b = siamese::ReadU32_LE(Handshake + 8);
        const uint64_t a = siamese::ReadU64_LE(Handshake);
        siamese::WriteU64_LE(footer, a);
        siamese::WriteU32_LE(footer + 8, b);
        footer += protocol::kHandshakeBytes;
    }

    // Tag checksum includes all the data before the timestamp
    const unsigned taggedBytes = (unsigned)(footer - datagramData);

    // Skip over the timestamp
    uint8_t* timestampData = footer;
    footer += protocol::kTimestampBytes;

    // Write flags
    footer[0] = flags;
    ++footer;

    // Encrypt message data but not the footer
    Encryptor.Cipher(datagramData, messageBytes, nonce);

    // Generate the main packet data tag
    uint16_t tag = Encryptor.Tag(datagramData, taggedBytes, nonce);
    static_assert(protocol::kEncryptionTagBytes == 2, "Update this if that changes");
    uint8_t* tagData = footer;

    const unsigned sendBytes = taggedBytes + protocol::kUntaggedDatagramBytes;

    DatagramBuffer datagramBuffer;
    datagramBuffer.AllocatorForFree = &WriteAllocator;
    datagramBuffer.Data = datagramData;
    datagramBuffer.FreePtr = datagram;
    datagramBuffer.Bytes = sendBytes;

    TONK_DEBUG_ASSERT(Deps.UDPSender != nullptr);
    TONK_DEBUG_ASSERT(HasPeerAddress);

    TONK_VERBOSE_OUTGOING_LOG("sendQueuedDatagram: datagram = ", datagram, " sequenceNumber=",
        sequenceNumber, " sequenceNumberBytes=", sequenceNumberBytes, " bytes=", datagramBuffer.Bytes);

#ifdef TONK_DETAILED_STATS
    DetailStats* stats = Deps.Deets->GetWritePtr();
    for (int i = 0; i < Stats_QueueCount; ++i) {
        stats->Stats[i] += datagram->Stats[i];
    }
    stats->Stats[Stats_PaddingBytesSent] += paddingBytes;
    stats->Stats[Stats_FootersBytesSent] += sendBytes - messageBytes;
#endif // TONK_DETAILED_STATS

    // Write timestamp and final tag - Reduce noise in timestamp measurements
    const uint64_t nowUsec = siamese::GetTimeUsec();
    const uint32_t timestamp24 = (uint32_t)(nowUsec >> kTime23LostBits);
    tag ^= Encryptor.TagInt(protocol::TimestampFlagTagInt(timestamp24, flags));
    siamese::WriteU24_LE(timestampData, timestamp24);
    siamese::WriteU16_LE(tagData, tag);

    const unsigned compressionSavings = datagram->CompressionSavings;

    // Send the data
    Deps.UDPSender->Send(
        PeerUDPAddress,
        datagramBuffer,
        Deps.ConnectionRef);

    // Update bandwidth control
    Deps.SenderControl->OnSendDatagram(
        compressionSavings,
        sendBytes,
        sequenceNumberBytes > 0);
}

/*
    Discussion: Where should we add data to the FEC encoder?

    Per message?  This would not allow us to combine messages.
    This is bad for the FEC encoder because speed is O(N^2) in
    the number of packets it is protecting -- fewer is better.

    When messages are put in the outgoing queue?  This would mean
    that FEC encoder output would include data that is not sent yet.
    It would cause unnecessary delays and CPU work at the decoder,
    and it would slow down the send path.

    Right before sending the datagram?  It is in a hot path which is
    unfortunate but this seems to be the only option without issues.
    One advantage of this approach is that the Flush() call, when
    invoked from the timer background thread, does not impact the
    application main loop.

    It also means we can do compression from a background thread.
*/

Result SessionOutgoing::Flush()
{
    // Note: There are a number of failure paths that leak objects.
    // This seems okay since in response to the error we disconnect,
    // and all of these allocations get automatically freed afterwards

    // If peer address is not known yet:
    if (!HasPeerAddress) {
        return Result::Success();
    }

    // Try to combine the last unreliable frames with the first reliable frames
    OutgoingQueuedDatagram* finalUnreliable = nullptr;

    // For the unreliable queue:

    OutgoingQueue* unreliableQueue = &Queues[Queue_Unreliable];

    // Quick skip for common case with no data waiting:
    if (unreliableQueue->GetQueuedBytes() > 0)
    // For all datagrams for this queue:
    for (;;)
    {
        // Grab next datagram from queue (if any)
        OutgoingQueuedDatagram* datagram;
        bool isEmpty = true;
        {
            Locker locker(OutgoingQueueLock);

            // Look at the next datagram
            datagram = unreliableQueue->FlushPeek();

            if (datagram) {
                // Pop it off the front
                isEmpty = unreliableQueue->PopIsEmpty();
            }
        }

        if (!datagram) {
            // Continue on to reliable queued data
            break;
        }

        TONK_DEBUG_ASSERT(datagram->NextWriteOffset > 0);

        if (isEmpty) {
            finalUnreliable = datagram;
            break;
        }

        // There are more to send- send this one now
        sendQueuedUnreliable(datagram);
    } // Next datagram in queue

    // Combine datagrams together if compression works well enough
    OutgoingQueuedDatagram* combined = nullptr;

    // Separate failed compression buffers with an empty compression frame
    bool prevCompressFailed = false;

    // Offset to reliable data
    unsigned reliableOffset = 0;

    // Send Reliable queue first and then LowPri queue datagrams
    for (unsigned queueIndex = Queue_Unmetered; queueIndex < Queue_Count; ++queueIndex)
    {
        OutgoingQueue* queue = &Queues[queueIndex];

        // Quick skip for common case with no data waiting:
        if (queue->GetQueuedBytes() <= 0) {
            continue;
        }

        // For all datagrams for this queue:
        for (;;)
        {
            // If this is a Reliable or LowPri message:
            if (queueIndex >= Queue_Reliable) {
                // If there is no space left:
                if (Deps.SenderControl->GetAvailableBytes() <= 0) {
                    goto DoneSending;
                }
            }

            const SiameseResult isReady = siamese_encoder_is_ready(FECEncoder);
            if (isReady != Siamese_Success) {
                //Deps.Logger->Warning("SiameseFEC not ready for more data. Delaying sending more until some data is acknowledged");
                goto DoneSending;
            }

            // Grab next datagram from queue (if any)
            // TBD: This lock is a little heavy - Sometimes it waits for
            // a queue insertion to finish.  It would be nicer if we flushed
            // just once at the top of the Flush() and then pulled results
            // off the protected queue a bit faster.
            OutgoingQueuedDatagram* datagram;
            {
                Locker locker(OutgoingQueueLock);
                datagram = queue->FlushPeek();
                if (!datagram) {
                    // Done with this queue
                    break;
                }
                queue->PopIsEmpty();
            }

            const unsigned datagramBytes = datagram->NextWriteOffset;
            const uint8_t* datagramData = datagram->Data;

            // TBD: Always allocate the combining buffer even if it is just one unordered message
            // TBD: We could be compressing directly into this buffer in some cases
            if (!combined)
            {
                combined = (OutgoingQueuedDatagram*)WriteAllocator.Allocate(
                    protocol::kMaxPossibleDatagramByteLimit * 2);
                if (!combined) {
                    return Result::OutOfMemory();
                }
                combined->Initialize();

                // If there was some unreliable message data to combine:
                if (finalUnreliable)
                {
                    // Indicate that the reliable data starts at an offset
                    const unsigned unreliableBytes = finalUnreliable->NextWriteOffset;
                    reliableOffset = unreliableBytes;

                    // Combine final unreliable messages with first reliable messages
                    memcpy(combined->Data, finalUnreliable->Data, unreliableBytes);
                    combined->NextWriteOffset = unreliableBytes;

#ifdef TONK_DETAILED_STATS
                    combined->AccumulateStats(finalUnreliable);
#endif // TONK_DETAILED_STATS

                    // Prevent this from running again
                    WriteAllocator.Free(finalUnreliable);
                    finalUnreliable = nullptr;
                } // End if there was a final unreliable message to combine
            } // End if no 'combined' buffer allocated yet

            unsigned syncOverhead = 0;

            // If the data is in a reliable-in-order queue and compression is enabled:
            if (queueIndex > Queue_Unmetered && Deps.EnableCompression)
            {
                unsigned writtenBytes = 0;

                // Compress datagram to scratch space
                const Result compressResult = Compressor.Compress(
                    datagramData,
                    datagramBytes,
                    combined->Data + combined->NextWriteOffset + protocol::kMessageFrameBytes,
                    writtenBytes);
                if (compressResult.IsFail()) {
                    return compressResult;
                }

                // If compression succeeded:
                if (writtenBytes > 0)
                {
                    TONK_DEBUG_ASSERT(writtenBytes < datagramBytes);

                    // Write Compressed frame header
                    protocol::WriteMessageFrameHeader(
                        combined->Data + combined->NextWriteOffset,
                        protocol::MessageType_Compressed,
                        writtenBytes);
                    writtenBytes += protocol::kMessageFrameBytes;

                    // Clear previous compression failure
                    prevCompressFailed = false;

                    // If there is enough space to keep it in the combined datagram:
                    if (combined->NextWriteOffset + writtenBytes <= MaxMessageSectionBytes)
                    {
                        // Data is already in-place
                        combined->NextWriteOffset += writtenBytes;

                        // Keep track of compression savings
                        combined->CompressionSavings += datagramBytes + protocol::kMessageFrameBytes - writtenBytes;

#ifdef TONK_DETAILED_STATS
                        // TBD: Do not keep the original reliable message breakdown.
                        // Replace those counters with the smaller compressed sent byte count
                        combined->Stats[Stats_CompressedBytesSent] += writtenBytes - protocol::kMessageFrameBytes;
                        combined->Stats[Stats_FramingBytesSent] += protocol::kMessageFrameBytes;
                        combined->Stats[Stats_CompressionSavedBytes] += datagramBytes;
#endif // TONK_DETAILED_STATS

                        WriteAllocator.Free(datagram);
                        continue;
                    }

                    // Move the compressed data to a new combined datagram:

                    OutgoingQueuedDatagram* newCombined = (OutgoingQueuedDatagram*)WriteAllocator.Allocate(
                        protocol::kMaxPossibleDatagramByteLimit * 2);
                    if (!newCombined) {
                        return Result::OutOfMemory();
                    }
                    newCombined->Initialize();

                    memcpy(newCombined->Data, combined->Data + combined->NextWriteOffset, writtenBytes);
                    newCombined->NextWriteOffset = writtenBytes;

                    // Keep track of compression savings
                    newCombined->CompressionSavings = datagramBytes + protocol::kMessageFrameBytes - writtenBytes;

                    const Result result = shrinkWrapAndSendReliable(combined, reliableOffset);
                    if (result.IsFail()) {
                        return result;
                    }

                    combined = newCombined;
                    reliableOffset = 0;

#ifdef TONK_DETAILED_STATS
                    // TBD: Do not keep the original reliable message breakdown.
                    // Replace those counters with the smaller compressed sent byte count
                    combined->Stats[Stats_CompressedBytesSent] += writtenBytes - protocol::kMessageFrameBytes;
                    combined->Stats[Stats_FramingBytesSent] += protocol::kMessageFrameBytes;
#endif // TONK_DETAILED_STATS

                    WriteAllocator.Free(datagram);
                    continue;
                } // End if compression succeeded

                // If previous compression failed too:
                if (prevCompressFailed) {
                    // Receiver will need a frame injected to know where the
                    // buffer split is at.
                    syncOverhead = protocol::kMessageFrameBytes;
                }

                prevCompressFailed = true;
            } // End if compression should be attempted
            else {
                TONK_DEBUG_ASSERT(!prevCompressFailed); // Should not have compressed anything yet
            }

            // If there is not enough space:
            if (combined->NextWriteOffset + datagramBytes + syncOverhead > MaxMessageSectionBytes)
            {
                const Result result = shrinkWrapAndSendReliable(combined, reliableOffset);
                if (result.IsFail()) {
                    return result;
                }

                combined = (OutgoingQueuedDatagram*)WriteAllocator.Allocate(
                    protocol::kMaxPossibleDatagramByteLimit * 2);
                if (!combined) {
                    return Result::OutOfMemory();
                }
                combined->Initialize();

                // All data is reliable after the first reliable datagram
                reliableOffset = 0;

                // Reset the compression failed flag
                prevCompressFailed = false;
                syncOverhead = 0;
            }

            // If we need to write a frame to synchronize compression history:
            if (syncOverhead != 0)
            {
                // See the "Why a Sync Frame is Needed" note in the
                // PacketCompression.h header for why this is needed.
                protocol::WriteMessageFrameHeader(
                    combined->Data + combined->NextWriteOffset,
                    protocol::MessageType_Compressed,
                    0);

#ifdef TONK_DETAILED_STATS
                combined->Stats[Stats_FramingBytesSent] += protocol::kMessageFrameBytes;
#endif // TONK_DETAILED_STATS

                combined->NextWriteOffset += protocol::kMessageFrameBytes;
            } // End if adding sync frame

            // Copy the datagram or compressed data to the end of the combined buffer
            memcpy(
                combined->Data + combined->NextWriteOffset,
                datagramData,
                datagramBytes);

            combined->NextWriteOffset += datagramBytes;

#ifdef TONK_DETAILED_STATS
            combined->AccumulateStats(datagram);
#endif // TONK_DETAILED_STATS

            WriteAllocator.Free(datagram);
        } // Next datagram in queue
    } // Next priority queue

DoneSending:

    // If we have still not sent the final unreliable datagram:
    if (finalUnreliable) {
        sendQueuedUnreliable(finalUnreliable);
    }

    if (!combined) {
        return Result::Success();
    }

    // If we allocated a buffer we didn't end up needing:
    if (combined->NextWriteOffset <= 0)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        WriteAllocator.Free(combined);
        return Result::Success();
    }
    // Otherwise this will send at least one empty datagram during handshaking

    return shrinkWrapAndSendReliable(combined, reliableOffset);
}

void SessionOutgoing::SetPeerUDPAddress(const UDPAddress& addr)
{
    Locker locker(PrivateLock);

    PeerUDPAddress = addr;

    // Change the maximum datagram bytes appropriately
    if (addr.address().is_v6()) {
        UDPMaxDatagramBytes = protocol::kUDPIPV6_MaxDatagramBytes;
    }
    else {
        UDPMaxDatagramBytes = protocol::kUDPIPV4_MaxDatagramBytes;
    }

    // Cache the string form of the UDP address
    SetTonkAddressStr(CachedRemoteAddress, addr);

    HasPeerAddress = true;
}

void SessionOutgoing::StartHandshakeC2SConnect(uint64_t key)
{
    protocol::handshake::WriteC2SConnectionRequest(Handshake, key);
    ShouldAttachHandshake = true;
}

void SessionOutgoing::StartHandshakePeer2PeerConnect(uint64_t key)
{
    protocol::handshake::WriteP2PConnectionRequest(Handshake, key);
    ShouldAttachHandshake = true;
}

void SessionOutgoing::StartHandshakeC2SUpdateSourceAddress(uint64_t key, uint32_t id)
{
    protocol::handshake::WriteC2SUpdateSourceAddress(Handshake, key, id);
    ShouldAttachHandshake = true;
}

void SessionOutgoing::StopHandshake()
{
    ShouldAttachHandshake = false;
}

Result SessionOutgoing::PostRetransmit(bool& messageSent)
{
    messageSent = false;

    if (!HasPeerAddress)
    {
        Deps.Logger->Warning("PostRetransmit ignored before peer address is known");
        return Result::Success();
    }

    // If there is no space left:
    if (Deps.SenderControl->GetAvailableBytes() <= 0) {
        return Result::Success();
    }

    // Get the highest priority packet to retransmit from Siamese
    SiameseOriginalPacket original;
    const int retransmitResult = siamese_encoder_retransmit(
        FECEncoder,
        &original);
    // Note: The returned data is valid until siamese_encoder_ack(),
    // which is only called from this thread.

    // If there is no data to retransmit:
    if (retransmitResult == Siamese_NeedMoreData) {
        return Result::Success();
    }

    // If the Siamese call failed:
    if (retransmitResult != Siamese_Success) {
        return Result("SessionOutgoing::PostRetransmit", "siamese_retransmit failed", ErrorType::Siamese, retransmitResult);
    }

    TONK_DEBUG_ASSERT(original.DataBytes + protocol::kMaxOverheadBytes < UDPMaxDatagramBytes);

    /*
        It is not worth trying to embed retransmits with other outgoing data.

        Arguments against:

        (1) Complexity in protocol.
        (2) Complexity in decoder to make sure the data is processed in sequence.
        (3) Complexity to avoid extra delay in delivering the data.
        (4) Often times the original datagram was already max size and cannot
            be clustered anymore.
        (5) We already clustered messages into one big datagram when first sent
            so it's unlikely that we will gain advantage from more clustering.
        (6) Retransmits are even more rare than packet loss rate, so optimizing
            the bandwidth of these rare events is not going to move the needle.
    */

    // Allocate space for queued datagram
    OutgoingQueuedDatagram* datagram = (OutgoingQueuedDatagram*)WriteAllocator.Allocate(
        kQueueHeaderBytes + original.DataBytes + protocol::kMaxOverheadBytes + protocol::kMaxRandomPaddingBytes);
    if (!datagram) {
        return Result::OutOfMemory();
    }

    datagram->Initialize();

#ifdef TONK_DETAILED_STATS
    datagram->Stats[Stats_RetransmitSent] += original.DataBytes;
#endif // TONK_DETAILED_STATS

    // Copy data into place
    memcpy(datagram->Data, original.Data, original.DataBytes);
    datagram->NextWriteOffset = original.DataBytes;

    // No need to shrink the queued datagram here because it's already sized for the data

    // Send datagram immediately
    sendQueuedDatagram(
        datagram,
        getPacketNumBytes(original.PacketNum),
        original.PacketNum);

    messageSent = true;

    return Result::Success();
}

bool SessionOutgoing::PostDummyDatagram()
{
    if (!HasPeerAddress)
    {
        Deps.Logger->Warning("PostDummyDatagram ignored before peer address is known");
        return false;
    }

    TONK_DEBUG_ASSERT(protocol::kDummyBytes + protocol::kMaxOverheadBytes < UDPMaxDatagramBytes);

    // Allocate space for queued datagram
    OutgoingQueuedDatagram* datagram = (OutgoingQueuedDatagram*)WriteAllocator.Allocate(
        kQueueHeaderBytes + protocol::kDummyBytes + protocol::kMaxOverheadBytes + protocol::kMaxRandomPaddingBytes);
    if (!datagram) {
        return false;
    }

    // No need to call Initialize on the OutgoingQueuedDatagram object

    uint8_t* datagramData = datagram->Data;
    unsigned messageBytes = protocol::kDummyBytes;

    const static unsigned kZeroBytes = protocol::kDummyBytes - protocol::kMessageFrameBytes;
    protocol::WriteMessageFrameHeader(
        datagram->Data,
        protocol::MessageType_Padding,
        kZeroBytes);
    memset(datagram->Data + protocol::kMessageFrameBytes, 0, kZeroBytes);

    // The following code must be kept in sync with sendQueuedDatagram().
    // It is inlined and optimized for this common case:

    // No random padding

    uint8_t* footer = datagramData + messageBytes;
    uint8_t flags = protocol::kConnectionMask;
    if (ShouldCompressSequenceNumbers) {
        flags |= protocol::kSeqCompMask;
    }

    // No reliable sequence number

    // Write packet nonce to header
    unsigned nonceBytes;
    NonceT nonce = writeNonce(footer, nonceBytes);
    flags |= nonceBytes << 2;
    footer += nonceBytes;

    if (ShouldAttachHandshake)
    {
        flags |= protocol::kHandshakeMask;
        const uint32_t b = siamese::ReadU32_LE(Handshake + 8);
        const uint64_t a = siamese::ReadU64_LE(Handshake);
        siamese::WriteU64_LE(footer, a);
        siamese::WriteU32_LE(footer + 8, b);
        footer += protocol::kHandshakeBytes;
    }

    // Tag checksum includes all the data before the timestamp
    const unsigned taggedBytes = (unsigned)(footer - datagramData);

    // Skip over the timestamp
    uint8_t* timestampData = footer;
    footer += protocol::kTimestampBytes;

    // Write flags
    footer[0] = flags;
    ++footer;

    Encryptor.Cipher(datagramData, messageBytes, nonce);

    // Write tag
    uint16_t tag = Encryptor.Tag(datagramData, taggedBytes, nonce);
    static_assert(protocol::kEncryptionTagBytes == 2, "Update this if that changes");
    uint8_t* tagData = footer;

    const unsigned sendBytes = taggedBytes + protocol::kUntaggedDatagramBytes;

    DatagramBuffer datagramBuffer;
    datagramBuffer.AllocatorForFree = &WriteAllocator;
    datagramBuffer.Data = datagramData;
    datagramBuffer.FreePtr = datagram;
    datagramBuffer.Bytes = sendBytes;

    TONK_DEBUG_ASSERT(Deps.UDPSender != nullptr);
    TONK_DEBUG_ASSERT(HasPeerAddress);

#ifdef TONK_DETAILED_STATS
    DetailStats* stats = Deps.Deets->GetWritePtr();
    stats->Stats[Stats_DummyBytesSent] += messageBytes - protocol::kMessageFrameBytes;
    stats->Stats[Stats_FramingBytesSent] += protocol::kMessageFrameBytes;
    stats->Stats[Stats_FootersBytesSent] += sendBytes - messageBytes;
#endif // TONK_DETAILED_STATS

    // Write timestamp and final tag - Reduce noise in timestamp measurements
    const uint64_t nowUsec = siamese::GetTimeUsec();
    const uint32_t timestamp24 = (uint32_t)(nowUsec >> kTime23LostBits);
    tag ^= Encryptor.TagInt(protocol::TimestampFlagTagInt(timestamp24, flags));
    siamese::WriteU24_LE(timestampData, timestamp24);
    siamese::WriteU16_LE(tagData, tag);

    // Send the data
    Deps.UDPSender->Send(
        PeerUDPAddress,
        datagramBuffer,
        Deps.ConnectionRef);

    const unsigned kCompressionSavings = 0;
    const bool kNotReliable = false;

    // Update bandwidth control
    Deps.SenderControl->OnSendDatagram(
        kCompressionSavings,
        sendBytes,
        kNotReliable);

#ifdef TONK_ENABLE_VERBOSE_OUTGOING
    ++m_Recovery;
    Deps.Logger->Info("Recovery sent. Current send rate = ", (float)m_Recovery / (float)m_Reliable);
#endif // TONK_ENABLE_VERBOSE_OUTGOING

    return true;
}

Result SessionOutgoing::PostRecovery(int& bytesSentOut)
{
    bytesSentOut = 0;

    if (!HasPeerAddress)
    {
        Deps.Logger->Warning("PostRecovery ignored before peer address is known");
        return Result::Success();
    }

    // Generate a recovery packet using the Siamese FEC library.
    // The recovery data is only valid until the next call to siamese_encode()
    SiameseRecoveryPacket recovery;
    const int encodeResult = siamese_encode(FECEncoder, &recovery);

    if (encodeResult == Siamese_NeedMoreData) {
        return Result::Success();
    }

    if (encodeResult != Siamese_Success) {
        return Result("SessionOutgoing::PostRecovery", "siamese_encode failed", ErrorType::Siamese, encodeResult);
    }

    // Allocate space for queued datagram
    OutgoingQueuedDatagram* datagram = (OutgoingQueuedDatagram*)WriteAllocator.Allocate(
        kQueueHeaderBytes + recovery.DataBytes + protocol::kMaxOverheadBytes);
    if (!datagram) {
        return Result::OutOfMemory();
    }

    // No need to call Initialize on the OutgoingQueuedDatagram object

    uint8_t* datagramData = datagram->Data;
    unsigned messageBytes = recovery.DataBytes;

    /*
        This memory allocation and copy cannot be trivially eliminated.
        Hopefully it won't show up on a CPU profiler.

        I started working on it in the Siamese scratch-space branch.
        The problem is that the send timer does not tick fast enough to keep up
        with data rates above 1 MB/s, assuming that Asio completes one send per
        tick.  Tonk must queue up multiple FEC Recovery Datagrams per tick.
    */
    memcpy(datagramData, recovery.Data, messageBytes);

    // The following code must be kept in sync with sendQueuedDatagram().
    // It is inlined and optimized for this common case:

    // No random padding

    uint8_t* footer = datagramData + messageBytes;
    uint8_t flags = protocol::kOnlyFECMask | protocol::kConnectionMask;
    if (ShouldCompressSequenceNumbers) {
        flags |= protocol::kSeqCompMask;
    }

    // No reliable sequence number

    // Write packet nonce to header
    unsigned nonceBytes;
    NonceT nonce = writeNonce(footer, nonceBytes);
    flags |= nonceBytes << 2;
    footer += nonceBytes;

    if (ShouldAttachHandshake)
    {
        flags |= protocol::kHandshakeMask;
        const uint32_t b = siamese::ReadU32_LE(Handshake + 8);
        const uint64_t a = siamese::ReadU64_LE(Handshake);
        siamese::WriteU64_LE(footer, a);
        siamese::WriteU32_LE(footer + 8, b);
        footer += protocol::kHandshakeBytes;
    }

    // Tag checksum includes all the data before the timestamp
    const unsigned taggedBytes = (unsigned)(footer - datagramData);

    // Skip over the timestamp
    uint8_t* timestampData = footer;
    footer += protocol::kTimestampBytes;

    // Write flags
    footer[0] = flags;
    ++footer;

    Encryptor.Cipher(datagramData, messageBytes, nonce);

    // Prepare tag
    uint16_t tag = Encryptor.Tag(datagramData, taggedBytes, nonce);
    static_assert(protocol::kEncryptionTagBytes == 2, "Update this if that changes");
    uint8_t* tagData = footer;

    const unsigned sendBytes = taggedBytes + protocol::kUntaggedDatagramBytes;

    DatagramBuffer datagramBuffer;
    datagramBuffer.AllocatorForFree = &WriteAllocator;
    datagramBuffer.Data = datagramData;
    datagramBuffer.FreePtr = datagram;
    datagramBuffer.Bytes = sendBytes;

    TONK_DEBUG_ASSERT(Deps.UDPSender != nullptr);
    TONK_DEBUG_ASSERT(HasPeerAddress);

#ifdef TONK_DETAILED_STATS
    DetailStats* stats = Deps.Deets->GetWritePtr();
    stats->Stats[Stats_SiameseBytesSent] += messageBytes; // Flags bit means no message frame header
    stats->Stats[Stats_FootersBytesSent] += sendBytes - messageBytes;
#endif // TONK_DETAILED_STATS

    // Write timestamp and final tag - Reduce noise in timestamp measurements
    const uint64_t nowUsec = siamese::GetTimeUsec();
    const uint32_t timestamp24 = (uint32_t)(nowUsec >> kTime23LostBits);
    tag ^= Encryptor.TagInt(protocol::TimestampFlagTagInt(timestamp24, flags));
    siamese::WriteU24_LE(timestampData, timestamp24);
    siamese::WriteU16_LE(tagData, tag);

    // Send the data
    Deps.UDPSender->Send(
        PeerUDPAddress,
        datagramBuffer,
        Deps.ConnectionRef);

    const unsigned kCompressionSavings = 0;
    const bool kNotReliable = false;

    // Update bandwidth control
    Deps.SenderControl->OnSendDatagram(
        kCompressionSavings,
        sendBytes,
        kNotReliable);

    // Report number of bytes sent
    bytesSentOut = static_cast<int>(sendBytes);

#ifdef TONK_ENABLE_VERBOSE_OUTGOING
    ++m_Recovery;
    Deps.Logger->Info("Recovery sent. Current send rate = ", (float)m_Recovery / (float)m_Reliable);
#endif // TONK_ENABLE_VERBOSE_OUTGOING

    return Result::Success();
}

Result SessionOutgoing::OnAcknowledgements(
    const uint8_t* data,
    unsigned bytes,
    Counter64 ackNonce,
    uint64_t receiveUsec)
{
    if (bytes < kAckOverheadBytes) {
        return Result("SessionOutgoing::OnAcknowledgements", "Truncated Ack", ErrorType::Tonk, Tonk_InvalidData);
    }

    // If ack was received out of order:
    if (ackNonce < LastAck.NextAcceptableNonce)
    {
        Deps.Logger->Debug("Ignored re-ordered ack");
        return Result::Success();
    }
    LastAck.NextAcceptableNonce = ackNonce + 1;

    // Decompress nonce field back to full size
    const Counter24 partialNonce = siamese::ReadU24_LE(data);
    const Counter64 recovered = Counter64::ExpandFromTruncated(LastAck.PeerNextExpectedNonce, partialNonce);

    // If ack was sent out of order:
    if (recovered < LastAck.PeerNextExpectedNonce) {
        return Result("SessionOutgoing::OnAcknowledgements", "Ack was sent out of order", ErrorType::Tonk, Tonk_InvalidData);
    }

    // If there is Ack data attached:
    if (bytes > kAckOverheadBytes)
    {
        // Process acknowledgement with encoder
        const int ackResult = siamese_encoder_ack(
            FECEncoder,
            data + kAckOverheadBytes,
            bytes - kAckOverheadBytes,
            &LastAck.PeerNextExpectedSeqNum);

        if (Siamese_Success != ackResult) {
            return Result("SessionOutgoing::OnAcknowledgements", "siamese_encoder_ack failed", ErrorType::Siamese, ackResult);
        }
    }

    // Message is validated - update state
    LastAck.PeerNextExpectedNonce = recovered;
    LastAck.ReceiveUsec = receiveUsec;

    // Update bandwidth control with realtime feedback
    BandwidthShape shape;
    shape.Decompress(data + 3);
    Deps.SenderControl->OnReceiverFeedback(shape);

    // If the peer is now caught up:
    if (PeerCaughtUp())
    {
        TONK_VERBOSE_OUTGOING_LOG("Peer caught up - sending AckAck for seqno=", LastAck.PeerNextExpectedSeqNum);

        // Send an AckAck
        uint8_t ackack[4];
        siamese::WriteU24_LE_Min4Bytes(ackack, LastAck.PeerNextExpectedSeqNum);

        return QueueUnreliable(
            protocol::MessageType_AckAck,
            ackack,
            protocol::kAckAckBytes);
    }

    TONK_VERBOSE_OUTGOING_LOG("Got Ack PeerNextExpectedSeq=", LastAck.PeerNextExpectedSeqNum,
        " PeerNextExpectedNonce=", LastAck.PeerNextExpectedNonce.ToUnsigned(), " bytes=", bytes);

    return Result::Success();
}

void SessionOutgoing::UpdateNextExpectedNonceFromTimeSync(Counter24 nextExpectedNonce24)
{
    // Recover the full 64-bit nonce
    Counter64 recovered = Counter64::ExpandFromTruncated(LastAck.PeerNextExpectedNonce, nextExpectedNonce24);

    // Only use it if it's newer than the last acknowledgement
    if (LastAck.PeerNextExpectedNonce < recovered) {
        LastAck.PeerNextExpectedNonce = recovered;
    }
}


//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void SessionOutgoing::tonk_status(TonkStatus& statusOut) const
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    unsigned unreliableQueueDepthBytes, reliableQueueDepthBytes, lowpriQueueDepthBytes;

    {
        Locker locker(OutgoingQueueLock);

        unreliableQueueDepthBytes = Queues[Queue_Unreliable].GetQueuedBytes();
        reliableQueueDepthBytes = Queues[Queue_Unmetered].GetQueuedBytes()
            + Queues[Queue_Reliable].GetQueuedBytes();
        lowpriQueueDepthBytes = Queues[Queue_LowPri].GetQueuedBytes();
    }

    // Reliable queue is behind unreliable queue
    reliableQueueDepthBytes += unreliableQueueDepthBytes;

    // Calculate reliable queue depth in milliseconds
    statusOut.ReliableQueueMsec = Deps.SenderControl->CalculateSendTimeMsec(
        reliableQueueDepthBytes);

    // Lowpri queue is behind reliable and unreliable queues
    lowpriQueueDepthBytes += reliableQueueDepthBytes;

    // Calculate lowpri queue depth in milliseconds
    statusOut.LowPriQueueMsec = Deps.SenderControl->CalculateSendTimeMsec(
        lowpriQueueDepthBytes);

    // Read BPS from bandwidth control
    statusOut.AppBPS = Deps.SenderControl->GetAppBPS();
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

void SessionOutgoing::tonk_status_ex(TonkStatusEx& statusExOut) const
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    Locker locker(PrivateLock);

    statusExOut.Remote = CachedRemoteAddress;
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

Result SessionOutgoing::queueMessage(
    unsigned queueIndex,
    protocol::MessageType messageType,
    const uint8_t* data,
    size_t bytes)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    static_assert(TONK_MAX_UNORDERED_BYTES == TONK_MAX_UNRELIABLE_BYTES, "Update this");

    // This function only supports a few types of messages: Unreliable, Unordered, Control
    TONK_DEBUG_ASSERT(
        messageType < protocol::MessageType_StartOfReliables ||
        messageType >= protocol::MessageType_Control);

    // Message types must be in the proper queues
    TONK_DEBUG_ASSERT(
        (messageType < protocol::MessageType_StartOfReliables && queueIndex == Queue_Unreliable) ||
        (messageType == protocol::MessageType_Control && queueIndex == Queue_Unmetered) ||
        (messageType == protocol::MessageType_Unordered && queueIndex == Queue_Unmetered)
    );

    // Check for oversized messages without any additions up front to avoid
    // integer overflow issues
    if (bytes >= TONK_MAX_UNRELIABLE_BYTES)
    {
        return Result("SessionOutgoing::queueMessage",
            std::to_string(bytes) + " byte message too large",
            ErrorType::Tonk,
            Tonk_MessageTooLarge);
    }

    Locker locker(OutgoingQueueLock);

    TONK_DEBUG_ASSERT(TONK_MAX_UNRELIABLE_BYTES < MaxMessageSectionBytes);

    OutgoingQueue* queue = &Queues[queueIndex];

    // Try #1 : Append to existing buffer - Fails if too big or not created yet
    // Try #2 : Append to fresh buffer
    for (int tries = 0; tries < 2; ++tries)
    {
        // Attempt to append a message to the queue's workspace buffer
        if (queue->Append(messageType, data, static_cast<unsigned>(bytes))) {
            return Result::Success();
        }

        // Need more space- Push the workspace onto the queue and try again
        const Result result = queue->PushAndGetFreshBuffer();
        if (result.IsFail()) {
            return result;
        }
    }

    TONK_DEBUG_BREAK(); // Should never get here
    return Result("SessionOutgoing::queueMessage",
        std::to_string(bytes) + " byte message caused Append failure",
        ErrorType::Tonk,
        Tonk_Error);
}

Result SessionOutgoing::QueueUnreliable(
    protocol::MessageType messageType,
    const uint8_t* data,
    size_t bytes)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    return queueMessage(Queue_Unreliable, messageType, data, bytes);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

Result SessionOutgoing::QueueUnordered(
    const uint8_t* data,
    size_t bytes)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    // Unordered types go into the same queue as unreliable types.
    // Unordered queue will bypass bandwidth limits and will not be compressed
    return queueMessage(Queue_Unmetered, protocol::MessageType_Unordered, data, bytes);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

Result SessionOutgoing::QueueControl(
    const uint8_t* data,
    size_t bytes)
{
    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    // Control messages are reliable in-order but get put in the queue with
    // Unordered data so that it can be reliable and bypass bandwidth limits.
    // As a side-effect these control messages are not compressed
    return queueMessage(Queue_Unmetered, protocol::MessageType_Control, data, bytes);
}

//------------------------------------------------------------------------------
// Public API -- Not Threadsafe
//------------------------------------------------------------------------------

Result SessionOutgoing::QueueReliable(
    unsigned messageType,
    const uint8_t* data,
    size_t bytes)
{
    // Note: Compressed type is generated internally by Flush()
    TONK_DEBUG_ASSERT(messageType > protocol::MessageType_Compressed);
    // Control/Unordered must be queued via QueueControl()/QueueUnordered()
    TONK_DEBUG_ASSERT(messageType < protocol::MessageType_Control);

    // WARNING: This function is not called on the Connection green thread,
    // so accessing members here is not threadsafe by default.

    // Check for oversized messages without any additions up front to avoid
    // integer overflow issues
    if (bytes > TONK_MAX_RELIABLE_BYTES)
    {
        return Result("SessionOutgoing::QueueReliableMessage",
            std::to_string(bytes) + " byte message too large",
            ErrorType::Tonk,
            Tonk_MessageTooLarge);
    }

    const unsigned queueIndex = (messageType >= protocol::MessageType_LowPri)
        ? Queue_LowPri : Queue_Reliable;

    Locker locker(OutgoingQueueLock);

    OutgoingQueue* queue = &Queues[queueIndex];

    for (;;)
    {
        // Attempt to append message to OutBuffer
        const size_t written = queue->AppendSplitReliable(messageType, data, bytes);
        TONK_DEBUG_ASSERT(written <= bytes);
        if (written >= bytes) {
            break; // Done
        }

        data += written, bytes -= written;

        // Push the OutBuffer to the send queue and start a new OutBuffer
        const Result result = queue->PushAndGetFreshBuffer();
        if (result.IsFail()) {
            TONK_DEBUG_BREAK();
            return result;
        }
    }

    return Result::Success();
}


} // namespace tonk
