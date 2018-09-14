/** \file
    \brief Tonk Implementation: Incoming Data Processor
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

#include "TonkineseIncoming.h"

namespace tonk {

static logger::Channel ModuleLogger("Incoming", MinimumLogLevel);

#ifdef TONK_ENABLE_VERBOSE_INCOMING
    #define TONK_VERBOSE_INCOMING_LOG(...) ModuleLogger.Info(__VA_ARGS__);
#else
    #define TONK_VERBOSE_INCOMING_LOG(...) ;
#endif


//------------------------------------------------------------------------------
// SessionIncoming

Result SessionIncoming::Initialize(
    const Dependencies& deps,
    uint64_t key,
    const TonkConnectionConfig* config,
    TonkConnection connection)
{
    Deps = deps;
    Connection = connection;
    Config = config;

    Decryptor.Initialize(key);
    AntiReplay.Reset();

    FECDecoder = siamese_decoder_create();
    if (!FECDecoder) {
        return Result("SessionIncoming::Initialize", "siamese_decoder_create failed", ErrorType::Siamese);
    }

    return Decompressor.Initialize(protocol::kCompressionAllocateBytes);
}

void SessionIncoming::ChangeEncryptionKey(uint64_t key)
{
    Decryptor.Initialize(key);
}

void SessionIncoming::Shutdown()
{
    uint64_t stats[SiameseDecoderStats_Count];

    const SiameseResult result = siamese_decoder_stats(
        FECDecoder,
        stats,
        SiameseDecoderStats_Count);

    if (result != Siamese_Success) {
        Deps.Logger->Error("Unable to retrieve siamese decoder stats");
    }
    else {
        Deps.Logger->Debug("Siamese decoder stats:" \
            " Memory used = ", stats[SiameseDecoderStats_MemoryUsed] / 1000, " KB" \
            ", Solve successs = ", stats[SiameseDecoderStats_SolveSuccessCount],
            ", Solve failures = ", stats[SiameseDecoderStats_SolveFailCount]);
    }

    siamese_decoder_free(FECDecoder);
    FECDecoder = nullptr;
}

Result SessionIncoming::ProcessDatagram(
    const uint64_t receiveUsec,
    const UDPAddress& addr,
    uint8_t* data,
    const unsigned bytes)
{
    // If this Connection is disconnected, then stop processing new datagrams
    // that arrive.  This prevents the OnData() callback from firing after the
    // OnClose() callback completes for the application.
    if (Deps.ConnectionRef->IsShutdown())
    {
        Deps.Logger->Warning("Ignoring incoming datagram during disco");
        return Result::Success();
    }

    // Read flags
    const uint8_t* footer = data + bytes - protocol::kFlagsBytes - protocol::kEncryptionTagBytes;
    protocol::FlagsReader flags = footer[0];

    // Read 24-bit remote send timestamp
    footer -= protocol::kTimestampBytes;
    const Counter24 remoteSendTS24 = siamese::ReadU24_LE_Min4Bytes(footer);

    // Read handshake
    const unsigned handshakeBytes = flags.HandshakeBytes();
    if (handshakeBytes > 0)
    {
        footer -= handshakeBytes;

        HandshakeResult result = Deps.IncomingHandler->OnUnauthenticatedHandshake(data, bytes);
        if (HandshakeResult::DropDatagram == result) {
            return Result::Success();
        }
    }

    // Read nonce
    const unsigned nonceBytes = flags.EncryptionNonceBytes();
    if (nonceBytes <= 0) { // Unauthenticated - Must return Tonk_BogonData
        return Result("SessionIncoming::ProcessDatagram", "No nonce", ErrorType::Tonk, Tonk_BogonData);
    }

    const uint32_t partialNonce = protocol::ReadFooterField(footer, nonceBytes);
    const uint64_t nonce = AntiReplay.Expand(partialNonce, nonceBytes).ToUnsigned();
    footer -= nonceBytes;

    // If nonce is duplicated:
    if (AntiReplay.IsDuplicate(nonce)) // Unauthenticated - Must return Tonk_BogonData
    {
#if 0
        ModuleLogger.Debug("Nonce was duplicated: nonceBytes=", nonceBytes,
            " partialNonce=", HexString(partialNonce), " nonce=", HexString(nonce));
#endif
        return Result("SessionIncoming::ProcessDatagram", "Ignored dupe", ErrorType::Tonk, Tonk_BogonData);
    }

    // Check if the checksum is correct
    const uint16_t expectedTag = Decryptor.Tag(data, bytes - protocol::kUntaggedDatagramBytes, nonce);
    const uint16_t tag = siamese::ReadU16_LE(data + bytes - protocol::kEncryptionTagBytes);
    const uint32_t flagsInt = protocol::TimestampFlagTagInt(remoteSendTS24.ToUnsigned(), flags.FlagsByte);
    if ((tag ^ Decryptor.TagInt(flagsInt)) != expectedTag) {
        return Result("SessionIncoming::ProcessDatagram", "Bad checksum", ErrorType::Tonk, Tonk_BogonData);
    }
    static_assert(protocol::kEncryptionTagBytes == 2, "Update this if that changes");

    // Accept this nonce
    AntiReplay.Accept(nonce);

    // Calculate number of bytes in the message payload part
    const unsigned messagePayloadBytes = bytes - flags.ConnectedFooterBytes();

    // Decrypt message payload
    Decryptor.Cipher(data, messagePayloadBytes, nonce);

//#define TONK_DUMP_INCOMING_DATAGRAMS /* WOO */
#ifdef TONK_DUMP_INCOMING_DATAGRAMS
    std::stringstream ss;
    ss << "RECV(" << bytes << " bytes) @ " << receiveUsec % 1000000 <<  ": ";
    for (size_t i = 0; i < bytes; ++i)
    {
        static const char* H = "0123456789abcdef";
        ss << H[data[i] >> 4] << H[data[i] & 15] << " ";
    }
    if (flags.ConnectedFooterBytes() > 0)
        ss << "Footer=" << flags.ConnectedFooterBytes() << " ";
    if (flags.TimestampBytes() > 0)
        ss << "Timestamp=" << flags.TimestampBytes() << " ";
    if (flags.EncryptionNonceBytes() > 0)
        ss << "Nonce=" << flags.EncryptionNonceBytes() << " ";
    if (flags.HandshakeBytes() > 0)
        ss << "Handshake=" << flags.HandshakeBytes() << " ";
    if (flags.SequenceNumberBytes() > 0)
        ss << "Seq=" << flags.SequenceNumberBytes() << " ";
    ModuleLogger.Error(ss.str());
