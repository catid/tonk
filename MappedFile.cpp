/*
    Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

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

#include "MappedFile.h"


//------------------------------------------------------------------------------
// Operating System Check

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

#if defined(CAT_OS_LINUX) || defined(CAT_OS_OSX)
# include <sys/mman.h>
# include <sys/stat.h>
# include <fcntl.h>
# include <errno.h>
#endif

#if defined(CAT_OS_WINDOWS)
# include <windows.h>
#elif defined(CAT_OS_LINUX) || defined(CAT_OS_AIX) || defined(CAT_OS_SOLARIS) || defined(CAT_OS_IRIX)
# include <unistd.h>
#elif defined(CAT_OS_OSX) || defined(CAT_OS_BSD)
# include <sys/sysctl.h>
# include <unistd.h>
#elif defined(CAT_OS_HPUX)
# include <sys/mpctl.h>
#endif


namespace cat {


static uint32_t GetAllocationGranularity()
{
    uint32_t alloc_gran = 0;
#if defined(CAT_OS_WINDOWS)
    SYSTEM_INFO sys_info;
    ::GetSystemInfo(&sys_info);
    alloc_gran = sys_info.dwAllocationGranularity;
#elif defined(CAT_OS_OSX) || defined(CAT_OS_BSD)
    alloc_gran = (uint32_t)getpagesize();
#else
    alloc_gran = (uint32_t)sysconf(_SC_PAGE_SIZE);
#endif
    return alloc_gran > 0 ? alloc_gran : 32;
}


//// MappedFile

MappedFile::MappedFile()
{
    Length = 0;
#if defined(CAT_OS_WINDOWS)
    File = INVALID_HANDLE_VALUE;
#else
    File = -1;
#endif
}

MappedFile::~MappedFile()
{
    Close();
}

bool MappedFile::OpenRead(
    const char* path,
    bool read_ahead,
    bool no_cache)
{
    Close();

    ReadOnly = true;

#if defined(CAT_OS_WINDOWS)

    (void)no_cache;
    const uint32_t access_pattern = !read_ahead ? FILE_FLAG_RANDOM_ACCESS : FILE_FLAG_SEQUENTIAL_SCAN;

    File = ::CreateFileA(
        path,
        GENERIC_READ,
        FILE_SHARE_READ,
        0,
        OPEN_EXISTING,
        access_pattern,
        0);

    if (File == INVALID_HANDLE_VALUE) {
        return false;
    }

    const BOOL getSuccess = ::GetFileSizeEx(File, (LARGE_INTEGER*)&Length);

    if (getSuccess != TRUE) {
        return false;
    }

#else
    File = open(path, O_RDONLY, (mode_t)0444);

    if (File == -1) {
        return false;
    }

    Length = lseek(File, 0, SEEK_END);

    if (Length <= 0) {
        return false;
    }

    (void)read_ahead;
#ifdef F_RDAHEAD
    if (read_ahead) {
        fcntl(File, F_RDAHEAD, 1);
    }
#endif

    (void)no_cache;
#ifdef F_NOCACHE
    if (no_cache) {
        fcntl(File, F_NOCACHE, 1);
    }
#endif

#endif

    return true;
}

bool MappedFile::OpenWrite(
    const char* path,
    uint64_t size)
{
    Close();

    ReadOnly = false;
    Length = 0;

#if defined(CAT_OS_WINDOWS)

    const uint32_t access_pattern = FILE_FLAG_SEQUENTIAL_SCAN;

    File = ::CreateFileA(
        path,
        GENERIC_WRITE|GENERIC_READ,
        FILE_SHARE_WRITE,
        0,
        CREATE_ALWAYS,
        access_pattern,
        0);

#else

    File = open(path, O_RDWR|O_CREAT|O_TRUNC, (mode_t)0666);

#endif

    return Resize(size);
}

bool MappedFile::Resize(uint64_t size)
{
    Length = size;

#if defined(CAT_OS_WINDOWS)

    if (File == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Set file size
    BOOL setSuccess = ::SetFilePointerEx(
        File,
        *(LARGE_INTEGER*)&Length,
        0,
        FILE_BEGIN);

    if (setSuccess != TRUE) {
        return false;
    }

    if (!::SetEndOfFile(File)) {
        return false;
    }

#else

    if (File == -1) {
        return false;
    }

    const int truncateResult = ftruncate(File, (off_t)size);

    if (0 != truncateResult) {
        return false;
    }

#if 0
    const int seekResult = lseek(File, size - 1, SEEK_SET);

    if (-1 == seekResult) {
        return false;
    }

    const int writeResult = write(File, "", 1);

    if (1 != writeResult) {
        return false;
    }
#endif

#endif

    return true;
}

void MappedFile::Close()
{
#if defined(CAT_OS_WINDOWS)

    if (File != INVALID_HANDLE_VALUE)
    {
        ::CloseHandle(File);
        File = INVALID_HANDLE_VALUE;
    }

#else

    if (File != -1)
    {
        close(File);
        File = -1;
    }

#endif

    Length = 0;
}


//// MappedView

MappedView::MappedView()
{
    Data = 0;
    Length = 0;
    Offset = 0;

#if defined(CAT_OS_WINDOWS)
    Map = 0;
#else
    Map = MAP_FAILED;
#endif
}

MappedView::~MappedView()
{
    Close();
}

bool MappedView::Open(MappedFile *file)
{
    Close();

    if (!file || !file->IsValid()) {
        return false;
    }

    File = file;

    return true;
}

uint8_t* MappedView::MapView(uint64_t offset, uint32_t length)
{
    Close();

    if (length == 0) {
        length = static_cast<uint32_t>( File->Length );
    }

    if (offset)
    {
        const uint32_t granularity = GetAllocationGranularity();

        // Bring offset back to the previous allocation granularity
        const uint32_t mask = granularity - 1;
        const uint32_t masked = static_cast<uint32_t>(offset) & mask;

        offset -= masked;
        length += masked;
    }

#if defined(CAT_OS_WINDOWS)

    const uint32_t protect = File->ReadOnly ? PAGE_READONLY : PAGE_READWRITE;

    Map = ::CreateFileMappingA(
        File->File,
        nullptr,
        protect,
        0, // no max size
        0, // no min size
        nullptr);

    if (!Map) {
        return nullptr;
    }

    uint32_t flags = FILE_MAP_READ;

    if (!File->ReadOnly) {
        flags |= FILE_MAP_WRITE;
    }

    Data = (uint8_t*)::MapViewOfFile(
        Map,
        flags,
        (uint32_t)(offset >> 32),
        (uint32_t)offset, length);

    if (!Data) {
        return nullptr;
    }

#else

    int prot = PROT_READ;

    if (!File->ReadOnly) {
        prot |= PROT_WRITE;
    }

    Map = mmap(
        0,
        length,
        prot,
        MAP_SHARED,
        File->File,
        offset);

    if (Map == MAP_FAILED) {
        return 0;
    }

    Data = reinterpret_cast<uint8_t*>( Map );

#endif

    Offset = offset;
    Length = length;

    return Data;
}

void MappedView::Close()
{
#if defined(CAT_OS_WINDOWS)

    if (Data)
    {
        ::UnmapViewOfFile(Data);
        Data = 0;
    }

    if (Map)
    {
        ::CloseHandle(Map);
        Map = 0;
    }

#else

    if (Map != MAP_FAILED)
    {
        munmap(Map, Length);
        Map = MAP_FAILED;
    }

    Data = 0;

#endif

    Length = 0;
    Offset = 0;
}


} // namespace cat
