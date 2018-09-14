/** \file
    \brief Cymric RNG Main C API Source
    \copyright Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name of Cymric nor the names of its contributors may be
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

#include "cymric.h"
#include "thirdparty/blake2.h"
#include "thirdparty/chacha.h"


//------------------------------------------------------------------------------
// Default Flags

#define CYMRIC_DEV_RANDOM
#define CYMRIC_CYCLES
//#define CYMRIC_SRANDOM
#define CYMRIC_SRAND
#define CYMRIC_USEC
//#define CYMRIC_GETTID
#define CYMRIC_PTHREADS
#define CYMRIC_GETPID
#define CYMRIC_UNINIT
//#define CYMRIC_WINTID
//#define CYMRIC_WINCRYPT
//#define CYMRIC_WINMEM
//#define CYMRIC_ARC4RANDOM
#define CYMRIC_COUNTER


//------------------------------------------------------------------------------
// Compiler Platform Detection

// Mac OS X additional compilation flags
#if defined(__APPLE__)
# include <TargetConditionals.h>
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
# define CAT_COMPILER_MINGW
#endif

//-----------------------------------------------------------------------------
// Intel C++ Compiler : Interoperates with MSVC and GCC
#if defined(__INTEL_COMPILER) || defined(__ICL) || defined(__ICC) || defined(__ECC)
# define CAT_COMPILER_ICC
# define CAT_FENCE_COMPILER __memory_barrier();
#endif

//-----------------------------------------------------------------------------
// Borland C++ Compiler : Compatible with MSVC syntax
#if defined(__BORLANDC__)
# define CAT_COMPILER_BORLAND
# define CAT_COMPILER_COMPAT_MSVC
# define CAT_INLINE __inline
# define CAT_ASM_EMIT __emit__

//-----------------------------------------------------------------------------
// Digital Mars C++ Compiler (previously known as Symantec C++)
#elif defined(__DMC__) || defined(__SC__) || defined(__SYMANTECC__)
# define CAT_COMPILER_DMARS
# define CAT_COMPILER_COMPAT_MSVC
# define CAT_INLINE __inline
# define CAT_ASM_EMIT __emit__

//-----------------------------------------------------------------------------
// Codeplay VectorC C++ Compiler : Compatible with GCC and MSVC syntax, prefer GCC
#elif defined(__VECTORC__)
# define CAT_COMPILER_CODEPLAY
# define CAT_COMPILER_COMPAT_GCC

//-----------------------------------------------------------------------------
// Pathscale C++ Compiler : Compatible with GCC syntax
#elif defined(__PATHSCALE__)
# define CAT_COMPILER_PATHSCALE
# define CAT_COMPILER_COMPAT_GCC

//-----------------------------------------------------------------------------
// Watcom C++ Compiler : Compatible with GCC and MSVC syntax, prefer GCC
#elif defined(__WATCOMC__)
# define CAT_COMPILER_WATCOM
# define CAT_COMPILER_COMPAT_GCC

//-----------------------------------------------------------------------------
// SUN C++ Compiler : Compatible with GCC syntax
#elif defined(__SUNPRO_CC)
# define CAT_COMPILER_SUN
# define CAT_COMPILER_COMPAT_GCC

//-----------------------------------------------------------------------------
// Metrowerks C++ Compiler : Compatible with MSVC syntax
#elif defined(__MWERKS__)
# define CAT_COMPILER_MWERKS
# define CAT_COMPILER_COMPAT_MSVC
# define CAT_INLINE inline
# define CAT_ASM_BEGIN _asm {
# define CAT_ASM_BEGIN_VOLATILE _asm {
# define CAT_ASM_EMIT __emit__

//-----------------------------------------------------------------------------
// GNU C++ Compiler
// SN Systems ProDG C++ Compiler : Compatible with GCC
#elif defined(__GNUC__) || defined(__APPLE_CC__) || defined(__SNC__) || defined(__clang__)
# define CAT_COMPILER_GCC
# define CAT_COMPILER_COMPAT_GCC
# define CAT_FASTCALL __attribute__ ((fastcall))

//-----------------------------------------------------------------------------
// Microsoft Visual Studio C++ Compiler
#elif defined(_MSC_VER)
# define CAT_COMPILER_MSVC
# define CAT_COMPILER_COMPAT_MSVC
# define CAT_FASTCALL __fastcall

# if _MSC_VER >= 1300
#  define CAT_FUNCTION __FUNCTION__
# endif

// Intrinsics:
# include <cstdlib>
# include <intrin.h>
# include <mmintrin.h>

//-----------------------------------------------------------------------------
// Otherwise unknown compiler
#else
# define CAT_COMPILER_UNKNOWN
# define CAT_ALIGNED(n) /* no way to detect alignment syntax */
# define CAT_PACKED /* no way to detect packing syntax */
# define CAT_INLINE inline
// No way to support inline assembly code here
# define CAT_RESTRICT