#endif // TONK_DUMP_INCOMING_DATAGRAMS

    // Update time synchronization subsystem
    const unsigned networkTripUsec = Deps.TimeSync->OnAuthenticatedDatagramTimestamp(
        remoteSendTS24,
        receiveUsec);

    // Number of bytes in the reliable sequence number
    const unsigned seqBytes = flags.SequenceNumberBytes();

    // The seqBytes can be 0 for FEC recovery data
    const uint32_t partialSeq = protocol::ReadFooterField(footer, seqBytes);
    footer -= seqBytes;

    bool inSequence = false, firstSeenOutOfSequence = false, checkedFSOOS = false;
    unsigned relseqNum = 0;

    if (seqBytes > 0)
    {
        // Decompress sequence number
        CounterSeqNo nextExpectedSeq = FECNextExpectedPacketNum;
        if (seqBytes == 1) {
            relseqNum = CounterSeqNo::ExpandFromTruncated(
                nextExpectedSeq,
                Counter8((uint8_t)partialSeq)).ToUnsigned();
        }
        else if (seqBytes == 2) {
            relseqNum = CounterSeqNo::ExpandFromTruncated(
                nextExpectedSeq,
                Counter16((uint16_t)partialSeq)).ToUnsigned();
        }
        else { // seqBytes == 3
            relseqNum = partialSeq;
        }

        // Check if reliable data is in sequence or not
        inSequence = (FECNextExpectedPacketNum == relseqNum);

        //AckRequired = true;
    }

    // React to an authenticated datagram
    Deps.IncomingHandler->OnAuthenticatedDatagram(addr, receiveUsec);

    // Keep track of what chunk of the data is reliable (protected by FEC/ARQ)
    unsigned reliableStartOffset = 0, reliableEndOffset = 0;

    siamese::ReadByteStream frameReader(data, messagePayloadBytes);

    // If this datagram only contains FEC recovery message payload:
    if (flags.IsOnlyFEC())
    {
        Result result = onRecovery(data, messagePayloadBytes);
        if (result.IsFail()) {
            return result;
        }

        // Skip over message processing
        goto SkipMessageProcessing;
    }

    // Reset the compression state machine
    FirstReliable = nullptr;

    // Process all messages contained in the datagram:
    while (frameReader.Remaining() >= protocol::kMessageFrameBytes)
    {
        /*
            Keep track of the frame start in case this is the first reliable
            message.  Incoming reliable data is carved out of incoming datagrams
            and it is stored in the Siamese FEC library.  Later that data is
            recalled when missing data is recovered (via FEC) or received
            (via ACK+retransmit), and only the reliable messages are processed.
        */
        const unsigned frameStartOffset = frameReader.BytesRead;

        // Read message frame header
        unsigned messageType = frameReader.Read16();
        const unsigned messageBytes = messageType & protocol::kMessageLengthMask;

        if (frameReader.Remaining() < messageBytes) {
            return Result("SessionIncoming::ProcessDatagram", "Truncated message", ErrorType::Tonk, Tonk_InvalidData);
        }

        messageType >>= protocol::kMessageLengthBits;
        const uint8_t* messageData = frameReader.Read(messageBytes);

        /*
            This code path is for processing data as soon as it has arrived, and
            as fast as possible.  The onRecoveredReliableData() function handles
            data that is recovered using FEC, so it needs to be kept in sync and
            any fixes applied to this part should also be applied there.
        */

        if (messageType >= protocol::MessageType_StartOfReliables)
        {
            if (messageType == protocol::MessageType_Unordered)
            {
                // If we have not checked if this is the first time we have seen this message:
                if (!checkedFSOOS)
                {
                    // Check now
                    checkedFSOOS = true;

                    // See if Siamese already has the next one in sequence
                    SiameseOriginalPacket outOfOrderOriginal;
                    outOfOrderOriginal.PacketNum = relseqNum;
                    const int getResult = siamese_decoder_get(FECDecoder, &outOfOrderOriginal);

                    // If this is the first time we have seen this data:
                    if (getResult == Siamese_NeedMoreData) {
                        firstSeenOutOfSequence = true;
                    }
                    else if (getResult != Siamese_Success) {
                        return Result("SessionIncoming::ProcessDatagram", "siamese_decoder_get failed", ErrorType::Siamese, getResult);
                    }
                }

                // If this is the first time we have seen this message:
                if (firstSeenOutOfSequence)
                {
                    // Handle it immediately
                    Config->OnData(
                        Config->AppContextPtr,
                        Connection,
                        TonkChannel_Unordered,
                        messageData,
                        messageBytes);
                }
            }
            else if (inSequence)
            {
                const Result result = handleReliableMessage(
                    messageType,
                    messageData,
                    messageBytes);
                if (result.IsFail()) {
                    return result;
                }
                // Note: The decompression state handled via `FirstReliable`
                // must only be updated when messages are in sequence
            }

            // If we haven't seen any reliable data yet:
            if (reliableEndOffset == 0) {
                reliableStartOffset = frameStartOffset;
            }

            // Keep track of where reliable data ends
            reliableEndOffset = frameReader.BytesRead;

            continue;
        }

        if (messageType == protocol::MessageType_Unreliable)
        {
            Config->OnData(
                Config->AppContextPtr,
                Connection,
                TonkChannel_Unreliable,
                messageData,
                messageBytes);
        }
        else if (messageType == protocol::MessageType_Acknowledgements)
        {
            Result result = Deps.Outgoing->OnAcknowledgements(
                messageData,
                messageBytes,
                nonce,
                receiveUsec);
            if (result.IsFail()) {
                return result;
            }
        }
        else if (messageType == protocol::MessageType_TimeSync)
        {
            Result result = Deps.IncomingHandler->OnTimeSync(
                messageData,
                messageBytes);
            if (result.IsFail()) {
                return result;
            }
        }
        else if (messageType == protocol::MessageType_AckAck)
        {
            if (messageBytes != protocol::kAckAckBytes) {
                return Result("SessionIncoming::ProcessDatagram", "Invalid ackack", ErrorType::Tonk, Tonk_InvalidData);
            }
            LastAckAckSeq = siamese::ReadU24_LE_Min4Bytes(messageData);
        }
        else // Other types:
        {
            TONK_DEBUG_ASSERT(messageType == protocol::MessageType_Padding);

            // Note: Currently only the Padding type will follow reliable data,
            // so we only need to insert uncompressed data if we see padding
            // or other types here
            insertUncompressed(messageData - protocol::kMessageFrameBytes);
        }
        // Ignore other types..
    } // Next message frame

    // If the message payload was not completely used:
    if (frameReader.Remaining() > 0)
    {
        TONK_DEBUG_BREAK(); // Invalid data
        return Result("SessionIncoming::ProcessDatagram", "Invalid frame", ErrorType::Tonk, Tonk_InvalidData);
    }

    // Insert remaining uncompressed bytes
    insertUncompressed(frameReader.Peek());

