/*
    Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Tonk nor the names of its contributors may be
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

#include "tonk_file_transfer.h"
#include "TonkineseTools.h"
#include "MappedFile.h"


//------------------------------------------------------------------------------
// Constants

// Maximum length of name field in bytes
static const unsigned kMaxNameBytes = TONK_MAX_FILE_NAME_BYTES;

// Map no more than 4 Megabytes at a time to save memory space
// TBD: For faster file transfers does this become a bottleneck?
static const uint32_t kMaxViewBytes = 4 * 1000 * 1000;

// Maximum bytes to send per chunk
static const unsigned kBytesChunk = 64000;

// Minimum bytes to write for a file transfer
static const unsigned kMinWriteBytes = 16;

// Maximum bytes that could be used for the header
static const unsigned kMaxHeaderOverhead = 1 + 8 + 2;

// <Magic(4 bytes)>
static const unsigned kCancelBytes = 4;

// Magic number preceeding a cancel message
static const uint32_t kCancelMagic = 0x18181818;

// Threshold for switching from buffer to output file
static const uint64_t kOutputFileThresholdBytes = 1000 * 1000;


//------------------------------------------------------------------------------
// Helper Object

enum class Protocol
{
    HeaderAndName,  ///< Processing header/name fields
    FileData        ///< Processing file data
};

// Internal object derived from public object
// This is done so the application can read information about the file
struct TonkFileTransfer : public TonkFile_t
{
    // Data store for name
    std::string NameStr;

    // Mapped file and view for input or output with files on disk
    cat::MappedFile File;
    cat::MappedView View; // Reverse destruction order: View dies first

    // Offset into current view
    unsigned ViewOffset = 0;

    // Is this an incoming transfer?
    bool IncomingTransfer = false;

    // State machine
    Protocol State = Protocol::HeaderAndName;

    //--------------------------------------------------------------------------
    // For receiving files only:

    // Output file name string
    std::string OutputNameStr;

    //--------------------------------------------------------------------------
    // For sending files only:

    // Application data pointer
    // If this is invalid then we are reading from the mappped file
    const uint8_t* AppDataPtr = nullptr;

    // Copy of the application data for TONK_FILE_BUFFER_COPY mode
    uint8_t* InternalDataCopy = nullptr;

    // Tonk connection
    TonkConnection Connection = nullptr;

    // Reliable channel to send on
    uint32_t Channel = 0;

    // Low priority channel?
    bool LowPriChannel = false;


    bool SetChannel(uint32_t channel)
    {
        Channel = channel;
        if (channel >= TonkChannel_Reliable0 && channel < TonkChannel_Reliable0 + TonkChannel_Count)
        {
            LowPriChannel = false;
            return true;
        }
        else if (channel >= TonkChannel_LowPri0 && channel < TonkChannel_LowPri0 + TonkChannel_Count)
        {
            LowPriChannel = true;
            return true;
        }
        TONK_DEBUG_BREAK();
        return false;
    }
};

static std::atomic<uint64_t> m_FileNameIndex = ATOMIC_VAR_INIT(0);

static uint64_t InitializeFileNameRandom()
{
    uint8_t key[8];
    tonk::SecureRandom_Next(key, 8);
    return m_FileNameIndex += siamese::ReadU64_LE(key);
}

// Make a file name string from a given file name
static std::string MakeFileNameString(const std::string& name)
{
    uint64_t index = m_FileNameIndex++;
    if (index == 0) {
        index = InitializeFileNameRandom();
    }

    std::string s;
    for (char ch : name) {
        if (std::isalnum(ch) || ch == '.' || ch == '_') {
            s.push_back(ch);
        }
    }

    s += "_";
    s += tonk::HexString(index);
    s += ".download";

    return s;
}


extern "C" {


//------------------------------------------------------------------------------
// Tonk File Transfer API

TONK_EXPORT TonkFile tonk_file_from_disk(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const char*      filepath   ///< Path for file to send
)
{
    // Input validation
    if (!connection || !name || !filepath || !filepath[0]) {
        TONK_DEBUG_BREAK();
        return nullptr;
    }

    TonkFileTransfer* transfer = new (std::nothrow) TonkFileTransfer;

    // Input validation
    if (!transfer->SetChannel(channel)) {
        delete transfer;
        return nullptr;
    }

    transfer->Connection = connection;
    transfer->NameStr = name;
    transfer->Name = transfer->NameStr.c_str();

    // Attempt to open the file for reading
    bool openSuccess = transfer->File.OpenRead(filepath);

    // If open failed:
    if (!openSuccess) {
        //TONK_DEBUG_BREAK();
        delete transfer;
        return nullptr;
    }

    // Attempt to create a view object
    bool viewSuccess = transfer->View.Open(&transfer->File);

    // If view failed:
    if (!viewSuccess) {
        TONK_DEBUG_BREAK();
        delete transfer;
        return nullptr;
    }

    // Limit the number of mapped bytes
    uint64_t mapBytes = transfer->File.Length;
    if (mapBytes > kMaxViewBytes) {
        mapBytes = kMaxViewBytes;
    }

    // Attempt to map the first chunk of the file
    uint8_t* mapPtr = transfer->View.MapView(0, static_cast<uint32_t>(mapBytes));

    // If map failed:
    if (!mapPtr) {
        TONK_DEBUG_BREAK();
        delete transfer;
        return nullptr;
    }
    TONK_DEBUG_ASSERT(transfer->View.Length == mapBytes);

    transfer->TotalBytes = transfer->File.Length;

    return reinterpret_cast<TonkFile>(transfer);
}

TONK_EXPORT TonkFile tonk_file_from_buffer(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const void*          data,  ///< File buffer data pointer
    uint32_t            bytes,  ///< File buffer bytes to send
    uint32_t            flags   ///< Flags for buffer
)
{
    // Input validation
    if (!connection || !name || !data) {
        TONK_DEBUG_BREAK();
        return nullptr;
    }

    TonkFileTransfer* transfer = new (std::nothrow) TonkFileTransfer;

    if (!transfer) {
        return nullptr; // OOM
    }

    // Input validation
    if (!transfer->SetChannel(channel)) {
        delete transfer;
        return nullptr;
    }

    transfer->Connection = connection;
    transfer->NameStr = name;
    transfer->Name = transfer->NameStr.c_str();

    if (flags & TONK_FILE_BUFFER_ZERO_COPY) {
        transfer->AppDataPtr = reinterpret_cast<const uint8_t*>(data);
    }
    else {
        transfer->InternalDataCopy = new (std::nothrow) uint8_t[bytes];

        if (!transfer->InternalDataCopy) {
            delete transfer;
            return nullptr; // OOM
        }

        // Make a copy of the app data
        memcpy(transfer->InternalDataCopy, data, bytes);
        transfer->AppDataPtr = transfer->InternalDataCopy;
    }
    transfer->TotalBytes = bytes;

    return reinterpret_cast<TonkFile>(transfer);
}

// Write a cancel message to the given connection reliable channel
static void WriteCancel(TonkConnection connection, unsigned channel)
{
    // Construct cancel message
    uint8_t packet[kCancelBytes];
    siamese::WriteU32_LE(packet, kCancelMagic);

    // Send cancel message ignoring return value
    tonk_send(connection, packet, kCancelBytes, channel);
}

// Packet buffer
uint8_t m_Packet[kBytesChunk];
tonk::Lock m_PacketLock;

static void WriteSendHeader(
    TonkFileTransfer* transfer,
    unsigned& deficitBytes,
    const uint8_t* viewData,
    const uint32_t viewBytes)
{
    uint8_t flags = 0xc0;

    // Calculate number of bytes needed for file size field
    unsigned lenBits = 1;
    uint64_t totalBytes = transfer->TotalBytes;
    if (totalBytes > 0) {
        if (totalBytes > 0xFFFFFFFF) {
            lenBits += 32;
            totalBytes >>= 32;
        }
        unsigned lowestBit = tonk::NonzeroLowestBitIndex(static_cast<uint32_t>(totalBytes));
        lenBits += lowestBit;
    }
    const unsigned lenBytes = (lenBits + 7) / 8; // Round up
    TONK_DEBUG_ASSERT(lenBytes >= 1);
    flags |= static_cast<uint8_t>((lenBytes - 1) << 1);

    // Calculate number of bytes needed for name length field
    unsigned nameLen = static_cast<unsigned>(transfer->NameStr.length());
    static_assert(kMaxNameBytes <= 0xffff, "Update this to support more than 2 bytes");
    if (nameLen > kMaxNameBytes) {
        nameLen = kMaxNameBytes; // Name length limit
    }
    unsigned nameLenBytes = 0;
    if (nameLen > 0) {
        nameLenBytes = (nameLen > 0xffff) ? 2 : 1;
    }
    flags |= static_cast<uint8_t>(nameLenBytes << 4);

    //--------------------------------------------------------------------------
    // Hold m_PacketLock here to guard m_Packet
    tonk::Locker locker(m_PacketLock);
    uint8_t* packet = m_Packet;

    // Write header
    packet[0] = flags;
    totalBytes = transfer->TotalBytes;
    packet[1] = static_cast<uint8_t>(totalBytes);
    for (unsigned i = 1; i < lenBytes; ++i) {
        packet[i + 1] = static_cast<uint8_t>(totalBytes >> (i * 8));
    }
    uint8_t* writeLen = packet + 1 + lenBytes;
    for (unsigned i = 0; i < nameLenBytes; ++i) {
        writeLen[i] = static_cast<uint8_t>(nameLen >> (i * 8));
    }

    static_assert(kMaxNameBytes < kBytesChunk - kMaxHeaderOverhead, "I chose the name limit to fit within the first packet to avoid needing to split it up");

    // Attempt to write entire name
    memcpy(writeLen + nameLenBytes, transfer->NameStr.c_str(), nameLen);

    // Determine how many data bytes to tack onto the first message
    unsigned dataBytes = 0;
    unsigned sendBytes = deficitBytes;
    if (sendBytes > kBytesChunk) {
        sendBytes = kBytesChunk;
    }
    const unsigned minSendBytes = 1 + lenBytes + nameLenBytes + nameLen;
    if (sendBytes <= minSendBytes)
    {
        // We will be sending the name even if it is a bit too much
        sendBytes = minSendBytes;
    }
    else
    {
        dataBytes = sendBytes - minSendBytes;
        TONK_DEBUG_ASSERT(dataBytes > 0);
        if (dataBytes > viewBytes) {
            dataBytes = viewBytes;
        }
        if (dataBytes >= transfer->TotalBytes)
        {
            dataBytes = static_cast<unsigned>(transfer->TotalBytes);

            // Header and data fit entirely within one message
            transfer->Flags |= TonkFileFlags_Done;
        }

        memcpy(
            writeLen + nameLenBytes + nameLen,
            viewData,
            dataBytes);
    }

    sendBytes = minSendBytes + dataBytes;

    // Send the first packet
    const TonkResult result = tonk_send(
        transfer->Connection,
        packet,
        sendBytes,
        transfer->Channel);

    if (TONK_FAILED(result))
    {
        transfer->Flags |= TonkFileFlags_Done;
        // Cannot abort since we cannot send messages
        TONK_DEBUG_BREAK();
        return;
    }

    // Now sending file data
    transfer->State = Protocol::FileData;
    transfer->ProgressBytes = dataBytes;
    transfer->ViewOffset = dataBytes;

    // If there is no more space:
    if (deficitBytes <= sendBytes + kMinWriteBytes) {
        deficitBytes = 0;
    }
    else {
        deficitBytes -= sendBytes;
    }
}

TONK_EXPORT void tonk_file_send(
    TonkFile file ///< File being sent
)
{
    TonkFileTransfer* transfer = reinterpret_cast<TonkFileTransfer*>(file);

    // Input validation
    if (!transfer ||
        (0 != (transfer->Flags & TonkFileFlags_Done)))
    {
        return;
    }

    // Skip sending if we are not connected yet,
    // or if the connection has closed
    TonkStatus status;
    tonk_status(transfer->Connection, &status);
    if (0 == (status.Flags & TonkFlag_Connected)) {
        return;
    }

    // Get current queue depth on the given channel
    uint32_t queueUsec = transfer->LowPriChannel ? status.LowPriQueueMsec : status.ReliableQueueMsec;
    queueUsec *= 1000;
    const uint32_t targetUsec = status.TimerIntervalUsec;

    // If queue is already full:
    if (queueUsec >= targetUsec) {
        return;
    }

    // Calculate how many bytes to write
    const unsigned deficitUsec = targetUsec - queueUsec;
    unsigned deficitBytes = ((uint64_t)status.AppBPS * deficitUsec) / 1000000;

    // If no data can be written right now:
    if (deficitBytes < kMinWriteBytes) {
        return;
    }

    const uint8_t* viewData = transfer->AppDataPtr;
    uint32_t viewBytes;
    if (viewData)
    {
        TONK_DEBUG_ASSERT(transfer->TotalBytes <= 0xffffffff);
        viewBytes = static_cast<uint32_t>(transfer->TotalBytes);
    }
    else
    {
        viewData = transfer->View.Data;
        viewBytes = transfer->View.Length;
    }

    // If we are writing the header still:
    if (transfer->State == Protocol::HeaderAndName) {
        // Note this function also sends some data and may send the whole file
        // if the file is within 64KB.
        WriteSendHeader(transfer, deficitBytes, viewData, viewBytes);
    }

    // Send file data chunks:
    while (deficitBytes > 0)
    {
        const unsigned viewOffset = transfer->ViewOffset;
        TONK_DEBUG_ASSERT(viewBytes >= viewOffset);
        unsigned viewRemainingBytes = viewBytes - viewOffset;

        // If there are remaining bytes in the current view:
        if (viewRemainingBytes > 0)
        {
            unsigned sendBytes = deficitBytes;
            if (sendBytes > kBytesChunk) {
                sendBytes = kBytesChunk;
            }
            if (sendBytes > viewRemainingBytes) {
                sendBytes = viewRemainingBytes;
            }
            TONK_DEBUG_ASSERT(sendBytes > 0);

            const TonkResult result = tonk_send(
                transfer->Connection,
                viewData + viewOffset,
                sendBytes,
                transfer->Channel);

            if (TONK_FAILED(result))
            {
                // Flag as dead
                transfer->ProgressBytes = 0;
                transfer->Flags |= TonkFileFlags_Done;
                // Cannot abort since we cannot send messages
                TONK_DEBUG_BREAK();
                return;
            }

            TONK_DEBUG_ASSERT(deficitBytes >= sendBytes);
            deficitBytes -= sendBytes;

            transfer->ProgressBytes += sendBytes;
            transfer->ViewOffset = viewOffset + sendBytes;
            TONK_DEBUG_ASSERT(transfer->ProgressBytes <= transfer->TotalBytes);

            viewRemainingBytes -= sendBytes;
        }

        // If the current view is complete:
        if (viewRemainingBytes <= 0)
        {
            // If transfer is done now:
            if (transfer->ProgressBytes >= transfer->TotalBytes) {
                transfer->Flags |= TonkFileFlags_Done;
                return;
            }
            TONK_DEBUG_ASSERT(!transfer->AppDataPtr);
            TONK_DEBUG_ASSERT(transfer->File.Length == transfer->TotalBytes);

            // Limit the number of mapped bytes
            uint64_t mapBytes = transfer->TotalBytes - transfer->ProgressBytes;
            if (mapBytes > kMaxViewBytes) {
                mapBytes = kMaxViewBytes;
            }

            // Attempt to map the first chunk of the file
           viewData = transfer->View.MapView(
                transfer->ProgressBytes,
                static_cast<uint32_t>(mapBytes));

            // If map failed:
            if (!viewData)
            {
                // Flag as dead
                transfer->ProgressBytes = 0;
                transfer->Flags |= TonkFileFlags_Done;

                // Cancel since we can still send messages
                WriteCancel(transfer->Connection, transfer->Channel);

                TONK_DEBUG_BREAK();
                return;
            }
            TONK_DEBUG_ASSERT(transfer->View.Length >= mapBytes);
            TONK_DEBUG_ASSERT(transfer->View.Data == viewData);
            TONK_DEBUG_ASSERT(transfer->ProgressBytes >= transfer->View.Offset);

            // Reset view offset
            transfer->ViewOffset = static_cast<unsigned>(transfer->ProgressBytes - transfer->View.Offset);
            viewBytes = transfer->View.Length;
        }

        TONK_DEBUG_ASSERT(0 == (transfer->Flags & TonkFileFlags_Done));
    } // While there is still room
}

static uint32_t ProcessReceiveFileData(
    TonkFile*   filePtr,  ///< TonkFile to update (or null)
    const uint8_t* data,  ///< Data received
    uint32_t      bytes   ///< Bytes of data
)
{
    TonkFile file = *filePtr;
    TonkFileTransfer* transfer = reinterpret_cast<TonkFileTransfer*>(file);
    unsigned nonDataBytes = 0;

    if (transfer)
    {
        const bool cancel =
            (bytes >= kCancelBytes) &&
            (kCancelMagic == siamese::ReadU32_LE(data));

        // If file transfer is not in progress:
        if (!transfer->IncomingTransfer ||
            (0 != (transfer->Flags & TonkFileFlags_Failed)) ||
            cancel)
        {
            tonk_file_free(file);
            file = nullptr;
            transfer = nullptr;
            *filePtr = nullptr;
            if (cancel) {
                return kCancelBytes;
            }
        }
    }

    // If this is the start of a file:
    if (!transfer)
    {
        // If header is truncated:
        if (bytes < 3)
        {
            TONK_DEBUG_BREAK(); // Invalid input
            return bytes;
        }

        const uint8_t flags = data[0];
        if (0xc0 != (flags & 0xc1)) {
            TONK_DEBUG_BREAK(); // Invalid input
            return bytes;
        }
        const unsigned lenBytes = ((flags >> 1) & 7) + 1;
        const unsigned nameLenBytes = (flags >> 4) & 3;
        const unsigned headerBytes = 1 + lenBytes + nameLenBytes;
        if (bytes < headerBytes) {
            TONK_DEBUG_BREAK(); // Invalid input
            return bytes;
        }
        uint64_t totalBytes = static_cast<uint64_t>(data[1]);
        for (unsigned i = 1; i < lenBytes; ++i) {
            totalBytes |= static_cast<uint64_t>(data[i + 1]) << (i * 8);
        }
        const uint8_t* readPtr = data + 1 + lenBytes;
        unsigned nameLen = 0;
        if (nameLenBytes >= 1) {
            nameLen = readPtr[0];
            if (nameLenBytes >= 2) {
                nameLen |= static_cast<unsigned>(readPtr[1]) << 8;
            }
        }
        if (bytes < headerBytes + nameLen) {
            TONK_DEBUG_BREAK(); // Invalid input
            return bytes;
        }
        nonDataBytes = 1 + lenBytes + nameLenBytes + nameLen;
        TONK_DEBUG_ASSERT(nonDataBytes <= bytes);

        transfer = new (std::nothrow) TonkFileTransfer;
        if (!transfer) {
            TONK_DEBUG_BREAK(); // OOM
            return bytes;
        }
        *filePtr = transfer;

        // Read name string
        transfer->NameStr.assign(
            reinterpret_cast<const char*>(readPtr + nameLenBytes),
            nameLen
        );
        transfer->Name = transfer->NameStr.c_str();

        transfer->IncomingTransfer = true;
        transfer->TotalBytes = totalBytes;
        transfer->State = Protocol::FileData;

        // If this must be downloaded to a file:
        if (totalBytes >= kOutputFileThresholdBytes)
        {
            // Pick an output file name
            transfer->OutputNameStr = MakeFileNameString(transfer->Name);
            transfer->OutputFileName = transfer->OutputNameStr.c_str();

            uint64_t mapBytes = totalBytes;
            if (mapBytes > kMaxViewBytes) {
                mapBytes = kMaxViewBytes;
            }

            // Attempt to open an output temp file
            bool writeSuccess =
                transfer->File.OpenWrite(transfer->OutputFileName, mapBytes) &&
                transfer->View.Open(&transfer->File) &&
                nullptr != transfer->View.MapView(0, static_cast<uint32_t>(mapBytes));
            if (!writeSuccess)
            {
                TONK_DEBUG_BREAK(); // Unable to open output file
                transfer->Flags |= TonkFileFlags_Failed;
            }
        }
        else
        {
            transfer->OutputData = new (std::nothrow) uint8_t[static_cast<unsigned>(totalBytes)];
            if (!transfer->OutputData)
            {
                TONK_DEBUG_BREAK(); // Unable to open output file
                transfer->Flags |= TonkFileFlags_Failed;
            }
        }

        // Eat header
        data += nonDataBytes;
        bytes -= nonDataBytes;
    } // end if no transfer

    // Get output buffer view
    uint8_t* viewData = transfer->OutputData;
    uint32_t viewBytes;
    if (viewData)
    {
        TONK_DEBUG_ASSERT(transfer->TotalBytes <= 0xffffffff);
        viewBytes = static_cast<uint32_t>(transfer->TotalBytes);
    }
    else
    {
        viewData = transfer->View.Data;
        viewBytes = transfer->View.Length;
    }

    TONK_DEBUG_ASSERT(viewBytes >= transfer->ViewOffset);
    const unsigned remainingBytes = viewBytes - transfer->ViewOffset;

    unsigned copyBytes = bytes;
    if (copyBytes > remainingBytes) {
        copyBytes = remainingBytes;
    }

    // If not ignoring data:
    if (0 == (transfer->Flags & TonkFileFlags_Failed))
    {
        memcpy(
            viewData + transfer->ViewOffset,
            data,
            copyBytes);
    }

    transfer->ProgressBytes += bytes;
    transfer->ViewOffset += bytes;

    // If file receive is complete:
    if (transfer->ProgressBytes >= transfer->TotalBytes)
    {
        TONK_DEBUG_ASSERT(transfer->ProgressBytes == transfer->TotalBytes);
        transfer->Flags |= TonkFileFlags_Done;
        // Note that IsFailedBool may have been set at some point

        // Close off files (if any were open) so they can be moved
        transfer->View.Close();
        transfer->File.Close();
    }
    // If view is complete:
    else if (transfer->ViewOffset >= viewBytes)
    {
        // Note some data still remains to be received so map another view
        TONK_DEBUG_ASSERT(!transfer->OutputData && transfer->File.IsValid());
        TONK_DEBUG_ASSERT(transfer->ViewOffset >= viewBytes);
        TONK_DEBUG_ASSERT(transfer->TotalBytes > transfer->ProgressBytes);

        uint64_t mapBytes = transfer->TotalBytes - transfer->ProgressBytes;
        if (mapBytes > kMaxViewBytes) {
            mapBytes = kMaxViewBytes;
        }

        // Expand the file on disk
        transfer->View.Close();
        bool resizeSuccess = transfer->File.Resize(transfer->ProgressBytes + mapBytes);
        if (!resizeSuccess)
        {
            TONK_DEBUG_BREAK(); // Unable to open output file
            transfer->Flags |= TonkFileFlags_Failed;
        }

        uint8_t* mapPtr = transfer->View.MapView(
            transfer->ProgressBytes,
            static_cast<uint32_t>(mapBytes));
        if (!mapPtr)
        {
            TONK_DEBUG_BREAK(); // Unable to open output file
            transfer->Flags |= TonkFileFlags_Failed;
        }

        TONK_DEBUG_ASSERT(transfer->ProgressBytes >= transfer->View.Offset);
        transfer->ViewOffset = static_cast<unsigned>(transfer->ProgressBytes - transfer->View.Offset);
    }

    // Return consumed byte count
    return nonDataBytes + copyBytes;
}

TONK_EXPORT TonkFile tonk_file_receive(
    TonkFile     file,  ///< TonkFile to update (or null)
    const void* vdata,  ///< Data received
    uint32_t    bytes,  ///< Bytes of data
    FpTonkFileDone fp,  ///< Callback function
    void*     context   ///< Context pointer
)
{
    const uint8_t* data = reinterpret_cast<const uint8_t*>(vdata);

    // Input validation
    if (!data || bytes <= 0)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return file;
    }

    for (;;)
    {
        uint32_t written = ProcessReceiveFileData(&file, data, bytes);

        // If file transfer is done:
        if (file && (0 != (file->Flags & TonkFileFlags_Done)))
        {
            // Close the file
            TonkFileTransfer* transfer = reinterpret_cast<TonkFileTransfer*>(file);
            if (transfer->OutputFileName)
            {
                transfer->View.Close();
                transfer->File.Close();
            }

            const int shouldDelete = fp(file, context);

            // If temporary file should be kept:
            if (shouldDelete != TONK_FILE_DELETE)
            {
                // Clear the output file name so it does not get deleted
                transfer->OutputFileName = nullptr;
            }

            tonk_file_free(file);
            file = nullptr;
        }

        if (written >= bytes) {
            break;
        }

        data += written;
        bytes -= written;
    }

    return file;
}

TONK_EXPORT void tonk_file_free(TonkFile file)
{
    TonkFileTransfer* transfer = reinterpret_cast<TonkFileTransfer*>( file );
    if (!transfer) {
        return;
    }

    // If this is on the receiving side:
    if (transfer->IncomingTransfer)
    {
        // If there is still an output file:
        if (transfer->OutputFileName)
        {
            transfer->File.Close();
            transfer->View.Close();

            // Remove the file from disk
            std::remove(transfer->OutputFileName);

            transfer->OutputFileName = nullptr;
        }
    }
    else // Sending side:
    {
        // If this transfer needs to be canceled:
        if (0 == (transfer->Flags & TonkFileFlags_Done)) {
            WriteCancel(transfer->Connection, transfer->Channel);
        }
    }

    // Free local copy of data
    delete[] transfer->InternalDataCopy;

    // Free output data buffer
    delete[] transfer->OutputData;

    // Delete transfer object
    delete transfer;
}


} // extern "C"