#endif


//------------------------------------------------------------------------------
// OS Platform Detection

#if defined(__SVR4) && defined(__sun)
# define CAT_OS_SOLARIS

#elif defined(__APPLE__) && (defined(__MACH__) || defined(__DARWIN__))
# define CAT_OS_OSX
# define CAT_OS_APPLE

#elif defined(__APPLE__) && defined(TARGET_OS_IPHONE)
# define CAT_OS_IPHONE
# define CAT_OS_APPLE

#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__FreeBSD__)
# define CAT_OS_BSD

#elif defined(__linux__) || defined(__unix__) || defined(__unix)
# define CAT_OS_LINUX

#ifdef __ANDROID__
# define CAT_OS_ANDROID
#endif

#elif defined(_WIN32_WCE)
# define CAT_OS_WINDOWS_CE
# define CAT_OS_WINDOWS /* Also defined */

#elif defined(_WIN32)
# define CAT_OS_WINDOWS

#elif defined(_XBOX) || defined(_X360)
# define CAT_OS_XBOX

#elif defined(_PS3) || defined(__PS3__) || defined(SN_TARGET_PS3)
# define CAT_OS_PS3

#elif defined(_PS4) || defined(__PS4__) || defined(SN_TARGET_PS4)
# define CAT_OS_PS4

#elif defined(__OS2__)
# define CAT_OS_OS2

#elif defined(__APPLE__)
# define CAT_OS_APPLE

#elif defined(_aix) || defined(aix)
# define CAT_OS_AIX

#elif defined(HPUX)
# define CAT_OS_HPUX

#elif defined(IRIX)
# define CAT_OS_IRIX

#else
# define CAT_OS_UNKNOWN
#endif

#if defined(CAT_OS_WINDOWS)
# undef CYMRIC_DEV_RANDOM
# undef CYMRIC_PTHREADS
# undef CYMRIC_GETPID
# define CYMRIC_WINTID
# define CYMRIC_WINCRYPT
# define CYMRIC_WINMEM

#elif defined(CAT_OS_APPLE)

#elif defined(CAT_OS_SOLARIS)

#elif defined(CAT_OS_OS2)

#elif defined(CAT_OS_BSD)
# if defined __OpenBSD__
#  define CYMRIC_ARC4RANDOM
# endif

#elif defined(CAT_OS_LINUX)
# undef CYMRIC_PTHREADS
# define CYMRIC_GETTID

#elif defined(CAT_OS_XBOX)

#elif defined(CAT_OS_PS3)

#elif defined(CAT_OS_PS4)

#elif defined(CAT_OS_AIX)

#elif defined(CAT_OS_HPUX)

#elif defined(CAT_OS_IRIX)

#else

#endif


//------------------------------------------------------------------------------
// Entropy Source Imports

#if defined(CAT_OS_WINDOWS)

#define WIN32_LEAN_AND_MEAN
# include <Windows.h>