SkipMessageProcessing:

    DatagramInfo info;
    info.DatagramBytes = bytes + protocol::kUDPIPV4_HeaderBytes; // Include UDP/IP header
    info.Nonce = nonce;
    info.ReceiveUsec = receiveUsec;
    info.SendTS24 = remoteSendTS24;
    info.NetworkTripUsec = networkTripUsec;

    // Calculate ReceiveSendDeltaTS24:
    {
        const Counter32 sendTimeTS24 = Counter32::ExpandFromTruncated(LargestSendTS24, remoteSendTS24);
        if (LargestSendTS24 < sendTimeTS24) {
            LargestSendTS24 = sendTimeTS24;
        }
        const Counter32 recvTimeTS24 = (uint32_t)(receiveUsec >> kTime23LostBits);
        info.ReceiveSendDeltaTS24 = recvTimeTS24 - sendTimeTS24;
    }

#ifdef TONK_DETAILED_STATS
    DetailStats* deets = Deps.Deets->GetWritePtr();
    deets->Stats[Stats_BytesReceived] += bytes;
    if (deets->Stats[Stats_MinUsecOWD] == 0 || deets->Stats[Stats_MinUsecOWD] > networkTripUsec) {
        deets->Stats[Stats_MinUsecOWD] = networkTripUsec;
    }
    if (deets->Stats[Stats_MaxUsecOWD] == 0 || deets->Stats[Stats_MaxUsecOWD] < networkTripUsec) {
        deets->Stats[Stats_MaxUsecOWD] = networkTripUsec;
    }
