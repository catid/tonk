/** \file
    \brief Tonk Implementation: Tools
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

#include "TonkineseTools.h"
#include "thirdparty/t1ha.h"

#include <fstream> // ofstream
#include <iomanip> // setw, setfill

#if !defined(_WIN32)
    #include <pthread.h>
    #include <unistd.h>
#endif // _WIN32

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif __MACH__
    #include <mach/mach_time.h>
    #include <mach/mach.h>
    #include <mach/clock.h>

    extern mach_port_t clock_port;
#else
    #include <time.h>
    #include <sys/time.h>
#endif

namespace tonk {

static logger::Channel Logger("Tools", MinimumLogLevel);


//------------------------------------------------------------------------------
// Thread Tools

#ifdef _WIN32
const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push,8)
typedef struct tagTHREADNAME_INFO
{
    DWORD dwType;       // Must be 0x1000.
    LPCSTR szName;      // Pointer to name (in user addr space).
    DWORD dwThreadID;   // Thread ID (-1=caller thread).
    DWORD dwFlags;      // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)
void SetCurrentThreadName(const char* threadName)
{
    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = threadName;
    info.dwThreadID = ::GetCurrentThreadId();
    info.dwFlags = 0;
#pragma warning(push)
#pragma warning(disable: 6320 6322)
    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
#pragma warning(pop)
}
#else
void SetCurrentThreadName(const char* threadName)
{
    pthread_setname_np(pthread_self(), threadName);
}
#endif


//------------------------------------------------------------------------------
// IP Address Integer Hash

/**
    Secret seed that changes each time the application runs.
    This prevents attackers from being able to easily predict hash values.

    TBD: This is pretty weak.
    (1) The seed is not very long.
    (2) The hash algorithm may be biased or reversible.
    (3) Timing attacks may reveal the seed.
    However I think this is good enough for now for hash tables.

    For hash values sent over the Internet like SYN cookies, this
    is not sufficient.  Additional obfuscation must be applied in the
    TonkineseFlood module.
*/
static uint64_t m_SecretSeed = 0;
static bool m_SeedInitialized = false;

Result InitializeAddressHash()
{
    Result result = SecureRandom_Next((uint8_t*)&m_SecretSeed, sizeof(m_SecretSeed));
    if (result.IsFail()) {
        return result;
    }

    m_SeedInitialized = true;
    return Result::Success();
}

uint64_t HashUDPAddress(const UDPAddress& addr)
{
    // InitializeAddressHash() must be called first
    TONK_DEBUG_ASSERT(m_SeedInitialized);

    if (addr.address().is_v4())
    {
        const asio::ip::address_v4::bytes_type TONK_ALIGNED(16) v4bytes = \
            addr.address().to_v4().to_bytes();
        return t1ha_local(&v4bytes[0], v4bytes.size(), m_SecretSeed ^ addr.port());
    }

    const asio::ip::address_v6::bytes_type TONK_ALIGNED(16) v6bytes = \
        addr.address().to_v6().to_bytes();
    return t1ha_local(&v6bytes[0], v6bytes.size(), m_SecretSeed ^ addr.port());
}


//------------------------------------------------------------------------------
// String Tools

std::string HexString(uint64_t value)
{
    static const char* H = "0123456789abcdef";

    char hex[16 + 1];
    hex[16] = '\0';

    char* hexWrite = &hex[14];
    for (unsigned i = 0; i < 8; ++i)
    {
        hexWrite[1] = H[value & 15];
        hexWrite[0] = H[(value >> 4) & 15];

        value >>= 8;
        if (value == 0)
            return hexWrite;
        hexWrite -= 2;
    }

    return hex;
}

std::string EscapeJsonString(const std::string& s)
{
    std::ostringstream o;
    const int slen = static_cast<int>(s.length());

    for (int i = 0; i < slen; ++i)
    {
        const char ch = s[i];
        switch (ch)
        {
        case '"':  o << "\\\""; break;
        case '\\': o << "\\\\"; break;
        case '\b': o << "\\b";  break;
        case '\f': o << "\\f";  break;
        case '\n': o << "\\n";  break;
        case '\r': o << "\\r";  break;
        case '\t': o << "\\t";  break;
        default:
            if ('\x00' <= ch && ch <= '\x1f')
                o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(ch);
            else
                o << ch;
        }
    }

    return o.str();
}

