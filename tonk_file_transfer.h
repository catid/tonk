/** \file
    \brief Tonk File Transfer C API Header
    \copyright Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

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

#ifndef CAT_TONKINESE_FILE_TRANSFER_H
#define CAT_TONKINESE_FILE_TRANSFER_H

#include "tonk.h"
#include <cstdio>

#ifdef __cplusplus
extern "C" {
#endif


//------------------------------------------------------------------------------
// File Transfer

/**
    Tonk File Transfer API

    This provides a helper utility built on top of the Tonk API to stream memory
    buffers and files over Tonk sockets.

    tonk_file_from_disk():

    For applications sending files, the tonk_file_from_disk() API can be used to
    handle the details of cross-platform memory-mapped files that can exceed 4GB
    in size.

    tonk_file_from_buffer():

    For applications sending JSON strings and other data that has unconstrained
    size that may be larger than 64000 bytes, the tonk_file_from_buffer() API
    can be used to send this data.
*/

/// Maximum number of bytes for the file name
#define TONK_MAX_FILE_NAME_BYTES 63000

typedef enum TonkFileFlagsT
{
    /// Is the transfer completed? (Success or failure)
    TonkFileFlags_Done = 1,

    /// Is the transfer a failure? (May still be in progress)
    /// The application should not react to this flag until IsDoneBool=1,
    /// because the sender may still be sending data.  If the sender aborts
    /// the transfer, this will also be set.
    TonkFileFlags_Failed = 2,
} TonkFileFlags;

/// TonkFile: Represents a file being sent over Tonk protocol
typedef struct TonkFile_t
{
    /// Bytes sent/received so far
    uint64_t ProgressBytes TONK_CPP(= 0);

    /// Total bytes in the file
    uint64_t TotalBytes TONK_CPP(= 0);

    /// Transfer state
    uint32_t Flags TONK_CPP(= 0);  ///< Combination of TonkFileFlags

    //--------------------------------------------------------------------------
    // For receiving files only:

    /// Name string provided by the sender.
    /// This cannot exceed TONK_MAX_FILE_NAME_BYTES in length.
    /// This is an empty string until we get the name.
    /// The name will switch when the next file starts being received.
    /// The name pointer will be valid until the next API call.
    const char* Name TONK_CPP(= 0);

    /// Pointer to the front of the data.
    /// If the transfer is complete and the file was very large, then the
    /// data pointer will be null and instead a file will be stored.
    /// The data pointer will be valid until the next API call.
    uint8_t* OutputData TONK_CPP(= 0);

    /// For very large files, instead of returning a data pointer Tonk
    /// will stream the data to a file on disk.  The application must
    /// check if Tonk decided to return the file via memory or disk.
    const char* OutputFileName TONK_CPP(= 0);
}* TonkFile;

/**
    tonk_file_from_disk()

    Create a TonkFile with the given name from the given file.

    Supports files with 0 bytes.

    Returns nullptr if the file could not be opened
    Return a valid pointer if file is ready to send
*/
TONK_EXPORT TonkFile tonk_file_from_disk(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const char*      filepath   ///< Path for file to send
);

/**
    tonk_file_from_buffer()

    If flags includes TONK_FILE_BUFFER_ZERO_COPY, then the data pointer must
    remain valid until the final call to tonk_file_send().

    If flags includes TONK_FILE_BUFFER_COPY instead, then the library
    will make an internal copy of the data and the application does not
    need to keep the data pinned until sending completes.

    Supports buffers with 0 bytes.

    Returns nullptr if sending could not be started
    Return a valid pointer if file is ready to send
*/

#define TONK_FILE_BUFFER_COPY 0
#define TONK_FILE_BUFFER_ZERO_COPY 1

TONK_EXPORT TonkFile tonk_file_from_buffer(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const void*          data,  ///< File buffer data pointer
    uint32_t            bytes,  ///< File buffer bytes to send
    uint32_t            flags   ///< Flags for buffer
);

/**
    tonk_file_send()

    In the OnTick() callback from Tonk, the application should call this
    function to send more data for the file.

    This function will take care of sending the right amount of data based
    on the available bandwidth.

    The application can get file transfer status directly from the updated
    TonkFile object.  See the TonkFile structure documentation above.
*/