#endif // TONK_DETAILED_STATS

    // Update bandwidth control
    Deps.ReceiverControl->OnProcessedDatagram(info);

    // If no data was reliable:
    if (reliableEndOffset <= reliableStartOffset) {
        return Result::Success();
    }

    // If reliable data was seen but no sequence number was attached:
    if (seqBytes == 0) {
        return Result("SessionIncoming::ProcessDatagram", "Reliable data sent with no sequence number", ErrorType::Tonk, Tonk_InvalidData);
    }

    SiameseOriginalPacket original;
    original.Data      = data + reliableStartOffset;
    original.DataBytes = reliableEndOffset - reliableStartOffset;
    original.PacketNum = relseqNum;

    // Add original data regardless of whether or not it was in sequence
    return decoderAddOriginal(original);
}

Result SessionIncoming::onCompressed(
    const uint8_t* data,
    unsigned bytes)
{
    if (bytes <= 0) {
        return Result::Success();
    }

    Decompressed decompressed;
    Result decompressResult = Decompressor.Decompress(
        data,
        bytes,
        decompressed);
    if (decompressResult.IsFail()) {
        return decompressResult;
    }

    siamese::ReadByteStream frameReader(decompressed.Data, decompressed.Bytes);

    // Process all messages contained in the compressed payload:
    while (frameReader.Remaining() >= protocol::kMessageFrameBytes)
    {
        // Read message frame header
        unsigned messageType = frameReader.Read16();
        const unsigned messageBytes = messageType & protocol::kMessageLengthMask;

        if (frameReader.Remaining() < messageBytes) {
            TONK_DEBUG_BREAK();
            return Result("SessionIncoming::decompress", "Truncated message", ErrorType::Tonk, Tonk_InvalidData);
        }

        messageType >>= protocol::kMessageLengthBits;
        const uint8_t* messageData = frameReader.Read(messageBytes);

        TONK_DEBUG_ASSERT(messageType >= protocol::MessageType_StartOfReliables);
        TONK_DEBUG_ASSERT(messageType != protocol::MessageType_Unordered);

        const Result deliveryResult = deliverMessage(messageType, messageData, messageBytes);
        if (deliveryResult.IsFail()) {
            return deliveryResult;
        }
    }

    // If the message payload was not completely used:
    if (frameReader.Remaining() > 0) {
        TONK_DEBUG_BREAK(); // Invalid data
        return Result("SessionIncoming::decompress", "Invalid frame", ErrorType::Tonk, Tonk_InvalidData);
    }

    return Result::Success();
}