void SetTonkAddressStr(TonkAddress& addrOut, const UDPAddress& addrIn)
{
    addrOut.UDPPort = addrIn.port();

    const auto ip_addr = addrIn.address();
    if (ip_addr.is_unspecified())
    {
        memset(addrOut.NetworkString, 0, sizeof(addrOut.NetworkString));
        return;
    }

    const std::string addrstr = ip_addr.to_string();

    size_t slen = addrstr.size();
    if (slen > TONK_IP_STRING_MAX_CHARS)
        slen = TONK_IP_STRING_MAX_CHARS;
    static_assert(sizeof(addrOut.NetworkString) == TONK_IP_STRING_BYTES, "Update this");

    for (size_t i = 0; i < slen; ++i)
        addrOut.NetworkString[i] = addrstr[i];
    for (size_t i = slen; i < TONK_IP_STRING_BYTES; ++i)
        addrOut.NetworkString[i] = '\0';
}


static uint16_t const str100p[100] = {
    0x3030, 0x3130, 0x3230, 0x3330, 0x3430, 0x3530, 0x3630, 0x3730, 0x3830, 0x3930,
    0x3031, 0x3131, 0x3231, 0x3331, 0x3431, 0x3531, 0x3631, 0x3731, 0x3831, 0x3931,
    0x3032, 0x3132, 0x3232, 0x3332, 0x3432, 0x3532, 0x3632, 0x3732, 0x3832, 0x3932,
    0x3033, 0x3133, 0x3233, 0x3333, 0x3433, 0x3533, 0x3633, 0x3733, 0x3833, 0x3933,
    0x3034, 0x3134, 0x3234, 0x3334, 0x3434, 0x3534, 0x3634, 0x3734, 0x3834, 0x3934,
    0x3035, 0x3135, 0x3235, 0x3335, 0x3435, 0x3535, 0x3635, 0x3735, 0x3835, 0x3935,
    0x3036, 0x3136, 0x3236, 0x3336, 0x3436, 0x3536, 0x3636, 0x3736, 0x3836, 0x3936,
    0x3037, 0x3137, 0x3237, 0x3337, 0x3437, 0x3537, 0x3637, 0x3737, 0x3837, 0x3937,
    0x3038, 0x3138, 0x3238, 0x3338, 0x3438, 0x3538, 0x3638, 0x3738, 0x3838, 0x3938,
    0x3039, 0x3139, 0x3239, 0x3339, 0x3439, 0x3539, 0x3639, 0x3739, 0x3839, 0x3939, };

static unsigned const PowersOf10[] = {
    1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000, 1000000000, };

unsigned inplace_itoa_vitaut_1_cat(char* buf, uint32_t val)
{
    // Common case of zero
    if (val == 0) {
        buf[0] = '0';
        return 1;
    }

    const unsigned bits = NonzeroLowestBitIndex(val);
    const unsigned t = (bits + 1) * 1233 >> 12;
    const unsigned digits = t + 1 - (val < PowersOf10[t]);

    buf += digits;

    while (val >= 100)
    {
        uint32_t const old = val;

        buf -= 2;
        val /= 100;
#ifdef _MSC_VER
        *(uint16_t*)buf = str100p[old - (val * 100)];
#else
        memcpy(buf, &str100p[old - (val * 100)], sizeof(uint16_t));
#endif
    }

    if (val >= 10)
    {
        buf -= 2;
#ifdef _MSC_VER
        *(uint16_t*)buf = str100p[val];
#else
        memcpy(buf, &str100p[val], sizeof(uint16_t));
#endif
    }
    else
    {
        buf[-1] = (char)(uint8_t)(str100p[val] >> 8);
    }

    return digits;
}


//------------------------------------------------------------------------------
// Result

