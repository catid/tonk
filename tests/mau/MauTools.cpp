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

#include "MauTools.h"

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

namespace mau {

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
#ifdef __APPLE__
    pthread_setname_np(threadName);
#else
    pthread_setname_np(pthread_self(), threadName);
#endif
}
#endif

bool SetCurrentThreadPriority(ThreadPriority prio)
{
#ifdef _WIN32
    int winPrio = THREAD_PRIORITY_NORMAL;
    switch (prio)
    {
    case ThreadPriority::High: winPrio = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadPriority::Low:  winPrio = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadPriority::Idle: winPrio = THREAD_PRIORITY_IDLE; break;
    default: break;
    }
    return 0 != ::SetThreadPriority(::GetCurrentThread(), winPrio);
#else
    int niceness = 0;
    switch (prio)
    {
    case ThreadPriority::High: niceness = 2; break;
    case ThreadPriority::Low:  niceness = -2; break;
    case ThreadPriority::Idle: niceness = 19; break;
    default: break;
    }
    return -1 != nice(niceness);
#endif
}

bool SetCurrentThreadAffinity(unsigned processorIndex)
{
#ifdef _WIN32
    return 0 != ::SetThreadAffinityMask(
        ::GetCurrentThread(), (DWORD_PTR)1 << (processorIndex & 63));
#elif !defined(ANDROID) && !defined(__APPLE__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(processorIndex, &cpuset);
    return 0 == pthread_setaffinity_np(pthread_self(),
        sizeof(cpu_set_t), &cpuset);
#else
    return true; // FIXME: Unused on Android anyway
#endif
}


//------------------------------------------------------------------------------
// PCG RNG

std::atomic<uint64_t> m_SeedTweak = ATOMIC_VAR_INIT(1);

uint64_t GetRandomSeed()
{
    const uint64_t t = GetTimeUsec();
    const uint64_t tweak = (m_SeedTweak += t + 1);
    return tweak;
}


//------------------------------------------------------------------------------
// Timing

#ifdef _WIN32
static double PerfFrequencyInverse = 0.;

static void InitPerfFrequencyInverse()
{
    LARGE_INTEGER freq = {};
    if (!::QueryPerformanceFrequency(&freq) || freq.QuadPart == 0)
        return;
    PerfFrequencyInverse = 1000000. / (double)freq.QuadPart;
}
#elif __MACH__
static bool m_clock_serv_init = false;
static clock_serv_t m_clock_serv = 0;

static void InitClockServ()
{
    m_clock_serv_init = true;
    host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &m_clock_serv);
}
#endif // _WIN32

uint64_t GetTimeUsec()
{
#ifdef _WIN32
    LARGE_INTEGER timeStamp = {};
    if (!::QueryPerformanceCounter(&timeStamp))
        return 0;
    if (PerfFrequencyInverse == 0.)
        InitPerfFrequencyInverse();
    return (uint64_t)(PerfFrequencyInverse * timeStamp.QuadPart);
#elif __MACH__
    if (!m_clock_serv_init)
        InitClockServ();

    mach_timespec_t tv;
    clock_get_time(m_clock_serv, &tv);

    return 1000000 * tv.tv_sec + tv.tv_nsec / 1000;
#else
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return 1000000 * tv.tv_sec + tv.tv_usec;
#endif
}

uint64_t GetTimeMsec()
{
    return GetTimeUsec() / 1000;
}


} // namespace mau