Result SessionIncoming::onRecovery(
    const uint8_t* messageData,
    unsigned messageBytes)
{
    SiameseRecoveryPacket recovery;
    recovery.Data      = messageData;
    recovery.DataBytes = messageBytes;

    const int addRecoveryResult = siamese_decoder_add_recovery(FECDecoder, &recovery);
    if (addRecoveryResult != Siamese_Success) {
        return Result("SessionIncoming::OnRecovery", "siamese_decoder_add_recovery failed", ErrorType::Siamese, addRecoveryResult);
    }

    return attemptDecode();
}

Result SessionIncoming::attemptDecode()
{
    // Run decode loop
    for (;;)
    {
        // Check if decoder is ready
        const int readyResult = siamese_decoder_is_ready(FECDecoder);

        if (readyResult == Siamese_NeedMoreData) {
            break;
        }

        if (readyResult != Siamese_Success)
        {
            Result result = Result("SessionIncoming::decoderAddOriginal", "siamese_decoder_is_ready failed", ErrorType::Siamese, readyResult);
            Deps.Logger->Error("siamese_decoder_is_ready failed: ", result.ToJson());
            return result;
        }

        SiameseOriginalPacket* recoveredOriginals = nullptr;
        unsigned recoveredOriginalCount = 0;

        // Attempt to decode
        const int decodeResult = siamese_decode(FECDecoder, &recoveredOriginals, &recoveredOriginalCount);

        // Handle any Unordered/Unreliable messages as they are decoded
        for (unsigned i = 0; i < recoveredOriginalCount; ++i) {
            onSiameseSolved(recoveredOriginals[i]);
        }

        if (decodeResult == Siamese_NeedMoreData)
        {
            uint64_t stats[SiameseDecoderStats_Count];
            siamese_decoder_stats(FECDecoder, stats, SiameseDecoderStats_Count);

            uint64_t solveFailCount = stats[SiameseDecoderStats_SolveFailCount];
            uint64_t successCount = stats[SiameseDecoderStats_SolveSuccessCount];

            float failRate = 0.f;
            if (solveFailCount > 0) {
                failRate = solveFailCount / (float)(solveFailCount + successCount);
                TONK_DEBUG_ASSERT(failRate < 0.8f)
            }
            Deps.Logger->Debug("Recovery failed and needs more data (rare): Failure rate = ", failRate, " solveFailCount=", solveFailCount, " successCount = ", successCount);

            break;
        }

        if (decodeResult != Siamese_Success)
        {
            Result result = Result("SessionIncoming::decoderAddOriginal", "siamese_decode failed", ErrorType::Siamese, decodeResult);
            Deps.Logger->Error("siamese_decode failed: ", result.ToJson());
            return result;
        }

        //Deps.Logger->Debug("SIAMESE RECOVERED: ", recoveredOriginalCount);

        // Resume processing
        Result result = resumeProcessing();
        if (result.IsFail()) {
            return result;
        }
    }

    return Result::Success();
}