const char* ErrorTypeToString(ErrorType errorType)
{
    static_assert((int)ErrorType::Count == 7, "Update this");
    switch (errorType)
    {
    case ErrorType::Tonk:    return "Tonk";
    case ErrorType::Siamese: return "Siamese";
    case ErrorType::Asio:    return "Asio";
    case ErrorType::LastErr: return "LastErr";
    case ErrorType::Cymric:  return "Cymric";
    case ErrorType::Encrypt: return "Encrypt";
    case ErrorType::Zstd:    return "Zstd";
    default: break;
    }
    return "Unknown";
}

std::string ErrorResult::ToJson() const
{
    std::ostringstream oss;
    oss << "{ \"source\": " << JsonValue(Source);
    oss << ", \"description\": " << JsonValue(Description);
    oss << ", \"error_type\": " << JsonValue(ErrorTypeToString(Type));
    if (Type == ErrorType::Tonk)
        oss << ", \"error_code\": " << JsonValue(tonk_result_to_string((TonkResult)Code));
    else
        oss << ", \"error_code\": " << JsonValue(Code);
    oss << " }";
    return oss.str();
}

std::string Result::ToJson() const
{
    if (Error)
        return Error->ToJson();
    return "{}";
}


//------------------------------------------------------------------------------
// Timer

void Timer::SetTimeoutUsec(uint64_t timeoutUsec, Behavior resetBehavior)
{
    TONK_DEBUG_ASSERT(timeoutUsec >= kTickDivisions); // Invalid input

    ResetBehavior = resetBehavior;
    UsecPerDivision = timeoutUsec / kTickDivisions;
}

void Timer::Start(uint64_t nowUsec)
{
    TONK_DEBUG_ASSERT(UsecPerDivision != 0); // Must SetTimeout() first
    WaitStartUsec = nowUsec;
    DivisionsExpired = 0;
    IsTicking = true;
}

void Timer::ForceExpire()
{
    if (!IsTicking) {
        return;
    }

    // Set up state to expire right away
    WaitStartUsec = 0; // Should be long before current time
    DivisionsExpired = kTickDivisions; // Expire next time
}

bool Timer::IsExpired(uint64_t nowUsec)
{
    if (!IsTicking) {
        return false;
    }

    const uint64_t elapsedUsec = static_cast<uint64_t>(nowUsec - WaitStartUsec);

    // If not enough time has elapsed yet:
    if (elapsedUsec < UsecPerDivision) {
        return false;
    }

    // If we haven't hit the 4rth tick yet:
    if (++DivisionsExpired < kTickDivisions)
    {
        // Use the latest timestamp and wait again
        WaitStartUsec = nowUsec;
        return false;
    }

    if (ResetBehavior == Behavior::Stop) {
        IsTicking = false;
    }

    WaitStartUsec = nowUsec;
    DivisionsExpired = 0;
    return true;
}

void Timer::Stop()
{
    IsTicking = false;
}


//------------------------------------------------------------------------------
// SecureRandom

static Lock PrngLock;
static bool PrngInitialized = false;
static cymric_rng Prng;

Result SecureRandom_Initialize()
{
    Locker locker(PrngLock);
    if (PrngInitialized)
        return Result::Success();

    int cymricResult = cymric_init();
    if (0 != cymricResult)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return Result("tonk_init", "cymric_init failed", ErrorType::Cymric, cymricResult);
    }

    cymricResult = cymric_seed(&Prng, nullptr, 0);
    if (0 != cymricResult)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return Result("SecureRandom_Initialize", "cymric_seed failed", ErrorType::Cymric, cymricResult);
    }

    PrngInitialized = true;
    return Result::Success();
}

Result SecureRandom_Next(uint8_t* key, unsigned bytes)
{
    Locker locker(PrngLock);
    TONK_DEBUG_ASSERT(PrngInitialized);

    int cymricResult = cymric_random(&Prng, key, static_cast<int>(bytes));
    if (0 != cymricResult)
    {
        TONK_DEBUG_BREAK(); // Should never happen
        return Result("SecureRandom_Next", "cymric_random failed", ErrorType::Cymric, cymricResult);
    }

    return Result::Success();
}


