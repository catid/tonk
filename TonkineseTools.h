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

#pragma once


//------------------------------------------------------------------------------
// Compile Switches

// Enable detailed stats collection?
//#define TONK_DETAILED_STATS

// Enable debug assertions in release mode?
//#define TONK_DEBUG_IN_RELEASE

// Enable verbose logging of outgoing data?
//#define TONK_ENABLE_VERBOSE_OUTGOING

// Enable verbose logging of incoming data?
//#define TONK_ENABLE_VERBOSE_INCOMING

// Enable verbose logging for bandwidth control?
//#define TONK_ENABLE_VERBOSE_BANDWIDTH


//------------------------------------------------------------------------------
// Common Includes

#include "Logger.h"
#include "Counter.h"
#include "PacketAllocator.h"

#include "thirdparty/IncludeAsio.h" // Asio

#include "SiameseTools.h"
#include "SiameseSerializers.h"

#include "tonk.h"

#include "cymric.h"

#if !defined(_WIN32)
    #include <pthread.h>
#endif // _WIN32

#include <stdint.h>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>


//------------------------------------------------------------------------------
// Portability macros

// Specify an intentionally unused variable (often a function parameter)
#define TONK_UNUSED(x) (void)(x);

// Compiler-specific debug break
#if defined(_DEBUG) || defined(DEBUG) || defined(TONK_DEBUG_IN_RELEASE)
    #define TONK_DEBUG
    #if defined(_WIN32)
        #define TONK_DEBUG_BREAK() __debugbreak()
    #else // _WIN32
        #define TONK_DEBUG_BREAK() __builtin_trap()
    #endif // _WIN32
    #define TONK_DEBUG_ASSERT(cond) { if (!(cond)) { TONK_DEBUG_BREAK(); } }
    #define TONK_IF_DEBUG(x) x;
#else // _DEBUG
    #define TONK_DEBUG_BREAK() ;
    #define TONK_DEBUG_ASSERT(cond) ;
    #define TONK_IF_DEBUG(x) ;
#endif // _DEBUG

// Compiler-specific force inline keyword
#if defined(_MSC_VER)
    #define TONK_FORCE_INLINE inline __forceinline
#else // _MSC_VER
    #define TONK_FORCE_INLINE inline __attribute__((always_inline))
#endif // _MSC_VER

// Compiler-specific align keyword
#if defined(_MSC_VER)
    #define TONK_ALIGNED(x) __declspec(align(x))
#else // _MSC_VER
    #define TONK_ALIGNED(x) __attribute__ ((aligned(x)))
#endif // _MSC_VER