# if defined(CAT_COMPILER_MSVC)
#  pragma warning(push)
#  pragma warning(disable: 4201) // Squelch annoying warning from MSVC2005 SDK
#  include <mmsystem.h>
#  pragma warning(pop)
#  pragma comment (lib, "winmm")
# else
#  include <mmsystem.h>
# endif

#else // Linux/other version

# include <sys/time.h>

#endif

#ifdef CAT_OS_OSX
# include <mach/mach_time.h>
#endif

#include <ctime>
using namespace std;

#include <stdlib.h>

#if defined(CYMRIC_CYCLES)

// Algorithm from Skein test app
static uint32_t GetCPUCycles()
{
    uint32_t x[2];

#if defined(CAT_COMPILER_MSVC)
    x[0] = (uint32_t)__rdtsc();

#elif defined(CAT_ASM_INTEL) && defined(CAT_ISA_X86)
    CAT_ASM_BEGIN_VOLATILE
        push eax
        push edx
        CAT_ASM_EMIT 0x0F
        CAT_ASM_EMIT 0x31
        mov x[0], eax
        pop edx
        pop eax
    CAT_ASM_END

#elif defined(CAT_ASM_ATT) && defined(CAT_ISA_X86)
    if (sync) {
        CAT_ASM_BEGIN_VOLATILE
            "cpuid\n\t"
            "rdtsc" : "=a"(x[0]), "=d"(x[1]) : : "ebx", "ecx"
        CAT_ASM_END
    } else {
        CAT_ASM_BEGIN_VOLATILE
            "rdtsc" : "=a"(x[0]), "=d"(x[1]) : : "ebx", "ecx"
        CAT_ASM_END
    }

#elif defined(CAT_ASM_ATT) && defined(CAT_ISA_PPC)
    // Based on code from Kazutomo Yoshii ( http://www.mcs.anl.gov/~kazutomo/rdtsc.html )
    uint32_t tmp;

    CAT_ASM_BEGIN_VOLATILE
        "0:                  \n"
        "\tmftbu   %0        \n"
        "\tmftb    %1        \n"
        "\tmftbu   %2        \n"
        "\tcmpw    %2,%0     \n"
        "\tbne     0b        \n"
        : "=r"(x[1]),"=r"(x[0]),"=r"(tmp)
    CAT_ASM_END

#elif defined(CAT_ASM_ATT) && defined(CAT_ISA_ARMV6)
    // This code is from http://google-perftools.googlecode.com/svn/trunk/src/base/cycleclock.h
    uint32_t pmccntr;
    uint32_t pmuseren;
    uint32_t pmcntenset;

    // Read the user mode perf monitor counter access permissions.
    CAT_ASM_BEGIN_VOLATILE
        "mrc p15, 0, %0, c9, c14, 0"
        : "=r" (pmuseren)
    CAT_ASM_END

    if (pmuseren & 1) {  // Allows reading perfmon counters for user mode code.
        CAT_ASM_BEGIN_VOLATILE
            "mrc p15, 0, %0, c9, c12, 1"
            : "=r" (pmcntenset)
        CAT_ASM_END

        if (pmcntenset & 0x80000000ul) {  // Is it counting?
            CAT_ASM_BEGIN_VOLATILE
                "mrc p15, 0, %0, c9, c13, 0"
                : "=r" (pmccntr)
            CAT_ASM_END

            // The counter is set up to count every 64th cycle
            return pmccntr << 6;
        }
    }

#if defined(CAT_OS_ANDROID)

    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (uint32_t)now.tv_nsec;

#else

    // Fall back to gettimeofday
    struct timeval cateq_v;
    struct timezone cateq_z;
    gettimeofday(&cateq_v, &cateq_z);
    x[0] = (uint32_t)cateq_v.tv_usec;

#endif

#else

# if defined(CAT_OS_WINDOWS)
    LARGE_INTEGER tim;
    QueryPerformanceCounter(&tim);
    x[0] = tim.LowPart;
# elif defined(CAT_OS_OSX)
    return (uint32_t)mach_absolute_time();
# else
    struct timeval cateq_v;
    struct timezone cateq_z;
    gettimeofday(&cateq_v, &cateq_z);
    x[0] = (uint32_t)cateq_v.tv_usec;
# endif

#endif

    return x[0];
}