//------------------------------------------------------------------------------
// Detailed Statistics Memory

#ifdef TONK_DETAILED_STATS

const char* GetStatsTypesName(unsigned type)
{
    static_assert(Stats_Count == 17, "Update this");
    switch (type)
    {
    case Stats_FramingBytesSent: return "FramingBytesSent";
    case Stats_AcksBytesSent: return "AcksBytesSent";
    case Stats_UnreliableBytesSent: return "UnreliableBytesSent";
    case Stats_UnorderedBytesSent: return "UnorderedBytesSent";
    case Stats_ControlBytesSent: return "ControlBytesSent";
    case Stats_ReliableBytesSent: return "ReliableBytesSent";
    case Stats_LowPriBytesSent: return "LowPriBytesSent";
    case Stats_RetransmitSent: return "RetransmitSent";
    case Stats_CompressedBytesSent: return "CompressedBytesSent";
    case Stats_CompressionSavedBytes: return "CompressionSavedBytes";
    case Stats_PaddingBytesSent: return "PaddingBytesSent";
    case Stats_FootersBytesSent: return "FootersBytesSent";
    case Stats_SiameseBytesSent: return "SiameseBytesSent";
    case Stats_DummyBytesSent: return "DummyBytesSent";
    case Stats_BytesReceived: return "BytesReceived";
    case Stats_MinUsecOWD: return "MinUsecOWD";
    case Stats_MaxUsecOWD: return "MaxUsecOWD";
    default: break;
    }
    TONK_DEBUG_BREAK(); // Invalid input
    return "Unknown";
}

void DetailStatsMemory::Clear()
{
    for (unsigned i = 0, size = RegionPtrs.GetSize(); i < size; ++i) {
        free(RegionPtrs.GetRef(i));
    }

    RegionPtrs.Clear();

    NextWriteRegionIndex = 0;
    NextReadRegionIndex = 0;
    NextReadDetailIndex = 0;

    WriteStats = &DevNull;
}

void DetailStatsMemory::NextWrite()
{
    if (NextWriteRegionIndex >= RegionPtrs.GetSize())
    {
        DetailStatsRegion* region = (DetailStatsRegion*)calloc(1, sizeof(DetailStatsRegion));

        if (!region || !RegionPtrs.Append(region))
        {
            TONK_DEBUG_BREAK(); // OOM
            Clear();
            return;
        }
    }

    TONK_DEBUG_ASSERT(NextWriteRegionIndex < RegionPtrs.GetSize());
    DetailStatsRegion* region = RegionPtrs.GetRef(NextWriteRegionIndex);
    TONK_DEBUG_ASSERT(region->WrittenCount < kDeetsPerRegion);
    WriteStats = region->Deets + region->WrittenCount;

    // Advance write index
    if (++region->WrittenCount >= kDeetsPerRegion) {
        ++NextWriteRegionIndex;
    }
}

unsigned DetailStatsMemory::GetMemoryUsed() const
{
    return (unsigned)(RegionPtrs.GetSize() * sizeof(DetailStatsRegion));
}

static const unsigned kBlobBytes = 4000;

// Enough margin for "]\0" and to represent 10 bytes of integer string
static const unsigned kGrowThreshBytes = kBlobBytes - 16;

struct JsonBlobStr
{
    JsonBlobStr* Next = nullptr;
    unsigned UsedBytes = 0;
    char Blob[kBlobBytes];
};

struct UIntListStringGenerator
{
    JsonBlobStr* Head = nullptr;
    JsonBlobStr* Tail = nullptr;

    UIntListStringGenerator()
    {
        JsonBlobStr* blob = new (std::nothrow) JsonBlobStr;
        if (!blob) {
            TONK_DEBUG_BREAK(); // OOM
            return;
        }

        Tail = Head = blob;
    }
    ~UIntListStringGenerator()
    {
        // Free allocated memory
        for (JsonBlobStr* node = Head, *next; node != nullptr; node = next)
        {
            next = node->Next;
            delete node;
        }
    }