namespace tonk {


//------------------------------------------------------------------------------
// Logging

#ifdef TONK_DEBUG
    static const logger::Level MinimumLogLevel = logger::Level::Debug;
#else
    static const logger::Level MinimumLogLevel = logger::Level::Silent;
#endif


//------------------------------------------------------------------------------
// Convenience Types

/// UDP/IP source or destination network address
typedef asio::ip::udp::endpoint UDPAddress;

/// 64-bit nonce type
typedef uint64_t NonceT;

/// Windowed unsigned maximum/minimum
typedef siamese::WindowedMinMax< unsigned, siamese::WindowedMaxCompare<unsigned> > WinMaxUnsigned;
typedef siamese::WindowedMinMax< unsigned, siamese::WindowedMinCompare<unsigned> > WinMinUnsigned;

/// Use CounterSeqNo to represent reliable sequence numbers
typedef Counter<uint32_t, SIAMESE_PACKET_NUM_BITS> CounterSeqNo;


//------------------------------------------------------------------------------
// Threads

/// Set the current thread name
void SetCurrentThreadName(const char* name);


//------------------------------------------------------------------------------
// String Tools

/// Convert value to hex string
std::string HexString(uint64_t value);

/// Escapes strings for insertion into a JSON field
std::string EscapeJsonString(const std::string& s);

/// Escapes values for insertion into a JSON field
template<typename T>
inline std::string JsonValue(const T value) {
    static_assert(std::is_arithmetic<T>::value, "Must be an arithmetic type or a string");
    return std::to_string(value);
}
template<> inline std::string JsonValue(const std::string value) {
    return "\"" + EscapeJsonString(value) + "\"";
}
template<> inline std::string JsonValue(const char* value) {
    if (value == nullptr)
        return "null";
    return JsonValue(static_cast<std::string>(value));
}
template<> inline std::string JsonValue(const char value) {
    return std::string("\"") + value + "\"";
}
template<> inline std::string JsonValue(const bool value) {
    return value ? "true" : "false";
}

TONK_FORCE_INLINE void SafeCopyCStr(char* dest, size_t destBytes, const char* src)
{
#if defined(_MSC_VER)
    strncpy_s(dest, destBytes, src, _TRUNCATE);
#else // _MSC_VER
    strncpy(dest, src, destBytes);
#endif // _MSC_VER
    // No null-character is implicitly appended at the end of destination
    dest[destBytes - 1] = '\0';
}

/// Fill in the TonkAddress structure from a UDPAddress
void SetTonkAddressStr(TonkAddress& addrOut, const UDPAddress& addrIn);

/// Case-insensitive string comparison
TONK_FORCE_INLINE int StrCaseCompare(const char* a, const char* b)
{
#if defined(_WIN32)
#if defined(_MSC_VER) && (_MSC_VER >= 1400)
    return ::_stricmp(a, b);
#else
    return ::stricmp(a, b);
#endif

#else
    return strcasecmp(a, b);
#endif
}

/// Convert unsigned 32-bit integer to decimal string.
/// Optimized for numbers that count up from 0 and are often close to 0.
/// Does not write a null terminator '\0'.
/// Returns the number of characters (digits) written
unsigned inplace_itoa_vitaut_1_cat(char* buf, uint32_t val);


//------------------------------------------------------------------------------
// Timer

/**
    Implements a polled timer that expires after a given number of microseconds.
    Supports timers that tick repeatedly and timers that tick once and then stop.

    Fixes a common bug in timer code: If the OS hibernates and resumes, then all
    timeouts pop unexpectedly because the timestamps have jumped ahead.  This is
    fixed by requiring that the timer expires 2 times before firing, which
    guarantees that time is moving forward.
*/
class Timer
{
public:
    /// Behavior for the timer on expiration: Set by SetTimeoutUsec()
    enum class Behavior
    {
        Restart, ///< Restart timer on expiration
        Stop     ///< Do not reset timer on expiration (require manual Reset)
    };

    /// Set the timeout in microseconds and reset behavior
    void SetTimeoutUsec(uint64_t timeoutUsec, Behavior resetBehavior = Behavior::Stop);

    /// Start the timer ticking
    void Start(uint64_t nowUsec);

    /// Is the timer expired?
    bool IsExpired(uint64_t nowUsec);

    /// Force timer to expire next time IsExpired() is called
    void ForceExpire();

    /// Stop the timer from ticking
    void Stop();

protected:
    /**
        Number of timer divisions until timer expires.

        This number must be kept small, because it is the minimum number of
        calls to IsExpired() that must be made in order for it to return true.

        The main cause of timers lurching forward (from the timer's perspective)
        is the computer going to sleep.  So the minimum number of divisions
        might be set to 2.  I didn't want to subdivide any further because it
        imposes a minimum number of ticks before a timeout.
    */
    static const unsigned kTickDivisions = 2; // Power-of-two preferred

    /// Microsecond timestamp when the timer started to wait
    uint64_t WaitStartUsec = 0;

    /// Number of microseconds per division
    uint64_t UsecPerDivision = 0;

    /// Counter of divisions that have expired so far
    unsigned DivisionsExpired = 0;

    /// Behavior when timer resets
    Behavior ResetBehavior = Behavior::Stop;

    /// Is the timer currently ticking?
    bool IsTicking = false;
};


//------------------------------------------------------------------------------
// Result

/// Type of error code
enum class ErrorType
{
    Tonk,
    Siamese,
    Asio,
    Cymric,
    LastErr,
    Encrypt,
    Zstd,

    Count // For asserts
};

/// Convert ErrorType to string for printing
const char* ErrorTypeToString(ErrorType errorType);

/// Error code type
typedef int64_t ErrorCodeT;

/// Error result structure allocated when errors occur
struct ErrorResult
{
    const char* Source = nullptr;
    std::string Description;
    ErrorType Type = ErrorType::Tonk;
    ErrorCodeT Code = Tonk_Error;


    std::string ToJson() const;


    explicit ErrorResult(const char* source,
                         const std::string& desc,
                         ErrorType type,
                         ErrorCodeT code)
        : Source(source)
        , Description(desc)
        , Type(type)
        , Code(code)
    {
    }

    explicit ErrorResult(const std::string& desc)
        : Description(desc)
    {
    }
};

/// Allows passing failure codes up the stack to the API
struct Result
{
    /// Error object pointer.  nullptr: Operation successful
    mutable ErrorResult* Error;