Result SessionIncoming::resumeProcessing()
{
    // Run processing loop
    for (;;)
    {
        SiameseOriginalPacket packet;
        packet.PacketNum = FECNextExpectedPacketNum;

        // See if we already have the next one in sequence
        const int getResult = siamese_decoder_get(FECDecoder, &packet);

        if (getResult == Siamese_NeedMoreData) {
            break;
        }

        if (getResult != Siamese_Success)
        {
            const Result result = Result("SessionIncoming::resumeProcessing", "siamese_decoder_get failed", ErrorType::Siamese, getResult);
            Deps.Logger->Error("siamese_decoder_get failed: ", result.ToJson());
            TONK_DEBUG_BREAK();
            return result;
        }

        // Reset the compression state machine
        FirstReliable = nullptr;

        // Increment the next expected packet number
        FECNextExpectedPacketNum = SIAMESE_PACKET_NUM_INC(FECNextExpectedPacketNum);

        siamese::ReadByteStream frameReader(packet.Data, packet.DataBytes);

        // Process all messages contained in the datagram:
        while (frameReader.Remaining() >= protocol::kMessageFrameBytes)
        {
            // Read message frame header
            unsigned messageType = frameReader.Read16();
            const unsigned messageBytes = messageType & protocol::kMessageLengthMask;

            if (frameReader.Remaining() < messageBytes) {
                TONK_DEBUG_BREAK();
                return Result("SessionIncoming::resumeProcessing", "Truncated message", ErrorType::Tonk, Tonk_InvalidData);
            }

            messageType >>= protocol::kMessageLengthBits;
            const uint8_t* messageData = frameReader.Read(messageBytes);

            /*
                When processing data at this stage, unreliable and unordered data
                has already been delivered for sure, so we can skip it here.

                Unordered (reliable) data specifically might arrive five ways:

                (1) In sequence -> Delivered right away
                (2) Out of sequence original (not seen yet) -> Delivered right away
                (3) Out of sequence original (but seen already) -> Not delivered
                (4) Recovered by FEC (not seen yet) -> Delivered right away
                (5) After FEC recovery (already seen) -> Not delivered

                Cases (3) and (5) are what we are handling with this check.
            */
            if (messageType < protocol::MessageType_StartOfReliables ||
                messageType == protocol::MessageType_Unordered)
            {
                continue;
            }

            const Result result = handleReliableMessage(
                messageType,
                messageData,
                messageBytes);
            if (result.IsFail()) {
                return result;
            }
        }

        // If the message payload was not completely used:
        if (frameReader.Remaining() > 0)
        {
            TONK_DEBUG_BREAK(); // Invalid data
            return Result("SessionIncoming::resumeProcessing", "Invalid frame", ErrorType::Tonk, Tonk_InvalidData);
        }

        // Insert remaining uncompressed bytes
        insertUncompressed(frameReader.Peek());
    }

    return Result::Success();
}

void SessionIncoming::insertUncompressed(const uint8_t* end)
{
    const uint8_t* start = FirstReliable;

    if (!start) {
        return;
    }

    const uintptr_t count = (uintptr_t)(end - start);
    TONK_DEBUG_ASSERT(count <= protocol::kMaxPossibleDatagramByteLimit);

    Decompressor.InsertUncompressed(start, (unsigned)count);

    FirstReliable = nullptr;
}

Result SessionIncoming::handleReliableMessage(
    unsigned messageType,
    const uint8_t* messageData,
    unsigned messageBytes)
{
    TONK_DEBUG_ASSERT(messageType >= protocol::MessageType_StartOfReliables);
    TONK_DEBUG_ASSERT(messageType != protocol::MessageType_Unordered);

    if (messageType == protocol::MessageType_Compressed)
    {
        TONK_VERBOSE_INCOMING_LOG("Handle compressed message block. messageBytes=", messageBytes);

        insertUncompressed(messageData - protocol::kMessageFrameBytes);

        return onCompressed(messageData, messageBytes);
    }

    // Remember the first reliable message that is not Control or Unordered.
    // These types are not included in the compression history buffer
    if (!FirstReliable && messageType < protocol::MessageType_Control) {
        FirstReliable = messageData - protocol::kMessageFrameBytes;
    }

    TONK_VERBOSE_INCOMING_LOG("Handle reliable messageType=", messageType, ". messageBytes=", messageBytes);

    return deliverMessage(messageType, messageData, messageBytes);
}