#endif


uint64_t GetMicrosecondTimeCounter()
{
#if defined(CAT_OS_WINDOWS)
    LARGE_INTEGER tim; // 64-bit! ieee
    QueryPerformanceCounter(&tim);
    return tim.QuadPart;
#elif defined(CAT_OS_ANDROID)
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec);
#else
    struct timeval cateq_v;
    struct timezone cateq_z;
    gettimeofday(&cateq_v, &cateq_z);
    return (uint64_t)cateq_v.tv_sec * 1000000ULL + cateq_v.tv_usec;
#endif
}


#if defined(CYMRIC_DEV_RANDOM)
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __OpenBSD__
// On OpenBSD, use /dev/srandom and also /dev/arandom through arc4random API
# define CYMRIC_RAND_FILE "/dev/srandom"
#else
# define CYMRIC_RAND_FILE "/dev/random"
#endif
# define CYMRIC_URAND_FILE "/dev/urandom"
#endif // CYMRIC_DEV_RANDOM

#if defined(CYMRIC_GETTID)

#ifdef CAT_OS_ANDROID
#include <unistd.h>
static uint32_t unix_gettid() {
    return (uint32_t)gettid();
}
#define CYMRIC_HAS_GETTID

#elif defined(CAT_OS_LINUX)

#include <sys/types.h>
#include <linux/unistd.h>

#ifdef __NR_gettid
static uint32_t unix_gettid() {
    return (uint32_t)syscall(__NR_gettid);
}
#define CYMRIC_HAS_GETTID

#endif // __NR_gettid

#endif // CAT_OS_LINUX

// If was not able to use gettid(),
#ifndef CYMRIC_HAS_GETTID
// Use pthreads instead
# undef CYMRIC_GETTID
# define CYMRIC_PTHREADS
#endif

#endif

#ifdef CYMRIC_WINCRYPT
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#endif // CYMRIC_WINCRYPT

#ifdef CYMRIC_PTHREADS
#include <pthread.h>
#endif

#if defined(CYMRIC_SRAND) || defined(CYMRIC_SRANDOM)
#include <time.h>
#endif

#ifdef CYMRIC_COUNTER
#include <atomic>
#endif


//------------------------------------------------------------------------------
// Helpers

#ifdef CYMRIC_DEV_RANDOM

static int read_file(const char *path, uint8_t *buffer, int bytes) {
    int completed = 0, retries = 100;

    do {
        int len = bytes - completed;

        // Attempt to open the file
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            // Set non-blocking mode
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);

            // Launch read request and close file
            len = read(fd, buffer, len);
            close(fd);
        }

        // If read request failed (ie. blocking):
        if (len <= 0) {
            if (--retries >= 0) {
                continue;
            } else {
                // Give up
                break;
            }
        }

        // Subtract off the read byte count from request size
        completed += len;
        buffer += len;
    } while (completed < bytes);

    // Return number of bytes completed
    return completed;
}

#endif // CYMRIC_DEV_RANDOM

#ifdef CYMRIC_COUNTER

#ifdef CYMRIC_UNINIT
static std::atomic<uint32_t> m_counter;
#else
static std::atomic<uint32_t> m_counter = ATOMIC_VAR_INIT(0);
#endif

static uint32_t get_counter() {
    // Note this may not be thread-safe if the platform is unsupported
    return ++m_counter;
}

#endif // CYMRIC_COUNTER


static volatile bool m_is_initialized = false;

// Decide how many bytes to keep for scratch space
#ifdef CYMRIC_WINMEM
static const int SEED_SCRATCH_BYTES = 64;
#else
static const int SEED_SCRATCH_BYTES = 32;
#endif


// Prevent compiler from optimizing out memset()
static inline void secure_zero_memory(void *v, size_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)v;

    for (size_t i = 0; i < n; ++i)
        p[i] = 0;
}