    void Append(uint32_t value)
    {
        if (!Tail) {
            return;
        }
        unsigned used = Tail->UsedBytes;

        if (used >= kGrowThreshBytes)
        {
            JsonBlobStr* node = new (std::nothrow) JsonBlobStr;
            if (!node) {
                TONK_DEBUG_BREAK(); // OOM
                return;
            }

            Tail->Next = node;
            Tail = node;
            used = 0;
        }

        char *blob = Tail->Blob;

        // Comma-separated
        if (Tail != Head || used > 0) {
            blob[used++] = ',';
        }

        const unsigned digits = inplace_itoa_vitaut_1_cat(blob + used, value);
        used += digits;

        Tail->UsedBytes = used;
    }
};

void DetailStatsMemory::WriteJsonToFile()
{
    const uint64_t t0 = siamese::GetTimeUsec();

    std::string sendStatsPath = "send_stats_";
    sendStatsPath += std::to_string(t0);
    sendStatsPath += ".js";

    std::string receiveStatsPath = "receive_stats_";
    receiveStatsPath += std::to_string(t0);
    receiveStatsPath += ".js";

    std::ofstream sendStatsFile(sendStatsPath, std::ofstream::out);
    if (!sendStatsFile) {
        Logger.Error("Unable to open ", sendStatsPath, " for writing detailed send stats json");
        return;
    }

    std::ofstream receiveStatsFile(receiveStatsPath, std::ofstream::out);
    if (!receiveStatsFile) {
        Logger.Error("Unable to open ", receiveStatsPath, " for writing detailed receive stats json");
        return;
    }

    UIntListStringGenerator generators[Stats_Count];
    UIntListStringGenerator timestamps;
    uint64_t lastTimeUsec = 0;
    uint32_t startUsecU32 = 0;

    ResetRead();
    DetailStats* stats;
    while (stats = GetNextRead(), stats != nullptr)
    {
        for (int i = 0; i < Stats_Count; ++i) {
            generators[i].Append(stats->Stats[i]);
        }

        if (lastTimeUsec == 0) {
            timestamps.Append(0);
            startUsecU32 = (uint32_t)stats->TimestampUsec;
        }
        else {
            timestamps.Append((uint32_t)(stats->TimestampUsec - lastTimeUsec));
        }
        lastTimeUsec = stats->TimestampUsec;
    }

    // Write file:
    sendStatsFile << "var SendStats = {" << std::endl;
    sendStatsFile << "\t\"SeriesNames\": [" << std::endl;
    for (int i = 0; i < Stats_SendCount; ++i) {
        sendStatsFile << "\t\t\"" << GetStatsTypesName(i) << "\"";
        if (i < Stats_SendCount - 1) {
            sendStatsFile << ",";
        }
        sendStatsFile << std::endl;
    }
    sendStatsFile << "\t]," << std::endl;
    for (int i = 0; i < Stats_SendCount; ++i) {
        sendStatsFile << "\t\"" << GetStatsTypesName(i) << "\": [ ";
        for (JsonBlobStr* node = generators[i].Head; node != nullptr; node = node->Next) {
            sendStatsFile.write(node->Blob, node->UsedBytes);
        }
        sendStatsFile << "\t]," << std::endl;
    }
    sendStatsFile << "\t\"MicrosecondTimestamps\": [ ";
    for (JsonBlobStr* node = timestamps.Head; node != nullptr; node = node->Next) {
        sendStatsFile.write(node->Blob, node->UsedBytes);
    }
    sendStatsFile << "\t]," << std::endl;
    sendStatsFile << "\t\"StartUsecU32\": " << std::to_string(startUsecU32) << std::endl;
    sendStatsFile << "};" << std::endl;
    sendStatsFile.close();

    receiveStatsFile << "var ReceiveStats = {" << std::endl;
    receiveStatsFile << "\t\"SeriesNames\": [" << std::endl;
    for (int i = Stats_SendCount; i < Stats_Count; ++i) {
        receiveStatsFile << "\t\t\"" << GetStatsTypesName(i) << "\"";
        if (i < Stats_Count - 1) {
            receiveStatsFile << ",";
        }
        receiveStatsFile << std::endl;
    }
    receiveStatsFile << "\t]," << std::endl;
    for (int i = Stats_SendCount; i < Stats_Count; ++i) {
        receiveStatsFile << "\t\"" << GetStatsTypesName(i) << "\": [ ";
        for (JsonBlobStr* node = generators[i].Head; node != nullptr; node = node->Next) {
            receiveStatsFile.write(node->Blob, node->UsedBytes);
        }
        receiveStatsFile << "\t]," << std::endl;
    }
    receiveStatsFile << "\t\"MicrosecondTimestamps\": [ ";
    for (JsonBlobStr* node = timestamps.Head; node != nullptr; node = node->Next) {
        receiveStatsFile.write(node->Blob, node->UsedBytes);
    }
    receiveStatsFile << "\t]," << std::endl;
    receiveStatsFile << "\t\"StartUsecU32\": " << std::to_string(startUsecU32) << std::endl;
    receiveStatsFile << "};" << std::endl;
    receiveStatsFile.close();

    const uint64_t t1 = siamese::GetTimeUsec();
    Logger.Info("Wrote send/receive detail stats json files in ", (t1 - t0), " usec");
}

