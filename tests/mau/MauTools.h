/** \file
    \brief Mau Implementation: Tools
    \copyright Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Mau nor the names of its contributors may be
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

#include "Logger.h"
#include "PacketAllocator.h"

#include "thirdparty/IncludeAsio.h"

#if !defined(_WIN32)
    #include <pthread.h>
#endif // _WIN32

#include <stdint.h>
#include <thread>
#include <functional>
#include <unordered_map>
#include <mutex>


//------------------------------------------------------------------------------
// Portability macros

// Specify an intentionally unused variable (often a function parameter)
#define MAU_UNUSED(x) (void)(x);

// Compiler-specific debug break
#if defined(_DEBUG) || defined(DEBUG)
    #define MAU_DEBUG
    #if defined(_WIN32)
        #define MAU_DEBUG_BREAK() __debugbreak()
    #else // _WIN32
        #define MAU_DEBUG_BREAK() __builtin_trap()
    #endif // _WIN32
    #define MAU_DEBUG_ASSERT(cond) { if (!(cond)) { MAU_DEBUG_BREAK(); } }
    #define MAU_IF_DEBUG(x) x;
#else // _DEBUG
    #define MAU_DEBUG_BREAK() ;
    #define MAU_DEBUG_ASSERT(cond) ;
    #define MAU_IF_DEBUG(x) ;
#endif // _DEBUG

// Compiler-specific force inline keyword
#if defined(_MSC_VER)
    #define MAU_FORCE_INLINE inline __forceinline
#else // _MSC_VER
    #define MAU_FORCE_INLINE inline __attribute__((always_inline))
#endif // _MSC_VER


//------------------------------------------------------------------------------
// Logging

#ifdef MAU_DEBUG
    static const logger::Level MinimumLogLevel = logger::Level::Trace;
#else
    static const logger::Level MinimumLogLevel = logger::Level::Trace;
#endif


namespace mau {


//------------------------------------------------------------------------------
// Convenience Types

/// UDP/IP source or destination network address
typedef asio::ip::udp::endpoint UDPAddress;


//------------------------------------------------------------------------------
// Threads

/// Set the current thread processor affinity
bool SetCurrentThreadAffinity(unsigned processorIndex);

enum class ThreadPriority
{
    High,
    Normal,
    Low,
    Idle
};

/// Set the thread priority of the current thread
bool SetCurrentThreadPriority(ThreadPriority prio);

/// Set the current thread name
void SetCurrentThreadName(const char* name);


//------------------------------------------------------------------------------
// Mutex

static const unsigned kMutexSpinCount = 1000;

#ifdef _WIN32

struct Lock
{
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
// Socket Options

/// Set options for TCP or UDP sockets
template<class SocketT>
MAU_FORCE_INLINE bool SetSocketOptions(
    SocketT& socket,
    unsigned sendBufferSizeBytes,
    unsigned recvBufferSizeBytes)
{
    asio::error_code error;

    socket->set_option(asio::socket_base::send_buffer_size(sendBufferSizeBytes), error);
    if (error) {
        return false;
    }

    socket->set_option(asio::socket_base::receive_buffer_size(recvBufferSizeBytes), error);
    if (error) {
        return false;
    }

    socket->set_option(asio::socket_base::reuse_address(true), error);
    if (error) {
        return false;
    }

    return true;
}


//------------------------------------------------------------------------------
// BufferAllocator

/// Threadsafe version of pktalloc::Allocator
class BufferAllocator
{
public:
    /// Allocate a new buffer
    MAU_FORCE_INLINE uint8_t* Allocate(unsigned bytes)
    {
        Locker locker(AllocatorLock);
        return Allocator.Allocate(bytes);
    }

    /// Shrink the given buffer to size
    MAU_FORCE_INLINE void Shrink(void* buffer, unsigned bytes)
    {
        Locker locker(AllocatorLock);
        return Allocator.Shrink(reinterpret_cast<uint8_t*>(buffer), bytes);
    }

    /// Free the buffer
    MAU_FORCE_INLINE void Free(void* buffer)
    {
        Locker locker(AllocatorLock);
        return Allocator.Free(reinterpret_cast<uint8_t*>(buffer));
    }

    /// Get statistics for the allocator
    MAU_FORCE_INLINE uint64_t GetUsedMemory() const
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
    uint8_t* FreePtr = nullptr;

    /// Allocator to use when freeing
    BufferAllocator* AllocatorForFree = nullptr;

    MAU_FORCE_INLINE void Free() const
    {
        if (AllocatorForFree)
            AllocatorForFree->Free(FreePtr);
    }
};


//------------------------------------------------------------------------------
// Portability Helpers

template<typename T, typename... Args>
MAU_FORCE_INLINE std::unique_ptr<T> MakeUniqueNoThrow(Args&&... args) {
    return std::unique_ptr<T>(new(std::nothrow) T(std::forward<Args>(args)...));
}

template<typename T, typename... Args>
MAU_FORCE_INLINE std::shared_ptr<T> MakeSharedNoThrow(Args&&... args) {
    return std::shared_ptr<T>(new(std::nothrow) T(std::forward<Args>(args)...));
}


//------------------------------------------------------------------------------
// PCG PRNG

/// From http://www.pcg-random.org/
class PCGRandom
{
public:
    void Seed(uint64_t y, uint64_t x = 0)
    {
        State = 0;
        Inc = (y << 1u) | 1u;
        Next();
        State += x;
        Next();
    }

    uint32_t Next()
    {
        const uint64_t oldstate = State;
        State = oldstate * UINT64_C(6364136223846793005) + Inc;
        const uint32_t xorshifted = (uint32_t)(((oldstate >> 18) ^ oldstate) >> 27);
        const uint32_t rot = oldstate >> 59;
        return (xorshifted >> rot) | (xorshifted << ((uint32_t)(-(int32_t)rot) & 31));
    }

    uint64_t State = 0, Inc = 0;
};

uint64_t GetRandomSeed();


//------------------------------------------------------------------------------
// Timing

/// Platform independent high-resolution timers
uint64_t GetTimeUsec();
uint64_t GetTimeMsec();


} // namespace mau
