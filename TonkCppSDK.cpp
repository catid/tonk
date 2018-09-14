/** \file
    \brief Tonk C++ SDK
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

#include "TonkCppSDK.h"

#include <algorithm>

// Enable this to debug why the library will not load.  This is useful if there
// is a missing symbol on the Linux build for example
//#define TONK_CPP_SDK_DLL_VERBOSE

#ifdef TONK_CPP_SDK_DLL_VERBOSE
#include <iostream>
using namespace std;
#endif // TONK_CPP_SDK_DLL_VERBOSE


//------------------------------------------------------------------------------
// Shim Implementation

#if !defined(TONK_DISABLE_SHIM)

#include <string.h>

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif // _WIN32

extern "C" {

//------------------------------------------------------------------------------
// Main API

typedef const char* (*fp_tonk_result_to_string_t)(TonkResult result);

typedef TonkResult (*fp_tonk_socket_create_t)(
    const TonkSocketConfig* config, // Configuration for the socket
    TonkSocket*          socketOut, // Created socket, or 0 on failure
    TonkJson*         errorJsonOut  // Receives detailed error message on error
);

typedef TonkResult(*fp_tonk_inject_t)(
    TonkSocket   tonkSocket, ///< [in] Socket to inject
    uint16_t           port, ///< [in] Local port
    const void*        data, ///< [in] Datagram data
    unsigned          bytes  ///< [in] Datagram bytes
);

typedef void (*fp_tonk_socket_destroy_t)(
    TonkSocket          tonkSocket  // Socket to shutdown
);

typedef TonkResult (*fp_tonk_advertise_t)(
    TonkSocket         tonkSocket, // Socket to send advertisement from
    const char*          ipString, // Destination IP address
    uint16_t                 port, // Destination port
    const void*              data, // Message data
    unsigned                 bytes  // Message bytes
);

typedef TonkResult (*fp_tonk_connect_t)(
    TonkSocket          tonkSocket, // Socket to connect from
    TonkConnectionConfig    config, // Configuration for the connection
    const char*           hostname, // Hostname of the remote host
    uint16_t                  port, // Port for the remote host
    TonkConnection*  connectionOut, // Set to connection on success, else 0
    TonkJson*         errorJsonOut  // Receives detailed error message on error
);

typedef void (*fp_tonk_status_t)(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatus*          statusOut  ///< [out] Status to return
);

typedef void (*fp_tonk_status_ex_t)(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatusEx*      statusExOut  ///< [out] Extended status to return
);

typedef TonkResult (*fp_tonk_send_t)(
    TonkConnection      connection, ///< Connection to send on
    const void*               data, ///< Pointer to message data
    uint64_t                 bytes, ///< Message bytes
    uint32_t               channel  ///< Channel to attach to message
);

typedef void (*fp_tonk_flush_t)(
    TonkConnection      connection  ///< Connection to flush
);

typedef void (*fp_tonk_close_t)(
    TonkConnection      connection  ///< Connection to close
);

typedef void (*fp_tonk_free_t)(
    TonkConnection      connection  ///< Connection to free
);

typedef uint64_t (*fp_tonk_time_t)();

typedef uint16_t (*fp_tonk_to_remote_time_16_t)(
    TonkConnection      connection, ///< Connection with remote host
    uint64_t             localUsec  ///< Local timestamp in microseconds
);

typedef uint64_t (*fp_tonk_from_local_time_16_t)(
    TonkConnection      connection, ///< Connection with remote host
    uint16_t    networkTimestamp16  ///< Network timestamp to interpret
);

typedef uint32_t (*fp_tonk_to_remote_time_23_t)(
    TonkConnection      connection, ///< Connection with remote host
    uint64_t             localUsec  ///< Local timestamp in microseconds
);

typedef uint64_t (*fp_tonk_from_local_time_23_t)(
    TonkConnection      connection, ///< Connection with remote host
    uint32_t    networkTimestamp23  ///< Network timestamp to interpret
);

typedef TonkResult (*fp_tonk_p2p_connect_t)(
    TonkConnection alice,
    TonkConnection   bob
);

typedef TonkResult (*fp_tonk_set_default_socket_config_t)(
    uint32_t tonk_version,
    TonkSocketConfig* configOut
);


//------------------------------------------------------------------------------
// File Transfer API

typedef TonkFile (*fp_tonk_file_from_disk_t)(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const char*      filepath   ///< Path for file to send
);

typedef TonkFile (*fp_tonk_file_from_buffer_t)(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const void*          data,  ///< File buffer data pointer
    uint32_t            bytes,  ///< File buffer bytes to send
    uint32_t            flags   ///< Flags for buffer
);

typedef void (*fp_tonk_file_send_t)(
    TonkFile file ///< File being sent
);

typedef TonkFile (*fp_tonk_file_receive_t)(
    TonkFile     file,  ///< TonkFile to update (or null)
    const void*  data,  ///< Data received
    uint32_t    bytes,  ///< Bytes of data
    FpTonkFileDone fp,  ///< Callback function
    void*     context   ///< Context pointer
);

typedef void (*fp_tonk_file_free_t)(TonkFile file);


//------------------------------------------------------------------------------
// Function Pointers

static std::mutex FunctionPointerLock;

// Main Tonk API
static fp_tonk_result_to_string_t fp_tonk_result_to_string = nullptr;
static fp_tonk_socket_create_t fp_tonk_socket_create = nullptr;
static fp_tonk_inject_t fp_tonk_inject = nullptr;
static fp_tonk_socket_destroy_t fp_tonk_socket_destroy = nullptr;
static fp_tonk_advertise_t fp_tonk_advertise = nullptr;
static fp_tonk_connect_t fp_tonk_connect = nullptr;
static fp_tonk_status_t fp_tonk_status = nullptr;
static fp_tonk_status_ex_t fp_tonk_status_ex = nullptr;
static fp_tonk_send_t fp_tonk_send = nullptr;
static fp_tonk_flush_t fp_tonk_flush = nullptr;
static fp_tonk_close_t fp_tonk_close = nullptr;
static fp_tonk_free_t fp_tonk_free = nullptr;
static fp_tonk_time_t fp_tonk_time = nullptr;
static fp_tonk_to_remote_time_16_t fp_tonk_to_remote_time_16 = nullptr;
static fp_tonk_from_local_time_16_t fp_tonk_from_local_time_16 = nullptr;
static fp_tonk_to_remote_time_23_t fp_tonk_to_remote_time_23 = nullptr;
static fp_tonk_from_local_time_23_t fp_tonk_from_local_time_23 = nullptr;
static fp_tonk_p2p_connect_t fp_tonk_p2p_connect = nullptr;
static fp_tonk_set_default_socket_config_t fp_tonk_set_default_socket_config = nullptr;

// File Transfer API
static fp_tonk_file_from_disk_t fp_tonk_file_from_disk = nullptr;
static fp_tonk_file_from_buffer_t fp_tonk_file_from_buffer = nullptr;
static fp_tonk_file_send_t fp_tonk_file_send = nullptr;
static fp_tonk_file_receive_t fp_tonk_file_receive = nullptr;
static fp_tonk_file_free_t fp_tonk_file_free = nullptr;


//------------------------------------------------------------------------------
// Library Loader

#ifdef _WIN32
    typedef HMODULE hmodule_t;
    #define TONK_LOAD_LIBRARY(name) ::LoadLibraryA(name)
    #define TONK_GET_PROC_ADDRESS(module, name) ::GetProcAddress(module, name)
#else // _WIN32
    typedef void* hmodule_t;
    #define TONK_LOAD_LIBRARY(name) dlopen(name, RTLD_NOW | RTLD_LOCAL)
    #define TONK_GET_PROC_ADDRESS(module, name) dlsym(module, name)
#endif // _WIN32

#define TONK_LOAD_FUNCTION(name) \
    fp_ ## name = (fp_ ## name ## _t)TONK_GET_PROC_ADDRESS(hModule, #name); \
    if (!fp_ ## name) \
        return false;

static const char* kLibrarySearchPaths_32[] = {
    "tonk",
    "./libtonk.so",
    "x86\\tonk",
    "./x86/libtonk.so",
    "x86\\release\\tonk",
    "x86\\debug\\tonk",
    "..\\dll\\x86\\tonk",
    "..\\..\\dll\\x86\\tonk",
    "..\\..\\..\\dll\\x86\\tonk",
    "debug\\tonk",
    "release\\tonk",
    nullptr
};

static const char* kLibrarySearchPaths_64[] = {
    "tonk",
    "./libtonk.so",
    "x86_64\\tonk",
    "./x86_64/libtonk.so",
    "x64\\tonk",
    "./x64/libtonk.so",
    "x64\\release\\tonk",
    "x64\\debug\\tonk",
    "..\\dll\\x86_64\\tonk",
    "..\\dll\\x64\\tonk",
    "..\\..\\dll\\x86_64\\tonk",
    "..\\..\\dll\\x64\\tonk",
    "..\\..\\..\\dll\\x86_64\\tonk",
    "..\\..\\..\\dll\\x64\\tonk",
    "debug\\tonk",
    "release\\tonk",
    nullptr
};

static bool LoadTonkLibrary()
{
    const char** paths = (sizeof(void*) == 8) ? kLibrarySearchPaths_64 : kLibrarySearchPaths_32;

    std::lock_guard<std::mutex> locker(FunctionPointerLock);
    hmodule_t hModule = nullptr;
    for (unsigned i = 0; paths[i]; ++i)
    {
        hModule = TONK_LOAD_LIBRARY(paths[i]);
#ifdef TONK_CPP_SDK_DLL_VERBOSE
        if (hModule) {
            cout << "LoadTonkLibrary(" << paths[i] << "): Success=" << hModule << endl;
        }
        else {
#ifdef _WIN32
            const char* errStr = "";
#else
            const char* errStr = dlerror();
#endif
            cout << "LoadTonkLibrary(" << paths[i] << "): Failure code = " << errStr << endl;
        }
#endif
        if (hModule)
            break;
    }

    // Main API
    TONK_LOAD_FUNCTION(tonk_result_to_string);
    TONK_LOAD_FUNCTION(tonk_socket_create);
    TONK_LOAD_FUNCTION(tonk_advertise);
    TONK_LOAD_FUNCTION(tonk_inject);
    TONK_LOAD_FUNCTION(tonk_socket_destroy);
    TONK_LOAD_FUNCTION(tonk_connect);
    TONK_LOAD_FUNCTION(tonk_status);
    TONK_LOAD_FUNCTION(tonk_status_ex);
    TONK_LOAD_FUNCTION(tonk_send);
    TONK_LOAD_FUNCTION(tonk_flush);
    TONK_LOAD_FUNCTION(tonk_close);
    TONK_LOAD_FUNCTION(tonk_free);
    TONK_LOAD_FUNCTION(tonk_time);
    TONK_LOAD_FUNCTION(tonk_to_remote_time_16);
    TONK_LOAD_FUNCTION(tonk_from_local_time_16);
    TONK_LOAD_FUNCTION(tonk_to_remote_time_23);
    TONK_LOAD_FUNCTION(tonk_from_local_time_23);
    TONK_LOAD_FUNCTION(tonk_p2p_connect);
    TONK_LOAD_FUNCTION(tonk_set_default_socket_config);

    // File Transfer API
    TONK_LOAD_FUNCTION(tonk_file_from_disk);
    TONK_LOAD_FUNCTION(tonk_file_from_buffer);
    TONK_LOAD_FUNCTION(tonk_file_send);
    TONK_LOAD_FUNCTION(tonk_file_receive);
    TONK_LOAD_FUNCTION(tonk_file_free);

    return true;
}


static void SafeCopyCStr(char* dest, size_t destBytes, const char* src)
{
#if defined(_MSC_VER)
    strncpy_s(dest, destBytes, src, _TRUNCATE);
#else // _MSC_VER
    strncpy(dest, src, destBytes);
#endif // _MSC_VER
    // No null-character is implicitly appended at the end of destination
    dest[destBytes - 1] = '\0';
}


//------------------------------------------------------------------------------
// Function Stubs: Main API

const char* tonk_result_to_string(TonkResult result)
{
    if (!fp_tonk_result_to_string) {
        return "Tonk_DLL_Not_Found";
    }

    return fp_tonk_result_to_string(result);
}

TonkResult tonk_socket_create(
    const TonkSocketConfig* config, // Configuration for the socket
    TonkSocket*          socketOut, // Created socket, or 0 on failure
    TonkJson*         errorJsonOut  // Receives detailed error message on error
)
{
    if (!fp_tonk_socket_create &&
        !LoadTonkLibrary())
    {
        if (socketOut) {
            *socketOut = nullptr;
        }
        if (errorJsonOut) {
            SafeCopyCStr(
                &errorJsonOut->JsonString[0],
                sizeof(TonkJson::JsonString),
                "{ \"description\": \"Tonk DLL Not Found\" }");
        }
        return Tonk_DLL_Not_Found;
    }

    return fp_tonk_socket_create(
        config,
        socketOut,
        errorJsonOut);
}

TonkResult tonk_inject(
    TonkSocket   tonkSocket, ///< [in] Socket to inject
    uint16_t     sourcePort, ///< [in] Source port of datagram
    const void*        data, ///< [in] Datagram data
    unsigned          bytes  ///< [in] Datagram bytes
)
{
    if (!fp_tonk_inject) {
        return Tonk_DLL_Not_Found;
    }

    return fp_tonk_inject(
        tonkSocket,
        sourcePort,
        data,
        bytes);
}

void tonk_socket_destroy(
    TonkSocket          tonkSocket  // Socket to shutdown
)
{
    if (fp_tonk_socket_destroy) {
        fp_tonk_socket_destroy(tonkSocket);
    }
}

TonkResult tonk_advertise(
    TonkSocket tonkSocket, ///< [in] Socket to send advertisement from
    const char*  ipString, ///< [in] Destination IP address
    uint16_t         port, ///< [in] Destination port
    const void*      data, ///< [in] Message data
    unsigned        bytes  ///< [in] Message bytes
)
{
    if (!fp_tonk_advertise) {
        return Tonk_DLL_Not_Found;
    }

    return fp_tonk_advertise(
        tonkSocket,
        ipString,
        port,
        data,
        bytes);
}

TonkResult tonk_connect(
    TonkSocket          tonkSocket, // Socket to connect from
    TonkConnectionConfig    config, // Configuration for the connection
    const char*           hostname, // Hostname of the remote host
    uint16_t                  port, // Port for the remote host
    TonkConnection*  connectionOut, // Set to connection on success, else 0
    TonkJson*         errorJsonOut  // Receives detailed error message on error
)
{
    if (!fp_tonk_connect)
    {
        if (connectionOut) {
            *connectionOut = nullptr;
        }
        if (errorJsonOut) {
            *errorJsonOut = TonkJson();
        }
        return Tonk_DLL_Not_Found;
    }

    return fp_tonk_connect(
        tonkSocket,
        config,
        hostname,
        port,
        connectionOut,
        errorJsonOut);
}

void tonk_status(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatus*          statusOut  ///< [out] Status to return
)
{
    if (fp_tonk_status) {
        fp_tonk_status(
            connection,
            statusOut);
    }
}

void tonk_status_ex(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatusEx*      statusExOut  ///< [out] Extended status to return
)
{
    if (fp_tonk_status_ex) {
        fp_tonk_status_ex(
            connection,
            statusExOut);
    }
}

TonkResult tonk_send(
    TonkConnection      connection, // Connection to send on
    const void*               data, // Pointer to message data
    uint64_t                 bytes, // Message bytes
    uint32_t               channel  // Channel to attach to message
)
{
    if (!fp_tonk_send) {
        return Tonk_DLL_Not_Found;
    }

    return fp_tonk_send(
        connection,
        data,
        bytes,
        channel);
}

void tonk_flush(
    TonkConnection      connection  // Connection to flush
)
{
    if (fp_tonk_flush) {
        fp_tonk_flush(connection);
    }
}

void tonk_close(
    TonkConnection      connection  // Connection to close
)
{
    if (fp_tonk_close) {
        fp_tonk_close(connection);
    }
}

void tonk_free(
    TonkConnection      connection  // Connection to free
)
{
    if (fp_tonk_free) {
        fp_tonk_free(connection);
    }
}

uint64_t tonk_time()
{
    if (!fp_tonk_time) {
        return 0;
    }

    return fp_tonk_time();
}

uint16_t tonk_to_remote_time_16(
    TonkConnection      connection, // Connection with remote host
    uint64_t             localUsec  // Local timestamp in microseconds
)
{
    // No DLL check needed because this function requires a connection
    return fp_tonk_to_remote_time_16(
        connection,
        localUsec);
}

uint64_t tonk_from_local_time_16(
    TonkConnection      connection, // Connection with remote host
    uint16_t    networkTimestamp16  // Network timestamp to interpret
)
{
    // No DLL check needed because this function requires a connection
    return fp_tonk_from_local_time_16(
        connection,
        networkTimestamp16);
}

uint32_t tonk_to_remote_time_23(
    TonkConnection      connection, // Connection with remote host
    uint64_t             localUsec  // Local timestamp in microseconds
)
{
    // No DLL check needed because this function requires a connection
    return fp_tonk_to_remote_time_23(
        connection,
        localUsec);
}

uint64_t tonk_from_local_time_23(
    TonkConnection      connection, // Connection with remote host
    uint32_t    networkTimestamp23  // Network timestamp to interpret
)
{
    // No DLL check needed because this function requires a connection
    return fp_tonk_from_local_time_23(
        connection,
        networkTimestamp23);
}

TonkResult tonk_p2p_connect(
    TonkConnection alice,
    TonkConnection   bob
)
{
    // No DLL check needed because this function requires a connection
    return fp_tonk_p2p_connect(
        alice,
        bob);
}


//------------------------------------------------------------------------------
// Function Stubs: File Transfer API

TonkFile tonk_file_from_disk(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const char*      filepath   ///< Path for file to send
)
{
    if (!fp_tonk_file_from_disk) {
        return 0;
    }

    return fp_tonk_file_from_disk(
        connection,
        channel,
        name,
        filepath);
}

TonkFile tonk_file_from_buffer(
    TonkConnection connection,  ///< Connection to send on
    uint32_t          channel,  ///< Reliable channel for data
    const char*          name,  ///< Transfer name
    const void*          data,  ///< File buffer data pointer
    uint32_t            bytes,  ///< File buffer bytes to send
    uint32_t            flags   ///< Flags for buffer
)
{
    if (!fp_tonk_file_from_buffer) {
        return 0;
    }

    return fp_tonk_file_from_buffer(
        connection,
        channel,
        name,
        data,
        bytes,
        flags);
}

void tonk_file_send(
    TonkFile file ///< File being sent
)
{
    if (fp_tonk_file_send) {
        fp_tonk_file_send(file);
    }
}

TonkFile tonk_file_receive(
    TonkFile     file,  ///< TonkFile to update (or null)
    const void*  data,  ///< Data received
    uint32_t    bytes,  ///< Bytes of data
    FpTonkFileDone fp,  ///< Callback function
    void*     context   ///< Context pointer
)
{
    if (!fp_tonk_file_receive) {
        return 0;
    }

    return fp_tonk_file_receive(
        file,
        data,
        bytes,
        fp,
        context);
}

void tonk_file_free(TonkFile file)
{
    if (fp_tonk_file_free) {
        fp_tonk_file_free(file);
    }
}


} // extern "C"

#endif // !TONK_DISABLE_SHIM


namespace tonk {


//------------------------------------------------------------------------------
// SDKConnection

SDKConnection::SDKConnection()
{
}

SDKConnection::~SDKConnection()
{
    if (MyConnection != nullptr)
    {
        tonk_free(MyConnection);
        MyConnection = nullptr;
    }
}

TonkStatus SDKConnection::GetStatus()
{
    TonkStatus status;
    tonk_status(MyConnection, &status);
    return status;
}

TonkStatusEx SDKConnection::GetStatusEx()
{
    TonkStatusEx statusEx;
    tonk_status_ex(MyConnection, &statusEx);
    return statusEx;
}

SDKResult SDKConnection::Send(
    const void*               data, ///< [in] Pointer to message data
    uint64_t                 bytes, ///< [in] Message bytes
    uint32_t               channel  ///< [in] Channel to attach to message
)
{
    SDKResult sdkResult = tonk_send(
        MyConnection,
        data,
        bytes,
        channel);
    return sdkResult;
}

void SDKConnection::Flush()
{
    tonk_flush(MyConnection);
}

void SDKConnection::Close()
{
    tonk_close(MyConnection);
}

uint64_t SDKConnection::TimeUsec()
{
    return tonk_time();
}

uint16_t SDKConnection::ToRemoteTime16(
    uint64_t             localUsec  ///< [in] Local timestamp in microseconds
)
{
    return tonk_to_remote_time_16(MyConnection, localUsec);
}

uint64_t SDKConnection::FromLocalTime16(
    uint16_t    networkTimestamp16  ///< [in] Local timestamp to interpret
)
{
    return tonk_from_local_time_16(MyConnection, networkTimestamp16);
}

uint32_t SDKConnection::ToRemoteTime23(
    uint64_t             localUsec  ///< [in] Local timestamp in microseconds
)
{
    return tonk_to_remote_time_23(MyConnection, localUsec);
}

uint64_t SDKConnection::FromLocalTime23(
    uint32_t    networkTimestamp23  ///< [in] Local timestamp to interpret
)
{
    return tonk_from_local_time_23(MyConnection, networkTimestamp23);
}

SDKResult SDKConnection::P2PConnect(
    SDKConnection* bobConnection    ///< [in] Peer 2 connection
)
{
    return tonk_p2p_connect(MyConnection, bobConnection ? bobConnection->MyConnection : 0);
}

void SDKConnection::OnConnect()
{
    // Default behavior: No reaction
}

void SDKConnection::OnData(
    uint32_t          /*channel*/,  ///< Channel number attached to each message by sender
    const uint8_t*       /*data*/,  ///< Pointer to a buffer containing the message data
    uint32_t            /*bytes*/   ///< Number of bytes in the message
)
{
    // Default behavior: No reaction
}

