/*
    Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

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

#include "tonk.h"
#include "TonkineseConnection.h"
#include "TonkineseFirewall.h"
#include "TonkineseSession.h"
#include "siamese.h"
#include "cymric.h"
#include "WLANOptimizer.h"

using namespace tonk;


//------------------------------------------------------------------------------
// Helpers

static TonkResult TonkResultFromDetailedResult(const Result& result)
{
    if (result.IsGood())
        return Tonk_Success;

    switch (result.Error->Type)
    {
    case ErrorType::Tonk:    return (TonkResult)result.Error->Code;
    case ErrorType::Asio:    return Tonk_NetworkFailed;
    case ErrorType::Cymric:  return Tonk_PRNGFailed;
    case ErrorType::Siamese: return Tonk_FECFailed;
    case ErrorType::LastErr: return Tonk_NetworkFailed; // TBD: Disambiguate this?
    default:
        break;
    }

    return Tonk_Error;
}

static Result ValidateTonkConnectionConfig(const TonkConnectionConfig* config)
{
    // OnData can be null if this is purely a send socket.
    // OnTick is null for simple clients/servers that do not need it.

    // OnClose can be null if no other callbacks are specified.
    if (!config->OnClose && (config->OnTick || config->OnData))
    {
        return Result("ValidateTonkConnectionConfig",
            "OnClose cannot be null: Other callbacks are specified and " \
            "could be called after tonk_free().  The application should " \
            "use OnClose() to call tonk_free().", ErrorType::Tonk, Tonk_InvalidInput);
    }

    return Result::Success();
}

static Result ValidateTonkSocketConfig(const TonkSocketConfig* config)
{
    if (config->UDPConnectIntervalUsec < 30 * 1000) {
        return Result("ValidateTonkSocketConfig", "UDPConnectIntervalUsec < 30 ms invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }
    if (config->UDPConnectIntervalUsec > 1000 * 1000) {
        return Result("ValidateTonkSocketConfig", "UDPConnectIntervalUsec > 1 second invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }

    if (config->ConnectionTimeoutUsec < 1000 * 1000) {
        return Result("ValidateTonkSocketConfig", "ConnectionTimeoutUsec < 1 second invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }
    if (config->ConnectionTimeoutUsec > 3 * 60 * 1000 * 1000) {
        return Result("ValidateTonkSocketConfig", "ConnectionTimeoutUsec > 3 minutes invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }

    // OnIncomingConnection is null if no new connections are allowed.
    // OnOutgoingConnection is null if no outgoing connections will be made
    // OnP2PConnectionStart is null if no P2P connections are allowed
    // or if no connection callback is needed.

    // If UDP port is set but incoming connection is not set:
    if (config->UDPListenPort != 0 && !config->OnIncomingConnection) {
        return Result("ValidateTonkSocketConfig", "OnIncomingConnection null", ErrorType::Tonk, Tonk_InvalidInput);
    }

    if (config->UDPListenPort > 65535) {
        return Result("ValidateTonkSocketConfig", "UDPPort invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }

    if (config->TimerIntervalUsec < TONK_TIMER_INTERVAL_MIN) {
        return Result("ValidateTonkSocketConfig", "TimerIntervalMsec < TONK_TIMER_INTERVAL_MIN invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }
    if (config->TimerIntervalUsec > TONK_TIMER_INTERVAL_MAX) {
        return Result("ValidateTonkSocketConfig", "TimerIntervalMsec > TONK_TIMER_INTERVAL_MAX invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }

    if (config->NoDataTimeoutUsec < TONK_NO_DATA_TIMEOUT_USEC_MIN) {
        return Result("ValidateTonkSocketConfig", "NoDataTimeoutUsec < TONK_NO_DATA_TIMEOUT_USEC_MIN invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }
    if (config->NoDataTimeoutUsec > TONK_NO_DATA_TIMEOUT_USEC_MAX) {
        return Result("ValidateTonkSocketConfig", "NoDataTimeoutUsec > TONK_NO_DATA_TIMEOUT_USEC_MAX invalid", ErrorType::Tonk, Tonk_InvalidInput);
    }

    return Result::Success();
}


extern "C" {


//------------------------------------------------------------------------------
// Tonk Constants Helpers

TONK_EXPORT const char* tonk_result_to_string(TonkResult result)
{
    static_assert(TonkResult_Count == 18, "Update this too");
    switch (result)
    {
    case Tonk_Success: return "Tonk_Success";
    case Tonk_InvalidInput: return "Tonk_InvalidInput";
    case Tonk_Error: return "Tonk_Error";
    case Tonk_HostnameLookup: return "Tonk_HostnameLookup";
    case Tonk_InvalidChannel: return "Tonk_InvalidChannel";
    case Tonk_MessageTooLarge: return "Tonk_MessageTooLarge";
    case Tonk_NetworkFailed: return "Tonk_NetworkFailed";
    case Tonk_PRNGFailed: return "Tonk_PRNGFailed";
    case Tonk_FECFailed: return "Tonk_FECFailed";
    case Tonk_InvalidData: return "Tonk_InvalidData";
    case Tonk_ConnectionRejected: return "Tonk_ConnectionRejected";
    case Tonk_ConnectionTimeout: return "Tonk_ConnectionTimeout";
    case Tonk_AppRequest: return "Tonk_AppRequest";
    case Tonk_RemoteRequest: return "Tonk_RemoteRequest";
    case Tonk_RemoteTimeout: return "Tonk_RemoteTimeout";
    case Tonk_BogonData: return "Tonk_BogonData";
    case Tonk_OOM: return "Tonk_OOM";
    case Tonk_DLL_Not_Found: return "Tonk_DLL_Not_Found";
    default: break;
    }
    return "Unknown";
}


//------------------------------------------------------------------------------
// Session Counter

static Lock m_SessionCounterLock;
static uint64_t m_SessionCounter = 0;

static void IncrementSessionCounter()
{
    Locker locker(m_SessionCounterLock);

    // If first session started:
    if (m_SessionCounter == 0)
    {
        // Start background utility threads:

        // Must be first
        logger::Start();

        StartWLANOptimizerThread();
        Firewall_Initialize();
    }

    ++m_SessionCounter;
}

static void DecrementSessionCounter()
{
    Locker locker(m_SessionCounterLock);

    TONK_DEBUG_ASSERT(m_SessionCounter >= 1);
    --m_SessionCounter;

    // If all sessions ended:
    if (m_SessionCounter == 0)
    {
        // Stop background utility threads:

        Firewall_Shutdown();
        StopWLANOptimizerThread();

#ifdef TONK_DLL
        // Must be last
        logger::Stop();
#endif
}
}


//------------------------------------------------------------------------------
// TonkSocket API

TONK_EXPORT TonkResult tonk_socket_create(
    const TonkSocketConfig* config, // Configuration for the socket
    TonkSocket*          socketOut, // Created socket, or 0 on failure
    TonkJson*         errorJsonOut  // Receives detailed error message on error
)
{
    IncrementSessionCounter();

    ApplicationSession* session = nullptr;
    Result result;
    int siameseResult;

    if (!config || !socketOut)
    {
        result = Result("tonk_socket_create", "Null parameter");
        TONK_DEBUG_BREAK(); // Invalid input
        goto OnError;
    }

    if (config->Version != TONK_VERSION)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        result = Result("tonk_socket_create", "config.Version does not match TONK_VERSION in header", ErrorType::Tonk, Tonk_InvalidInput);
        goto OnError;
    }

    siameseResult = siamese_init();
    if (0 != siameseResult)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        result = Result("tonk_socket_create", "siamese_init failed", ErrorType::Siamese, siameseResult);
        goto OnError;
    }

    result = SecureRandom_Initialize();
    if (result.IsFail())
    {
        TONK_DEBUG_BREAK(); // Should never happen
        goto OnError;
    }

    result = InitializeAddressHash();
    if (result.IsFail())
    {
        TONK_DEBUG_BREAK(); // PRNG failed
        goto OnError;
    }

    result = ValidateTonkSocketConfig(config);
    if (result.IsFail())
    {
        TONK_DEBUG_BREAK(); // Invalid input
        goto OnError;
    }

    session = new (std::nothrow) ApplicationSession;
    if (!session)
    {
        TONK_DEBUG_BREAK(); // Out of memory
        result = Result("tonk_socket_create", "Out of memory");
        goto OnError;
    }

    result = session->Initialize(*config);
    if (result.IsFail())
    {
        delete session;
        session = nullptr;
        goto OnError;
    }

OnError:
    if (socketOut)
        *socketOut = reinterpret_cast<TonkSocket>(session);

    if (!session)
        DecrementSessionCounter();

    // Write optional error json object
    if (errorJsonOut)
        SafeCopyCStr(
            errorJsonOut->JsonString,
            TONK_ERROR_JSON_MAX_BYTES,
            result.ToJson().c_str());
    return TonkResultFromDetailedResult(result);
}

TONK_EXPORT TonkResult tonk_advertise(
    TonkSocket tonkSocket, ///< [in] Socket to send advertisement from
    const char*  ipString, ///< [in] Destination IP address
    uint16_t         port, ///< [in] Destination port
    const void*      data, ///< [in] Message data
    unsigned        bytes  ///< [in] Message bytes
)
{
    auto session = reinterpret_cast<ApplicationSession*>(tonkSocket);
    if (!session || !ipString || port == 0 ||
        (data == nullptr && bytes > 0) ||
        bytes > TONK_MAX_UNRELIABLE_BYTES)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return Tonk_InvalidInput;
    }

    Result result = session->tonk_advertise(ipString, port, data, bytes);
    return TonkResultFromDetailedResult(result);
}

TONK_EXPORT TonkResult tonk_inject(
    TonkSocket   tonkSocket, ///< [in] Socket to inject
    uint16_t     sourcePort, ///< [in] Source port of datagram
    const void*        data, ///< [in] Datagram data
    unsigned          bytes  ///< [in] Datagram bytes
)
{
    auto session = reinterpret_cast<ApplicationSession*>(tonkSocket);
    if (!session || sourcePort == 0 || !data || bytes <= 0)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return Tonk_InvalidInput;
    }

    Result result = session->tonk_inject(
        sourcePort,
        reinterpret_cast<const uint8_t*>(data),
        bytes);
    return TonkResultFromDetailedResult(result);
}

TONK_EXPORT void tonk_socket_destroy(
    TonkSocket tonkSocket  ///< Socket to shutdown
)
{
    auto session = reinterpret_cast<ApplicationSession*>(tonkSocket);
    if (session)
    {
        session->tonk_socket_destroy();

        // Object goes out of scope here----------------------------------------
        delete session;

        DecrementSessionCounter();
    }
}


//------------------------------------------------------------------------------
// TonkConnection API

TONK_EXPORT TonkResult tonk_connect(
    TonkSocket          tonkSocket, // Socket to connect from
    TonkConnectionConfig    config, // Configuration for the connection
    const char*           hostname, // Hostname of the remote host
    uint16_t                  port, // Port for the remote host
    TonkConnection*  connectionOut, // Set to connection on success, else 0
    TonkJson*         errorJsonOut  // Receives detailed error message on error
)
{
    SIAMESE_DEBUG_ASSERT(hostname != nullptr); // Invalid input

    Result result;

    if (!tonkSocket || !hostname || !connectionOut)
        result = Result("tonk_connect", "Null parameter");
    else
    {
        *connectionOut = nullptr;

        result = ValidateTonkConnectionConfig(&config);
        if (result.IsFail())
        {
            TONK_DEBUG_BREAK(); // Invalid input
        }
        else
        {
            ApplicationSession* session = reinterpret_cast<ApplicationSession*>(tonkSocket);

            if (port == 0)
                result = Result("tonk_connect", "Port was not set");
            else
            {
                result = session->tonk_connect(
                    config,
                    hostname,
                    port,
                    connectionOut);
            }
        }
    }

    // Write optional error json object
    if (errorJsonOut)
        SafeCopyCStr(
            errorJsonOut->JsonString,
            TONK_ERROR_JSON_MAX_BYTES,
            result.ToJson().c_str());
    return TonkResultFromDetailedResult(result);
}

TONK_EXPORT void tonk_status(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatus*          statusOut  ///< [out] Status to return
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (tonkConnection && statusOut)
    {
        tonkConnection->tonk_status(*statusOut);
        return;
    }
    TONK_DEBUG_BREAK(); // Invalid input
}

TONK_EXPORT void tonk_status_ex(
    TonkConnection      connection, ///< [in] Connection to query
    TonkStatusEx*      statusExOut  ///< [out] Extended status to return
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (tonkConnection && statusExOut)
    {
        tonkConnection->tonk_status_ex(*statusExOut);
        return;
    }
    TONK_DEBUG_BREAK(); // Invalid input
}

TONK_EXPORT TonkResult tonk_send(
    TonkConnection      connection, // Connection to send on
    const void*               data, // Pointer to message data
    uint64_t                 bytes, // Message bytes
    uint32_t               channel  // Channel to attach to message
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return Tonk_InvalidInput;
    }

    Result result = tonkConnection->tonk_send(
        channel,
        reinterpret_cast<const uint8_t*>(data),
        bytes);
    return TonkResultFromDetailedResult(result);
}

TONK_EXPORT void tonk_flush(
    TonkConnection      connection  // Connection to flush
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return;
    }

    tonkConnection->tonk_flush();
}

TONK_EXPORT void tonk_close(
    TonkConnection      connection  // Connection to close
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return;
    }

    tonkConnection->tonk_close();
}

TONK_EXPORT void tonk_free(
    TonkConnection      connection  // Connection to free
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return;
    }

    tonkConnection->tonk_free();
}


//------------------------------------------------------------------------------
// Time Synchronization API

TONK_EXPORT uint64_t tonk_time()
{
    return siamese::GetTimeUsec();
}

TONK_EXPORT uint16_t tonk_to_remote_time_16(
    TonkConnection      connection, // Connection with remote host
    uint64_t             localUsec  // Local timestamp in microseconds
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return 0;
    }

    return tonkConnection->tonk_to_remote_time_16(localUsec);
}

TONK_EXPORT uint64_t tonk_from_local_time_16(
    TonkConnection      connection, // Connection with remote host
    uint16_t    networkTimestamp16  // Network timestamp to interpret
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return 0;
    }

    return tonkConnection->tonk_from_local_time_16(networkTimestamp16);
}

TONK_EXPORT uint32_t tonk_to_remote_time_23(
    TonkConnection      connection, // Connection with remote host
    uint64_t             localUsec  // Local timestamp in microseconds
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return 0;
    }

    return tonkConnection->tonk_to_remote_time_23(localUsec);
}

TONK_EXPORT uint64_t tonk_from_local_time_23(
    TonkConnection      connection, // Connection with remote host
    uint32_t    networkTimestamp23  // Network timestamp to interpret
)
{
    Connection* tonkConnection = reinterpret_cast<Connection*>(connection);
    if (!tonkConnection)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return 0;
    }

    return tonkConnection->tonk_from_local_time_23(networkTimestamp23);
}


//------------------------------------------------------------------------------
// Peer2Peer

TONK_EXPORT TonkResult tonk_p2p_connect(
    TonkConnection alice,
    TonkConnection   bob
)
{
    if (!alice || !bob)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return Tonk_InvalidInput;
    }

    Connection* tonkAlice = reinterpret_cast<Connection*>(alice);
    Connection* tonkBob = reinterpret_cast<Connection*>(bob);

    Result result = tonkAlice->tonk_p2p_connect(tonkBob);
    return TonkResultFromDetailedResult(result);
}


//------------------------------------------------------------------------------
// Extra Tools

TONK_EXPORT TonkResult tonk_set_default_socket_config(
    uint32_t tonk_version,
    TonkSocketConfig* configOut)
{
    // NOTE: We cannot use the logger in this code path because it may have
    // been shutdown.

    if (!configOut || tonk_version != TONK_VERSION)
    {
        TONK_DEBUG_BREAK(); // Invalid input
        return Tonk_InvalidInput;
    }

    *configOut = TonkSocketConfig();
    return Tonk_Success;
}

// Check TimeSync constannts
static_assert(TONK_TIME23_LSB_USEC == (1 << kTime23LostBits), "Update this");
static_assert((TONK_TIME23_MAX_FUTURE_USEC + 1) == (kTime23Bias << kTime23LostBits), "Update this");
static_assert((TONK_TIME23_MAX_FUTURE_USEC + TONK_TIME23_MAX_PAST_USEC + 1) == ((1 << 23) << kTime23LostBits), "Update this");
static_assert(TONK_TIME16_LSB_USEC == (1 << kTime16LostBits), "Update this");
static_assert((TONK_TIME16_MAX_FUTURE_USEC + 1) == (kTime16Bias << kTime16LostBits), "Update this");
static_assert((TONK_TIME16_MAX_FUTURE_USEC + TONK_TIME16_MAX_PAST_USEC + 1) == ((1 << 16) << kTime16LostBits), "Update this");


} // extern "C"