Result SessionIncoming::decoderAddOriginal(const SiameseOriginalPacket& original)
{
    // Add original packet data to Siamese decoder instance
    const int addOriginalResult = siamese_decoder_add_original(FECDecoder, &original);

    if (addOriginalResult == Siamese_DuplicateData)
    {
        // Ignore duplicate data.
        // This happens when e.g. a retransmitted packet arrives after recovery succeeded
        return Result::Success();
    }

    if (addOriginalResult != Siamese_Success)
    {
        const Result result = Result("SessionIncoming::decoderAddOriginal", "siamese_decoder_add_original failed", ErrorType::Siamese, addOriginalResult);
        Deps.Logger->Error("siamese_decoder_add_original failed: ", result.ToJson());
        return result;
    }

    // If the packet number is the one we were expecting:
    if (original.PacketNum == FECNextExpectedPacketNum)
    {
        // Increment the next expected packet number
        FECNextExpectedPacketNum = SIAMESE_PACKET_NUM_INC(FECNextExpectedPacketNum);

        // Resume processing
        const Result result = resumeProcessing();
        if (result.IsFail()) {
            return result;
        }
    }

    return attemptDecode();
}

Result SessionIncoming::onSiameseSolved(const SiameseOriginalPacket& packet)
{
    siamese::ReadByteStream frameReader(packet.Data, packet.DataBytes);

    // Process all messages contained in the datagram:
    while (frameReader.Remaining() >= protocol::kMessageFrameBytes)
    {
        // Read message frame header
        unsigned messageType = frameReader.Read16();
        const unsigned messageBytes = messageType & protocol::kMessageLengthMask;

        if (frameReader.Remaining() < messageBytes)
        {
            TONK_DEBUG_BREAK();
            return Result("SessionIncoming::onSiameseSolved", "Truncated message", ErrorType::Tonk, Tonk_InvalidData);
        }

        messageType >>= protocol::kMessageLengthBits;
        const uint8_t* messageData = frameReader.Read(messageBytes);

        // Unordered and unreliable data can be processed after Siamese solves
        // the recovery problem
        if (messageType == protocol::MessageType_Unordered ||
            messageType == protocol::MessageType_Unreliable)
        {
            TONK_VERBOSE_INCOMING_LOG("Deliver onSiameseSolved unordered(", packet.PacketNum, ") message: messageType=", messageType, " messageBytes=", messageBytes);

            const Result result = deliverMessage(messageType, messageData, messageBytes);
            if (result.IsFail()) {
                return result;
            }
        }
    }

    // If the message payload was not completely used:
    if (frameReader.Remaining() > 0)
    {
        TONK_DEBUG_BREAK(); // Invalid data
        return Result("SessionIncoming::onSiameseSolved", "Invalid frame", ErrorType::Tonk, Tonk_InvalidData);
    }

    return Result::Success();
}