/**
    Example OnTick():

    tonk_file_send(file);

    if (file->Flags & TonkFileFlags_Done)
    {
        if (file->Flags & TonkFileFlags_Failed)
        {
            // Handle transfer failure
        }
        else
        {
            // Handle transfer complete
        }

        tonk_file_free(file);
        file = 0;
    }
    else
    {
        float percent = file->ProgressBytes * 100 / (float)file->TotalBytes;

        // Progress update here
    }
*/

TONK_EXPORT void tonk_file_send(
    TonkFile file ///< File being sent
);

/**
    tonk_file_receive()

    This function should be called when file data is received.
    It creates a TonkFile instance that must be released with tonk_file_free().

    The application can get file transfer status directly from the updated
    TonkFile object.  See the TonkFile structure documentation above.

    The FpTonkFileDone callback is called for state updates.
    Check TonkFile::IsDoneBool to know when the file is done sending.
    And then check TonkFile::IsFailedBool to know if the file failed to send.

    Returns the new value of the TonkFile pointer.
    This should overwrite the old pointer.
*/

#define TONK_FILE_DELETE 1 /**< Delete temporary file after FpTonkFileDone */
#define TONK_FILE_KEEP   0 /**< Keep the temporary file */

/**
    tonk_file_receive() Example Usage:

    void MyClientConnection::OnData(
        uint32_t          channel,  // Channel number attached to each message by sender
        const uint8_t*       data,  // Pointer to a buffer containing the message data
        uint32_t            bytes   // Number of bytes in the message
    )
    {
        if (channel == TonkChannel_LowPri0)
        {
            File = tonk_file_receive(
                File,
                data,
                bytes,
                [](TonkFile file, void* context) -> int
            {
                MyClientConnection* thiz = reinterpret_cast<MyClientConnection*>(context);

                thiz->OnFileDone(file);

                return TONK_FILE_DELETE; // Delete file here
            }, this);

            if (File)
            {
                float percent = File->ProgressBytes * 100 / (float)File->TotalBytes;

                // Progress update here
            }
        }
    }

    void MyClientConnection::OnFileDone(TonkFile file)
    {
        if (file->Flags & TonkFileFlags_Failed)
        {
            // Handle transfer failure
            return;
        }

        float percent = file->ProgressBytes * 100 / (float)file->TotalBytes;

        if (file->OutputData)
        {
            // Data is stored in file->OutputData
            // Data size is file->TotalBytes
        }
        else
        {
            // Data is stored in temporary file: File->OutputFileName
            // Data size is file->TotalBytes
        }
    }
*/

typedef int (*FpTonkFileDone)(TonkFile file, void* context);

TONK_EXPORT TonkFile tonk_file_receive(
    TonkFile     file,  ///< TonkFile to update (or null)
    const void*  data,  ///< Data received
    uint32_t    bytes,  ///< Bytes of data
    FpTonkFileDone fp,  ///< Callback function
    void*     context   ///< Context pointer
);

/**
    tonk_file_free()

    Release memory for the TonkFile object.
    It is safe to pass null pointers to this function.

    This function will abort an outgoing send transfer if one is in progress,
    so tonk_file_free() must be called before the application releases its
    TonkConnection pointer with tonk_free().

    To avoid a large memory leak in the application, be sure to pass any
    created TonkFile objects from sending files and TonkFile objects from
    tonk_file_receive() to this function.
*/
TONK_EXPORT void tonk_file_free(TonkFile file);


#ifdef __cplusplus
}
#endif // __cplusplus

#endif // CAT_TONKINESE_FILE_TRANSFER_H

/**
    File Transfer Wire Protocol

    <Flags Byte(1 byte)>
        From lowest bit to highest bit:
        <Marker Bit = 0>
        <File Size Bytes - 1(3 bits)>
            0: 1 byte in file size field
            1: 2 bytes " " "
            ...
            7: 8 bytes " " "
        <File Name Bytes(2 bits)>
            0: No file name
            1: 1 byte in name length field
            2: 2 bytes " " "
            3: Unused
        <Marker Bit = 1>
        <Marker Bit = 1>

    <Total File Size(1..8 bytes)>
        Number of bytes in the file (field sized based on header)

    Optional: [File Name Length(1..2 bytes)]
        Optional field for length of file name in bytes
        This does not include a \0 terminator
    Optional: [File Name]
        Optional field for file name (the Name field in the TonkFile struct)
        This does not include a \0 terminator
        This field will be absent if the header specifies no file name

    <File Data>
        The size of the file data will match the Total File Size field
        When all file data is received the transfer is complete
*/