    TONK_FORCE_INLINE bool IsGood() const
    {
        return nullptr == Error;
    }
    TONK_FORCE_INLINE bool IsFail() const
    {
        return nullptr != Error;
    }

    static inline Result Success()
    {
        Result success;
        return success;
    }
    static inline Result OutOfMemory()
    {
        std::string empty;
        return Result("Out of memory", empty, ErrorType::Tonk, Tonk_OOM);
    }

    std::string ToJson() const;

    TONK_FORCE_INLINE ErrorCodeT GetErrorCode() const
    {
        if (IsGood()) {
            return Tonk_Success;
        }
        return Error->Code;
    }

    ~Result()
    {
        delete Error;
    }

    explicit TONK_FORCE_INLINE Result()
        : Error(nullptr)
    {
    }
    explicit TONK_FORCE_INLINE Result(const std::string& desc)
        : Error(new ErrorResult(desc))
    {
    }
    explicit TONK_FORCE_INLINE Result(const char* source,
                                      const std::string& desc,
                                      ErrorType type = ErrorType::Tonk,
                                      ErrorCodeT code = Tonk_Error)
        : Error(new ErrorResult(source, desc, type, code))
    {
    }

    /// Modifies the provided result object to move the error into lval object
    TONK_FORCE_INLINE Result(const Result& result)
        : Error(result.Error)
    {
        result.Error = nullptr;
    }

    /// Modifies the provided result object to move the error into lval object
    TONK_FORCE_INLINE Result& operator=(const Result& result)
    {
        Error = result.Error;
        result.Error = nullptr;
        return *this;
    }
};


//------------------------------------------------------------------------------
// Intrinsics

/// Returns first position with set bit (LSB = 0)
/// Precondition: x != 0
TONK_FORCE_INLINE unsigned NonzeroLowestBitIndex(uint32_t x)
{
#ifdef _MSC_VER
    unsigned long index;
    // Note: Ignoring result because x != 0
    _BitScanReverse(&index, x);
    return (unsigned)index;
#else
    // Note: Ignoring return value of 0 because x != 0
    return 31 - (unsigned)__builtin_clz(x);
#endif
}


//------------------------------------------------------------------------------
// Mutex

static const unsigned kMutexSpinCount = 1000;

#ifdef _WIN32

struct Lock
{
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    CRITICAL_SECTION cs;

    Lock() { ::InitializeCriticalSectionAndSpinCount(&cs, (DWORD)kMutexSpinCount); }
    ~Lock() { ::DeleteCriticalSection(&cs); }
    bool TryEnter() { return ::TryEnterCriticalSection(&cs) != FALSE; }
    void Enter() { ::EnterCriticalSection(&cs); }
    void Leave() { ::LeaveCriticalSection(&cs); }
};

#else

struct Lock
{
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    TONK_FORCE_INLINE Lock() {}

    std::recursive_mutex cs;

    bool TryEnter() { return cs.try_lock(); }
    void Enter() { cs.lock(); }
    void Leave() { cs.unlock(); }
};

#endif

class Locker
{
public:
    Locker(Lock& lock) {
        TheLock = &lock;
        if (TheLock)
            TheLock->Enter();
    }
    ~Locker() { Clear(); }
    bool TrySet(Lock& lock) {
        Clear();
        if (!lock.TryEnter())
            return false;
        TheLock = &lock;
        return true;
    }
    void Set(Lock& lock) {
        Clear();
        lock.Enter();
        TheLock = &lock;
    }
    void Clear() {
        if (TheLock)
            TheLock->Leave();
        TheLock = nullptr;
    }
protected:
    Lock* TheLock;
};


//------------------------------------------------------------------------------
// SecureRandom

/// Initialize the generator
Result SecureRandom_Initialize();

/// Generate next value
Result SecureRandom_Next(uint8_t* key, unsigned bytes);


//------------------------------------------------------------------------------
// Source Address Hash

/// Initialize the address hash function's secret key.
/// Must be called after SecureRandom_Initialize()
Result InitializeAddressHash();

/// Hash IP address + port using fastest approach available.
/// Result is salted by a random secret on each run to make it unpredictable
uint64_t HashUDPAddress(const UDPAddress& addr);

struct UDPAddressHasher
{
    TONK_FORCE_INLINE std::size_t operator()(UDPAddress const& addr) const noexcept
    {
        return (std::size_t)HashUDPAddress(addr);
    }
};

struct UInt32Hasher
{
    TONK_FORCE_INLINE std::size_t operator()(uint32_t const& id) const noexcept
    {
        return id;
    }
};


//------------------------------------------------------------------------------
// BufferAllocator

/// Threadsafe version of pktalloc::Allocator
class BufferAllocator
{
public:
    /// Allocate a new buffer
    TONK_FORCE_INLINE uint8_t* Allocate(unsigned bytes)
    {
        Locker locker(AllocatorLock);
        return Allocator.Allocate(bytes);
    }