void SDKConnection::OnTick(
    uint64_t          /*nowUsec*/   ///< Current timestamp in microseconds
)
{
    // Default behavior: No reaction
}

void SDKConnection::OnClose(
    const tonk::SDKJsonResult& /*reason*/
)
{
    // Default behavior: No reaction
}


//------------------------------------------------------------------------------
// SDKSocket

SDKSocket::SDKSocket()
{
}

SDKSocket::~SDKSocket()
{
    Destroy();
}

TonkConnectionConfig SDKSocket::MakeConnectionConfig(SDKConnection* sdkConnection)
{
    TonkConnectionConfig config;
    config.AppContextPtr = reinterpret_cast<TonkAppContextPtr>(sdkConnection);

    config.OnConnect = [](
        TonkAppContextPtr context,
        TonkConnection /*connection*/)
    {
        SDKConnection* conn = (SDKConnection*)context;
        conn->OnConnect();
    };
    config.OnTick = [](
        TonkAppContextPtr context,
        TonkConnection /*connection*/,
        uint64_t nowUsec)
    {
        SDKConnection* conn = (SDKConnection*)context;
        conn->OnTick(nowUsec);
    };
    config.OnData = [](
        TonkAppContextPtr context,
        TonkConnection /*connection*/,
        uint32_t channel,
        const uint8_t* data,
        uint32_t bytes)
    {
        SDKConnection* conn = (SDKConnection*)context;
        conn->OnData(channel, data, bytes);
    };
    config.OnClose = [](
        TonkAppContextPtr context,
        TonkConnection /*connection*/,
        TonkResult reason,
        const char* reasonJson)
    {
        SDKConnection* conn = (SDKConnection*)context;
        SDKJsonResult result;
        result.ErrorJson = reasonJson;
        result.Result = reason;
        conn->OnClose(result);

        // Release self-reference here, allowing object to go out of scope.
        conn->SelfReference.reset();
    };

    return config;
}