DetailStats* DetailStatsMemory::GetNextRead()
{
    if (NextReadRegionIndex >= RegionPtrs.GetSize()) {
        return nullptr;
    }
    DetailStatsRegion* region = RegionPtrs.GetRef(NextReadRegionIndex);
    if (NextReadDetailIndex >= region->WrittenCount) {
        return nullptr;
    }

    TONK_DEBUG_ASSERT(NextReadDetailIndex < kDeetsPerRegion);
    DetailStats* stats = region->Deets + NextReadDetailIndex;

    if (++NextReadDetailIndex >= kDeetsPerRegion)
    {
        NextReadDetailIndex = 0;
        ++NextReadRegionIndex;
    }

    return stats;
}

#endif // TONK_DETAILED_STATS


//------------------------------------------------------------------------------
// RefCounter

void RefCounter::AppRelease()
{
    // Begin disconnect if it has not happened yet
    StartShutdown(
        Tonk_AppRequest,
        Result("AppRelease", "Application requested shutdown", ErrorType::Tonk, Tonk_AppRequest));

    AppHoldsReference = false;
}

void RefCounter::StartShutdown(unsigned reason, const Result& result)
{
    std::lock_guard<std::mutex> locker(ShutdownLock);

    if (Shutdown) {
        return;
    }

    ShutdownReason = reason;
    ShutdownJson = result.ToJson();

    Shutdown = true;

    // Remove self reference
    DecrementReferences();
}


//------------------------------------------------------------------------------
// Debugging: Christmas Tree

std::atomic<int> ActiveConnectionCount = ATOMIC_VAR_INIT(0);
std::atomic<int> ActiveUDPSocketCount = ATOMIC_VAR_INIT(0);
std::atomic<int> ActiveSessionSocketCount = ATOMIC_VAR_INIT(0);


//------------------------------------------------------------------------------
// Tonk API Helpers

void MakeCallbacksOptional(TonkConnectionConfig& config)
{
    if (!config.OnConnect)
    {
        config.OnConnect = [](
            TonkAppContextPtr /*context*/,
            TonkConnection /*connection*/)
        {
            // No-op
        };
    }
    if (!config.OnData)
    {
        config.OnData = [](
            TonkAppContextPtr /*context*/,
            TonkConnection /*connection*/,
            unsigned          /*channel*/,
            const uint8_t*       /*data*/,
            unsigned            /*bytes*/)
        {
            // No-op
        };
    }
    if (!config.OnTick)
    {
        config.OnTick = [](
            TonkAppContextPtr /*context*/,
            TonkConnection /*connection*/,
            uint64_t /*nowUsec*/)
        {
            // No-op
        };
    }
    if (!config.OnClose)
    {
        config.OnClose = [](
            TonkAppContextPtr /*context*/,
            TonkConnection /*connection*/,
            TonkResult         /*reason*/,
            const char*    /*reasonJson*/)
        {
            // No-op
        };
    }
}


} // namespace tonk
