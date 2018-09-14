#include "mau.h"

#include "MauProxy.h"

using namespace mau;


//------------------------------------------------------------------------------
// Logger

static Lock LoggerLock;
static uint64_t LoggerRefs = 0;

static void IncrementLoggerRefs()
{
    Locker locker(LoggerLock);

    if (LoggerRefs == 0)
        logger::Start();

    ++LoggerRefs;
}

static void DecrementLoggerRefs()
{
    Locker locker(LoggerLock);

    --LoggerRefs;
    MAU_DEBUG_ASSERT(LoggerRefs >= 0);

#ifdef MAU_DLL
    // Stop logger when built as DLL
    if (LoggerRefs == 0)
        logger::Stop();
#endif
}


//------------------------------------------------------------------------------
// MauProxy API

static bool ValidateMauProxyConfig(const MauProxyConfig& config)
{
    if (!config.SendHook)
    {
        if (config.UDPListenPort <= 0 || config.UDPListenPort >= 65536) {
            return false;
        }

        if (config.UDPRecvBufferSizeBytes <= 2000) {
            return false;
        }
        if (config.UDPSendBufferSizeBytes <= 2000) {
            return false;
        }
    }

    if (config.MaxDatagramBytes < 1) {
        return false;
    }

    return true;
}

static bool IsWithin01(float f)
{
    return f >= 0.f && f <= 1.f;
}

static bool ValidateMauChannelConfig(const MauChannelConfig& config)
{
    if (!IsWithin01(config.LossRate)) {
        return false;
    }
    if (!IsWithin01(config.DeliveryRate)) {
        return false;
    }

    if (config.Router_MBPS <= 0.01f) {
        return false;
    }
    if (config.Router_QueueMsec <= 10) {
        return false;
    }

    if (config.ReorderMaximumLatencyMsec < config.ReorderMinimumLatencyMsec) {
        return false;
    }

    if (!IsWithin01(config.ReorderRate)) {
        return false;
    }
    if (!IsWithin01(config.OrderRate)) {
        return false;
    }

    if (!IsWithin01(config.DuplicateRate)) {
        return false;
    }
    if (!IsWithin01(config.CorruptionRate)) {
        return false;
    }

    return true;
}

MAU_EXPORT MauResult mau_proxy_create(
    const MauProxyConfig*    config, ///< [in] Configuration for the socket
    const MauChannelConfig* channel, ///< [in] Channel configuration
    const char*            hostname, ///< [in] Hostname of the server
    uint16_t                   port, ///< [in] UDP Port for the server
    MauProxy*              proxyOut  ///< [out] Created socket, or 0 on failure
)
{
    IncrementLoggerRefs();

    ProxySession* session = nullptr;
    MauResult result = Mau_Error;

    if (!config || !proxyOut)
    {
        MAU_DEBUG_BREAK(); // Invalid input
        result = Mau_InvalidInput;
        goto OnError;
    }

    if (config->Version != MAU_VERSION)
    {
        MAU_DEBUG_BREAK(); // Invalid input
        result = Mau_InvalidInput;
        goto OnError;
    }

    if (!ValidateMauProxyConfig(*config))
    {
        MAU_DEBUG_BREAK(); // Invalid input
        result = Mau_InvalidInput;
        goto OnError;
    }

    if (!ValidateMauChannelConfig(*channel))
    {
        MAU_DEBUG_BREAK(); // Invalid input
        result = Mau_InvalidInput;
        goto OnError;
    }

    session = new (std::nothrow) ProxySession;
    if (!session)
    {
        MAU_DEBUG_BREAK(); // Out of memory
        result = Mau_OOM;
        goto OnError;
    }

    result = session->Initialize(hostname, port, *config, *channel);
    if (MAU_FAILED(result))
    {
        delete session;
        session = nullptr;
        goto OnError;
    }

OnError:
    if (proxyOut)
        *proxyOut = reinterpret_cast<MauProxy>(session);

    if (!session)
        DecrementLoggerRefs();

    return result;
}

MAU_EXPORT MauResult mau_proxy_inject(
    MauProxy proxy, ///< [in] Proxy object
    uint16_t sourcePort, ///< [in] Source port
    const void* data, ///< [in] Datagram buffer
    unsigned bytes ///< [in] Datagram bytes
)
{
    auto session = reinterpret_cast<ProxySession*>(proxy);
    if (!session || !data || bytes == 0)
        return Mau_InvalidInput;

    return session->Inject(sourcePort, data, bytes);
}

MAU_EXPORT void mau_proxy_config(
    MauProxy proxy, ///< [in] Proxy to shutdown
    const MauChannelConfig* config /// [in] Channel configuration
)
{
    auto session = reinterpret_cast<ProxySession*>(proxy);
    if (session && config)
    {
        session->SetChannelConfig(*config);
    }
}

MAU_EXPORT void mau_proxy_stop(
    MauProxy   proxy ///< [in] Proxy object
)
{
    auto session = reinterpret_cast<ProxySession*>(proxy);
    if (session)
    {
        session->Shutdown();
    }
}

MAU_EXPORT MauResult mau_proxy_destroy(
    MauProxy proxy ///< [in] Proxy to shutdown
)
{
    MauResult result = Mau_Success;

    auto session = reinterpret_cast<ProxySession*>(proxy);
    if (session)
    {
        result = session->GetLastResult();

        session->Shutdown();

        // Object goes out of scope here----------------------------------------
        delete session;

        DecrementLoggerRefs();
    }

    return result;
}