SDKJsonResult SDKSocket::Create()
{
    Destroy();

    TonkSocketConfig config = Config;

    // Override the following members:
    config.AppContextPtr = reinterpret_cast<TonkAppContextPtr>(this);
    config.Version = TONK_VERSION;
    config.OnP2PConnectionStart = [](
        TonkAppContextPtr context,
        TonkConnection connection,
        const TonkAddress* address,
        TonkConnectionConfig* configOut) -> uint32_t
    {
        SDKSocket* thiz = (SDKSocket*)context;
        SDKConnection* sdkConnection = thiz->OnP2PConnectionStart(*address);
        if (!sdkConnection) {
            return TONK_DENY_CONNECTION;
        }

        sdkConnection->MyConnection = connection;
        // Hold a self-reference if connection is started
        sdkConnection->SetSelfReference();

        *configOut = MakeConnectionConfig(sdkConnection);
        return TONK_ACCEPT_CONNECTION;
    };
    config.OnIncomingConnection = [](
        TonkAppContextPtr context,
        TonkConnection connection,
        const TonkAddress* address,
        TonkConnectionConfig* configOut) -> uint32_t
    {
        SDKSocket* thiz = (SDKSocket*)context;
        SDKConnection* sdkConnection = thiz->OnIncomingConnection(*address);
        if (!sdkConnection) {
            return TONK_DENY_CONNECTION;
        }

        sdkConnection->MyConnection = connection;
        // Hold a self-reference if connection is started
        sdkConnection->SelfReference = sdkConnection->shared_from_this();

        *configOut = MakeConnectionConfig(sdkConnection);
        return TONK_ACCEPT_CONNECTION;
    };
    config.OnAdvertisement = [](
        TonkAppContextPtr context,
        TonkSocket /*tonkSocket*/,
        const char* ipString,
        uint16_t port,
        uint8_t* data,
        unsigned bytes)
    {
        SDKSocket* thiz = (SDKSocket*)context;
        thiz->OnAdvertisement(ipString, port, data, bytes);
    };

    TonkJson json;
    TonkResult result = tonk_socket_create(&config, &MySocket, &json);

    SDKJsonResult error;
    if (TONK_FAILED(result))
    {
        error.Result = result;
        error.ErrorJson = json.JsonString;
    }
    return error;
}