    /// Shrink the given buffer to size
    TONK_FORCE_INLINE void Shrink(void* buffer, unsigned bytes)
    {
        Locker locker(AllocatorLock);
        return Allocator.Shrink(reinterpret_cast<uint8_t*>(buffer), bytes);
    }

    /// Free the buffer
    TONK_FORCE_INLINE void Free(void* buffer)
    {
        Locker locker(AllocatorLock);
        return Allocator.Free(reinterpret_cast<uint8_t*>(buffer));
    }

    /// Get statistics for the allocator
    TONK_FORCE_INLINE uint64_t GetUsedMemory() const
    {
        Locker locker(AllocatorLock);
        return Allocator.GetMemoryAllocatedBytes();
    }

protected:
    /// Lock protecting the allocator
    mutable Lock AllocatorLock;

    /// Buffer allocator
    pktalloc::Allocator Allocator;
};


//------------------------------------------------------------------------------
// DatagramBuffer

struct DatagramBuffer
{
    /// Pointer to valid data buffer
    uint8_t* Data = nullptr;

    /// Number of valid data bytes
    unsigned Bytes = 0;

    /// Pointer to pass when freeing
    void* FreePtr = nullptr;

    /// Allocator to use when freeing
    BufferAllocator* AllocatorForFree = nullptr;

    TONK_FORCE_INLINE void Free() const
    {
        if (AllocatorForFree) {
            AllocatorForFree->Free(FreePtr);
        }
    }
};


//------------------------------------------------------------------------------
// Detailed Statistics Memory

#ifdef TONK_DETAILED_STATS

enum StatsTypes
{
    // These are all reset each Connection Tick

    Stats_FramingBytesSent,
    Stats_AcksBytesSent,
    Stats_UnreliableBytesSent,
    Stats_UnorderedBytesSent,
    Stats_ControlBytesSent,
    Stats_ReliableBytesSent,
    Stats_LowPriBytesSent,
    Stats_RetransmitSent,
    Stats_CompressedBytesSent,
    Stats_CompressionSavedBytes,

    Stats_PaddingBytesSent,
    Stats_FootersBytesSent,
    Stats_SiameseBytesSent,
    Stats_DummyBytesSent,

    Stats_BytesReceived,
    Stats_MinUsecOWD,
    Stats_MaxUsecOWD,

    Stats_Count,

    // The first few stats are incremented in the Outgoing queue
    Stats_QueueCount = Stats_PaddingBytesSent,

    // The first set are related to sending data
    Stats_SendCount = Stats_BytesReceived,
};

const char* GetStatsTypesName(unsigned type);

struct DetailStats
{
    /// Statistics counters
    uint32_t Stats[Stats_Count];

    /// Timestamp for this statistics
    uint64_t TimestampUsec;
};

static const unsigned kDeetsPerRegion = 4000 / sizeof(DetailStats);

struct DetailStatsRegion
{
    /// Array of detailed stats
    DetailStats Deets[kDeetsPerRegion];

    /// Number of details elements written so far
    unsigned WrittenCount = 0;
};

/// Zero-copy growing statistics log
class DetailStatsMemory
{
public:
    TONK_FORCE_INLINE DetailStatsMemory()
    {
        WriteStats = &DevNull;
    }
    TONK_FORCE_INLINE ~DetailStatsMemory()
    {
        Clear();
    }

    /// Clear all data and reset read/write pointers
    void Clear();

    /// Get current stats pointer.  Never returns nullptr
    TONK_FORCE_INLINE DetailStats* GetWritePtr() const
    {
        return WriteStats;
    }

    /// Advance to next detail stats to write (will be cleared to zeros)
    void NextWrite();

    /// Start read iterator over from the beginning
    TONK_FORCE_INLINE void ResetRead()
    {
        NextReadRegionIndex = 0;
        NextReadDetailIndex = 0;
    }

    /// Get next detail stats to read.
    /// Returns null if no detail stats remain to be read
    DetailStats* GetNextRead();

    /// Get memory used
    unsigned GetMemoryUsed() const;

    /// Write data in JSON format to a text file.
    /// If no file name is provided, it uses a default name
    void WriteJsonToFile();

protected:
    /// Array of all the allocated regions
    pktalloc::LightVector<DetailStatsRegion*> RegionPtrs;