#define CAT_SECURE_OBJCLR(obj) secure_zero_memory((void*)&(obj), (int)sizeof(obj))

#define CAT_ROL32(n, r) ( (uint32_t)((uint32_t)(n) << (r)) | (uint32_t)((uint32_t)(n) >> (32 - (r))) ) /* only works for uint32_t */


//------------------------------------------------------------------------------
// API

#ifdef __cplusplus
extern "C" {
#endif

int cymric_init_(int expected_version) {
    if (CYMRIC_VERSION != expected_version)
        return -1;

#ifdef CYMRIC_WINMEM
    static_assert(sizeof(MEMORYSTATUS) <= SEED_SCRATCH_BYTES, "Fix this");
#endif

    m_is_initialized = true;
    return 0;
}


//------------------------------------------------------------------------------
// Generator Seed API

int cymric_seed(
    cymric_rng* R,      ///< [out] Generator to seed
    const void* seed,   ///< [in] Seed bytes to use
    int bytes           ///< [in] Number of seed bytes
) {
    // Self-test, verified to fail if set too high
    static const int MIN_ENTROPY = 425;
    int entropy = 0;

    // Align scratch space to 16 bytes
    static const unsigned kAlignmentBytes = 16;

    // Scratch space for loading entropy source bits
    uint8_t scratch_buff[kAlignmentBytes + SEED_SCRATCH_BYTES];

    // Align scratch to avoid alignment issues on mobile
    uint8_t* scratch = scratch_buff;
    scratch += kAlignmentBytes - ((uintptr_t)scratch % kAlignmentBytes);

    // Initialize BLAKE2 state for 512-bit output
    blake2b_state state;

    // If input is invalid:
    if (!R)
        return -1;

    // If not initialized yet:
    if (!m_is_initialized)
        return -2;

    // Mix in previous or uninitialized state
    if (blake2b_init_key(&state, 64, R->internal, 64))
        return -3;

    // Mix in the seed
    if (seed && bytes > 0) {
        if (blake2b_update(&state, (const uint8_t *)seed, bytes))
            return -4;
    }

#ifdef CYMRIC_CYCLES
    {
        // Mix in initial cycle count
        uint32_t *t0 = (uint32_t *)scratch;
        *t0 = GetCPUCycles();
        if (blake2b_update(&state, scratch, 4))
            return -5;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_DEV_RANDOM
    {
        // Mix in /dev/random.
        // This is a bit of a hack.  Most Linuxy systems "cache" about 20 bytes
        // for /dev/random.  Requesting 32 bytes may take up to a minute longer
        // and this would be really irritating for users of the library.  So
        // instead we grab just 20 bytes and then take the rest from urandom,
        // which is often implemented as a separate pool that will actually
        // provide additional entropy over /dev/random even when that device
        // is blocking waiting for more
        int devrand_bytes = read_file(CYMRIC_RAND_FILE, scratch, 20);

        // Fill in the rest with /dev/urandom
        int min_bytes = 32 - devrand_bytes;
        if (read_file(CYMRIC_URAND_FILE, scratch + devrand_bytes, min_bytes) < min_bytes)
            return -6;

        if (blake2b_update(&state, scratch, 32))
            return -7;
        entropy += 256;
    }
#endif

#ifdef CYMRIC_WINCRYPT
    {
        // Mix in Windows cryptographic RNG
        HCRYPTPROV hCryptProv;
        if (!CryptAcquireContext(&hCryptProv, 0, 0, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT|CRYPT_SILENT))
            return -8;
        if (hCryptProv && !CryptGenRandom(hCryptProv, 32, scratch))
            return -9;
        if (hCryptProv && !CryptReleaseContext(hCryptProv, 0))
            return -10;
        if (blake2b_update(&state, scratch, 32))
            return -11;
        entropy += 256;
    }
#endif

#ifdef CYMRIC_ARC4RANDOM
    {
        // Mix in arc4random
        arc4random_buf(scratch, 32);
        if (blake2b_update(&state, scratch, 32))
            return -12;
        entropy += 256;
    }
#endif
    
#ifdef CYMRIC_WINMEM
    {
        // Mix in Windows memory info
        MEMORYSTATUS *mem_stats = (MEMORYSTATUS *)scratch;
        GlobalMemoryStatus(mem_stats);
        if (blake2b_update(&state, scratch, sizeof(MEMORYSTATUS)))
            return -13;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_GETPID
    {
        // Mix in process id
        uint32_t *pid = (uint32_t *)scratch;
        *pid = (uint32_t)getpid();
        if (blake2b_update(&state, scratch, 4))
            return -14;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_GETTID
    {
        // Mix in thread id
        uint32_t *tid = (uint32_t *)scratch;
        *tid = unix_gettid();
        if (blake2b_update(&state, scratch, 4))
            return -15;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_PTHREADS
    {
        // Mix in pthread self id
        pthread_t *myself = (pthread_t *)scratch;
        *myself = pthread_self();
        if (blake2b_update(&state, scratch, sizeof(pthread_t)))
            return -16;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_WINTID
    {
        // Mix in thread id
        uint32_t *wintid = (uint32_t *)scratch;
        *wintid = GetCurrentThreadId();
        if (blake2b_update(&state, scratch, 4))
            return -17;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_USEC
    {
        // Mix in microsecond clock
        uint64_t *s0 = (uint64_t *)scratch;
        *s0 = GetMicrosecondTimeCounter();
        if (blake2b_update(&state, scratch, sizeof(double)))
            return -18;
        entropy += 32;
    }
#endif

#ifdef CYMRIC_SRANDOM
    {
        // Mix in srandom
        srandom(random() ^ time(0));
        uint32_t *sr = (uint32_t *)scratch;
        sr[0] = random();
        sr[1] = random();
        if (black2b_update(&state, scratch, 8))
            return -19;
        entropy += 40;
    }
#endif

#ifdef CYMRIC_SRAND
    {
        // Mix in srand
        srand(rand() ^ (unsigned int)time(0));
        uint32_t *sr = (uint32_t *)scratch;
        sr[0] = rand();
        sr[1] = rand();
        if (blake2b_update(&state, scratch, 8))
            return -20;
        entropy += 40;
    }
#endif

#ifdef CYMRIC_CYCLES
    {
        // Mix in final cycle count
        uint32_t *t1 = (uint32_t *)scratch;
        *t1 = GetCPUCycles();
        if (blake2b_update(&state, scratch, 4))
            return -21;
        entropy += 9;
    }
#endif

#ifdef CYMRIC_COUNTER
    {
        // Mix in incrementing counter
        uint32_t *c0 = (uint32_t *)scratch;
        *c0 = get_counter();
        if (blake2b_update(&state, scratch, 4))
            return -22;
        entropy += 1;
    }
#endif

    // Squeeze out 512 random bits from entropy sources
    if (blake2b_final(&state, (uint8_t *)R->internal, 64))
        return -23;

    // Erase BLAKE2 state and scratch
    CAT_SECURE_OBJCLR(state.buf);
    CAT_SECURE_OBJCLR(scratch);

    // Sanity check for compilation
    if (entropy < MIN_ENTROPY)
        return -24;

    // Indicate state is seeded
    R->internal[64] = 'S';
    R->internal[65] = 'E';
    R->internal[66] = 'E';
    R->internal[67] = 'D';

    return 0;
}


//------------------------------------------------------------------------------
// Random Number Generation API

int cymric_random(
    cymric_rng* R,  ///< [in] Generator to use
    void* buffer,   ///< [in, out] Buffer to fill with random bytes
    int bytes       ///< [in] Number of bytes to fill
) {
    static const int CHACHA_ROUNDS = 12;

    // Validate input
    if (!R || !buffer || bytes <= 0)
        return -1;

    // If R was not seeded:
    if (R->internal[64] != 'S' || R->internal[65] != 'E' ||
        R->internal[66] != 'E' || R->internal[67] != 'D') {
        return -2;
    }

    // Use low 32 bytes as a key for ChaCha20
    chacha_key *key = (chacha_key *)R->internal;

    // Use next 8 bytes as an IV for ChaCha20, reserving the top 24 bytes
    chacha_iv *iv = (chacha_iv *)(R->internal + 32);

    // Mix in some fast entropy sources
    uint32_t *iv_mix = (uint32_t *)iv;

#ifdef CYMRIC_CYCLES
    {
        // Mix in initial cycle count
        iv_mix[0] ^= GetCPUCycles();
    }
#endif

#ifndef CYMRIC_UNINIT
    CAT_MEMCLR(buffer, bytes);
#endif

#ifdef CYMRIC_GETTID
    {
        // Mix in thread id
        iv_mix[1] ^= unix_gettid();
    }
#endif

#ifdef CYMRIC_WINTID
    {
        // Mix in thread id
        iv_mix[1] ^= GetCurrentThreadId();
    }
#endif

#ifdef CYMRIC_PTHREADS
    {
        // Mix in pthread self id
        pthread_t myself = pthread_self();
        const uint32_t *words = (const uint32_t *)&myself;
        iv_mix[1] ^= words[0];
        if (sizeof(myself) > 4) {
            iv_mix[0] += words[1];
        }
    }
#endif

#ifdef CYMRIC_COUNTER
    {
        // Mix in incrementing counter
        const uint32_t c0 = get_counter();
        iv_mix[0] += CAT_ROL32(c0, 13);
    }
#endif

    // Note this is "encrypting" the uninitialized buffer with random
    // bytes from the ChaCha20 function.  Valgrind is not going to like
    // this but it is intentional
    chacha(key, iv, (const uint8_t *)buffer, (uint8_t *)buffer, bytes, CHACHA_ROUNDS);

#ifdef CYMRIC_CYCLES
    {
        // Mix in final cycle count
        iv_mix[1] += GetCPUCycles();
    }
#endif

    blake2b_state state;

    // Iterate the hash to erase the ChaCha20 key material
    if (blake2b_init(&state, 64))
        return -3;
    if (blake2b_update(&state, (const uint8_t *)R->internal, 64))
        return -4;
    if (blake2b_final(&state, (uint8_t *)R->internal, 64))
        return -5;

    CAT_SECURE_OBJCLR(state.buf);

    return 0;
}

int cymric_derive(
    cymric_rng* basisGenerator, ///< [in] Generator to seed from
    const void* seedBuffer,     ///< [in] Additional seed buffer
    int seedBytes,              ///< [in] Seed buffer bytes
    cymric_rng* resultOut       ///< [out] Generator to seed
) {
    // If input is invalid:
    if (!resultOut || !basisGenerator)
        return -1;

    // Initialize BLAKE2 state for 512-bit output
    blake2b_state state;

    // Mix in previous or uninitialized state
    if (blake2b_init_key(&state, 64, resultOut->internal, 64))
        return -2;

    // Mix in the seed
    if (seedBuffer && seedBytes > 0) {
        if (blake2b_update(&state, (const uint8_t *)seedBuffer, seedBytes))
            return -3;
    }

    // Generate a key seed from the old RNG
    char key[64];
    if (cymric_random(basisGenerator, key, 64))
        return -4;

    // Mix in the key seed
    if (blake2b_update(&state, (const uint8_t *)key, 64))
        return -5;

    // Squeeze out 512 random bits from seed
    if (blake2b_final(&state, (uint8_t *)resultOut->internal, 64))
        return -6;

    // Erase BLAKE2 state and key
    CAT_SECURE_OBJCLR(state.buf);
    CAT_SECURE_OBJCLR(key);

    // Indicate state is seeded
    resultOut->internal[64] = 'S';
    resultOut->internal[65] = 'E';
    resultOut->internal[66] = 'E';
    resultOut->internal[67] = 'D';

    return 0;
}


#ifdef __cplusplus
}
#endif