SDKJsonResult SDKSocket::Connect(
    SDKConnection* sdkConnection,
    const std::string& hostname, ///< [in] Hostname of the remote host
    uint16_t port ///< [in] Port for the remote host
)
{
    TonkConnectionConfig config = MakeConnectionConfig(sdkConnection);

    TonkJson errorJson;
    TonkResult result = tonk_connect(
        MySocket,
        config,
        hostname.c_str(),
        port,
        &sdkConnection->MyConnection,
        &errorJson);

    SDKJsonResult error;
    if (TONK_FAILED(result))
    {
        error.Result = result;
        error.ErrorJson = errorJson.JsonString;
    }
    else
    {
        // Hold a self-reference if connection is started
        sdkConnection->SelfReference = sdkConnection->shared_from_this();
    }
    return error;
}

void SDKSocket::Destroy()
{
    if (MySocket != 0)
    {
        tonk_socket_destroy(MySocket);
        MySocket = 0;
    }
}

SDKResult SDKSocket::Advertise(
    const std::string& ipString,
    uint16_t port,
    const void* data,
    unsigned bytes)
{
    return tonk_advertise(MySocket, ipString.c_str(), port, data, bytes);
}

SDKConnection* SDKSocket::OnIncomingConnection(
    const TonkAddress& /*address*/)  ///< Address of the client requesting a connection
{
    // Deny connections by default
    return nullptr;
}

SDKConnection* SDKSocket::OnP2PConnectionStart(
    const TonkAddress& /*address*/)  ///< Address of the other peer we are to connect to
{
    // Deny connections by default
    return nullptr;
}

void SDKSocket::OnAdvertisement(
    const std::string& /*ipString*/, ///< Source IP address of advertisement
    uint16_t               /*port*/, ///< Source port address of advertisement
    const uint8_t*         /*data*/,  ///< Pointer to a buffer containing the message data
    uint32_t              /*bytes*/)  ///< Number of bytes in the message
{
    // Ignore advertisements by default
}


} // namespace tonk