    /// The next region index for reading
    unsigned NextReadRegionIndex = 0;

    /// The next detail stats index for reading
    unsigned NextReadDetailIndex = 0;

    /// The next region index for writing
    unsigned NextWriteRegionIndex = 0;

    /// Stats writing
    DetailStats* WriteStats = nullptr;

    /// Used if out of memory or disabled
    DetailStats DevNull;
};

#endif // TONK_DETAILED_STATS


//------------------------------------------------------------------------------
// RefCounter

/**
    RefCounter keeps track of the number of references to an object, and whether
    or not that object is attempting to shutdown.  The RefCounter itself holds
    one reference that is released on StartShutdown().
*/
class RefCounter
{
public:
    /// Is the object still in use by Tonk or application?
    /// If not then it can be deleted.
    TONK_FORCE_INLINE bool ObjectInUse() const
    {
        TONK_DEBUG_ASSERT(RefCount >= 0);
        return AppHoldsReference || RefCount != 0;
    }

    //--------------------------------------------------------------------------
    // Application reference

    /// Release application reference
    void AppRelease();

    TONK_FORCE_INLINE bool DoesAppHoldReference() const
    {
        return AppHoldsReference;
    }

    //--------------------------------------------------------------------------
    // Shutdown

    /// Are we shutdown?
    TONK_FORCE_INLINE bool IsShutdown() const
    {
        return Shutdown;
    }
    TONK_FORCE_INLINE bool NotShutdown() const
    {
        return !IsShutdown();
    }

    /// Start shutdown process asynchronously.
    /// This will latch in the first result provided, ignoring the rest.
    /// Latching is synchronized with the ShutdownLock
    void StartShutdown(unsigned reason, const Result& result);

    /// These are not valid to read until Shutdown is true:

    /// Reason for shutdown provided by StartShutdown()
    unsigned ShutdownReason = Tonk_Success;

    /// Jsonified data about the reason for the shutdown
    std::string ShutdownJson;

    //--------------------------------------------------------------------------
    // Internal reference count

    /// Increment or decrement the reference count
    TONK_FORCE_INLINE void IncrementReferences()
    {
        TONK_DEBUG_ASSERT(RefCount >= 0);
        ++RefCount;
    }
    TONK_FORCE_INLINE void DecrementReferences()
    {
        TONK_DEBUG_ASSERT(RefCount > 0);
        --RefCount;
        TONK_DEBUG_ASSERT(RefCount >= 0);
    }

protected:
    /// Internal reference count
    /// Holds one reference count that is released on StartShutdown()
    std::atomic<int32_t> RefCount = ATOMIC_VAR_INIT(1);

    /// IsShutdown() flag
    std::atomic<bool> Shutdown = ATOMIC_VAR_INIT(false);

    /// Does the application hold references on the object?
    std::atomic<bool> AppHoldsReference = ATOMIC_VAR_INIT(true);

    /// Synchronizes threads calling StartShutdown()
    std::mutex ShutdownLock;
};


//------------------------------------------------------------------------------
// Debugging: Christmas Tree

/// This is a set of diagnostic information that can help track down problems.
/// Number of active objects of different sorts
extern std::atomic<int> ActiveConnectionCount;
extern std::atomic<int> ActiveUDPSocketCount;
extern std::atomic<int> ActiveSessionSocketCount;


//------------------------------------------------------------------------------
// Tonk API Helpers

/// Fill in config data to replace null function pointers with lambda stubs
void MakeCallbacksOptional(TonkConnectionConfig& config);


//------------------------------------------------------------------------------
// Portability Helpers

template<typename T, typename... Args>
inline std::unique_ptr<T> MakeUniqueNoThrow(Args&&... args) {
    return std::unique_ptr<T>(new(std::nothrow) T(std::forward<Args>(args)...));
}

template<typename T, typename... Args>
inline std::shared_ptr<T> MakeSharedNoThrow(Args&&... args) {
    return std::shared_ptr<T>(new(std::nothrow) T(std::forward<Args>(args)...));
}


} // namespace tonk

namespace std {


//------------------------------------------------------------------------------
// Android Portability

#if defined(ANDROID) && !defined(DEFINED_TO_STRING)
#define DEFINED_TO_STRING
#include <sstream>
/// Polyfill for to_string() on Android
template<typename T> string to_string(T value)
{
    ostringstream os;
    os << value;
    return os.str();
}
#endif // DEFINED_TO_STRING


} // namespace std