Result SessionIncoming::deliverMessage(
    unsigned messageType,
    const uint8_t* messageData,
    unsigned messageBytes)
{
    TONK_DEBUG_ASSERT(messageType >= protocol::MessageType_Reliable);

    if (messageType == protocol::MessageType_Control) {
        return Deps.IncomingHandler->OnControlChannel(messageData, messageBytes);
    }

    if (messageType == protocol::MessageType_Unordered)
    {
        Config->OnData(
            Config->AppContextPtr,
            Connection,
            TonkChannel_Unordered,
            messageData,
            messageBytes);
        return Result::Success();
    }

    if (messageType == protocol::MessageType_Unreliable)
    {
        Config->OnData(
            Config->AppContextPtr,
            Connection,
            TonkChannel_Unreliable,
            messageData,
            messageBytes);
        return Result::Success();
    }

    unsigned channel = messageType - protocol::MessageType_Reliable;

    // If the channel is invalid:
    if (channel >= protocol::kLowPriEndChannel0 + TonkChannel_Count)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return Result("SessionIncoming::deliverMessage", "Invalid message type", ErrorType::Tonk, Tonk_InvalidData);
    }

    IncomingReliableChannel* irc;
    unsigned channelOffset;

    // If this is a Reliable message:
    if (channel < TonkChannel_Count * 2)
    {
        irc = &ReliableIncoming[0];
        channelOffset = TonkChannel_Reliable0;
    }
    else // This is a LowPri message
    {
        channel -= TonkChannel_Count * 2;
        irc = &LowPriIncoming[0];
        channelOffset = TonkChannel_LowPri0;
    }

    // If this is the start/middle of a message:
    if (channel < TonkChannel_Count)
    {
        irc += channel;
        irc->Append(messageData, messageBytes);

        if (irc->GetBytes() > TONK_MAX_RELIABLE_BYTES)
        {
            TONK_DEBUG_BREAK(); // Could be a bug?
            return Result("SessionIncoming::deliverMessage", "Reliable message too long", ErrorType::Tonk, Tonk_InvalidData);
        }

        TONK_VERBOSE_INCOMING_LOG("Append to channel ", channel, " - now ", irc->GetBytes(), " bytes");

        return Result::Success();
    }

    channel -= TonkChannel_Count;
    TONK_DEBUG_ASSERT(channel < TonkChannel_Count);
    irc += channel;

    if (!irc->IsEmpty())
    {
        irc->Append(messageData, messageBytes);
        const unsigned totalBytes = irc->GetBytes();
        if (totalBytes > TONK_MAX_RELIABLE_BYTES)
        {
            TONK_DEBUG_BREAK(); // Could be a bug?
            return Result("SessionIncoming::deliverMessage", "Reliable message too long", ErrorType::Tonk, Tonk_InvalidData);
        }

        messageData  = irc->GetData();
        messageBytes = irc->GetBytes();

        TONK_VERBOSE_INCOMING_LOG("Final append to channel ", channel, " - now ", irc->GetBytes(), " bytes");
    }

    Config->OnData(
        Config->AppContextPtr,
        Connection,
        channelOffset + channel,
        messageData,
        messageBytes);

    irc->Reset();

    return Result::Success();
}

Result SessionIncoming::PostAcknowledgements()
{
    //AckRequired = false;

    // Check if the receiver has already got this ack
    if (LastAckAckSeq == FECNextExpectedPacketNum) {
        return Result::Success();
    }

    // Generate decoder ack data
    uint8_t data[protocol::kMinPossibleDatagramByteLimit];

    // Write NextExpectedNonce to the front (truncated)
    siamese::WriteU24_LE_Min4Bytes(data, GetNextExpectedNonce24());

    // Calculate sender shape to deliver to remote peer
    const BandwidthShape shape = Deps.ReceiverControl->GetSenderShape();
    shape.Compress(data + 3);

    // Append acknowledgement data
    unsigned ackBufferBytes = 0;
    const int ackResult = siamese_decoder_ack(
        FECDecoder,
        data + kAckOverheadBytes,
        protocol::kMinPossibleDatagramByteLimit - kAckOverheadBytes,
        &ackBufferBytes);

    if (ackResult == Siamese_NeedMoreData) {
        // No datagrams received yet
        TONK_DEBUG_ASSERT(ackBufferBytes == 0);
    }
    else if (ackResult != Siamese_Success) {
        return Result("SessionIncoming::PostAcknowledgements", "siamese_decoder_ack failed", ErrorType::Siamese, ackResult);
    }

    // Let the bandwidth control subsystem know we sent an ack
    Deps.ReceiverControl->OnSendAck();

    return Deps.Outgoing->QueueUnreliable(
        protocol::MessageType_Acknowledgements,
        data,
        kAckOverheadBytes + ackBufferBytes);
}


} // namespace tonk
