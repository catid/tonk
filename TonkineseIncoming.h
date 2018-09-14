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

#pragma once

#include "TonkineseTools.h"
#include "TonkineseProtocol.h"
#include "TonkineseBandwidth.h"
#include "TonkineseInterfaces.h"
#include "PacketCompression.h"
#include "TimeSync.h"
#include "StrikeRegister.h"
#include "SimpleCipher.h"

#include "siamese.h"

#include <atomic>

namespace tonk {


//------------------------------------------------------------------------------
// IncomingReliableChannel

/// This is a thin wrapper around a memory buffer for storing data chunks as
/// they arrive, appending new chunks
class IncomingReliableChannel
{
public:
    TONK_FORCE_INLINE void Reset()
    {
        // Expecting this to not free the memory for better performance
        ReceivedDataSoFar.Clear();
    }
    TONK_FORCE_INLINE void Append(const uint8_t* data, unsigned bytes)
    {
        const unsigned existingBytes = ReceivedDataSoFar.GetSize();
        ReceivedDataSoFar.SetSize_Copy(existingBytes + bytes);
        memcpy(ReceivedDataSoFar.GetPtr(existingBytes), data, bytes);
    }
    TONK_FORCE_INLINE const uint8_t* GetData() const
    {
        return ReceivedDataSoFar.GetPtr(0);
    }
    TONK_FORCE_INLINE unsigned GetBytes() const
    {
        return ReceivedDataSoFar.GetSize();
    }
    TONK_FORCE_INLINE bool IsEmpty() const
    {
        return ReceivedDataSoFar.GetSize() == 0;
    }

private:
    pktalloc::LightVector<uint8_t> ReceivedDataSoFar;
};


//------------------------------------------------------------------------------
// SessionIncoming

/**
    The SessionIncoming helper object is responsible for processing incoming
    datagrams, interpreting the footer protocol, stepping through message frames
    and delivering them to the application.

    Some remaining number of message types are passed to IIncomingHandler for
    further processing, such as acknowledgements that must be routed to the
    SessionOutgoing helper object.

    It tracks state related to receiving data from the peer, so it exposes the
    PostAcknowledgements() function to push an acknowledgement message into the
    outgoing message queue.
*/

class SessionIncoming
{
public:
    struct Dependencies
    {
        RefCounter* ConnectionRef;
        IIncomingHandler* IncomingHandler;
        TimeSynchronizer* TimeSync;
        IIncomingToOutgoing* Outgoing;
        ReceiverBandwidthControl* ReceiverControl;
#ifdef TONK_DETAILED_STATS
        DetailStatsMemory* Deets;
#endif // TONK_DETAILED_STATS
        logger::Channel* Logger;
    };

    Result Initialize(
        const Dependencies& deps,
        uint64_t key,
        const TonkConnectionConfig* config,
        TonkConnection connection);
    void ChangeEncryptionKey(uint64_t key);
    void Shutdown();


    /**
        PostAcknowledgements()

        Queues an outgoing MessageType_Acknowledgements message containing
        acknowledgement data provided by the Siamese library.  The message
        will also contain the NextExpectedNonce to receive from the peer,
        which is used by the peer to decide how many bytes to use to represent
        nonces in datagrams that are sent after it is received.
    */
    Result PostAcknowledgements();

    /**
        ProcessDatagram()

        Processes a UDP datagram returning any issue encountered.

        receiveUsec: [In] Time at which the datagram was received
        data: [In] Pointer to the datagram data
        bytes: [In] Number of bytes of data

        authenticatedOut: [Out] Set to true if the data was authenticated
        and definitely came from the peer rather than some other host.

        Returns the result from the datagram processing.
    */
    Result ProcessDatagram(
        const uint64_t receiveUsec,
        const UDPAddress& addr,
        uint8_t* data,
        const unsigned bytes);

#if 0
    /// Is an acknowledgement required?
    bool IsAckRequired() const
    {
        return AckRequired;
    }
#endif

    /// Get low 24 bits of next expected nonce
    uint32_t GetNextExpectedNonce24() const
    {
        return static_cast<uint32_t>(AntiReplay.GetNextExpected().ToUnsigned());
    }

protected:
    Dependencies Deps;
    TonkConnection Connection = nullptr;
    const TonkConnectionConfig* Config = nullptr;

    /// Duplicated packet rejection
    security::StrikeRegister AntiReplay;

    /// Packet encryption: Decryption module
    security::SimpleCipher Decryptor;

    /// Siamese FEC decoder
    /// Note that no lock is required to protect SiameseDecoder because all
    /// usages are from the same thread.
    SiameseDecoder FECDecoder = nullptr;

    /// Next expected packet number to receive
    unsigned FECNextExpectedPacketNum = 0;
    // TBD: FECNextExpectedPacketNum is also available from the Siamese codec
    // but it is cleaner to maintain a copy of it independently than to query?

    /// Acknowledgement required for new data received
    //bool AckRequired = false;

    /// Last AckAck sequence number
    unsigned LastAckAckSeq = 0x800000;

    /// Reliable message channels
    IncomingReliableChannel ReliableIncoming[TonkChannel_Count];

    /// LowPri message channels
    IncomingReliableChannel LowPriIncoming[TonkChannel_Count];

    /// Message compression
    MessageDecompressor Decompressor;

    /// First reliable message in current datagram
    const uint8_t* FirstReliable = nullptr;

    /// Largest SendTS24 seen so far
    Counter32 LargestSendTS24;


    /// Resume processing data
    /// Precondition: FECNextExpectedPacketNum is set to the next packet that may
    /// have been received already
    Result resumeProcessing();

    /// Add original data to decoder
    Result decoderAddOriginal(const SiameseOriginalPacket& original);

    /// Handle a FEC recovery packet from peer
    Result onRecovery(const uint8_t* messageData, unsigned messageBytes);

    /// Attempts to decode with the available received data
    Result attemptDecode();

    /// Deliver message to app
    Result deliverMessage(
        unsigned messageType,
        const uint8_t* messageData,
        unsigned messageBytes);

    /// Called when Siamese has recovered some reliable messages and we want
    /// to scan those messages for ones to deliver right away
    Result onSiameseSolved(const SiameseOriginalPacket& packet);

    /// Handle compressed message type
    Result onCompressed(const uint8_t* data, unsigned bytes);

    /// Handle a reliable (compressed or uncompressed) and pass the result to the application
    Result handleReliableMessage(
        unsigned messageType,
        const uint8_t* messageData,
        unsigned messageBytes);

    /// Insert uncompressed data between `FirstReliable` through `end`
    void insertUncompressed(const uint8_t* end);
};


} // namespace tonk
