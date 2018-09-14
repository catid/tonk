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

/*
    Memory-mapped files are a fairly good compromise between performance and flexibility.

    Compared with asynchronous io, memory-mapped files are:
        + Much easier to implement in a portable way
        + Automatically paged in and out of RAM
        + Automatically read-ahead cached

    When asynch io is not available or blocking is acceptable then this is a
    great alternative with low overhead and similar performance.

    For random file access, use MappedView with a MappedFile that has been
    opened with random_access = true.  Random access is usually used for a
    database-like file type, which is much better implemented using asynch io.
*/

#pragma once

#include <stdint.h>

namespace cat {

struct MappedView;


// Memory-mapped file
struct MappedFile
{
    friend struct MappedView;

#if defined(_WIN32)
    /*HANDLE*/ void* File = nullptr;
#else
    int File = -1;
#endif

    bool ReadOnly = true;
    uint64_t Length = 0;

    inline bool IsValid() const { return Length != 0; }

    // Opens the file for shared read-only access with other applications
    // Returns false on error (file not found, etc)
    bool OpenRead(
        const char* path,
        bool read_ahead = false,
        bool no_cache = false);

    // Creates and opens the file for exclusive read/write access
    bool OpenWrite(
        const char* path,
        uint64_t size);

    // Resizes a file
    bool Resize(uint64_t size);

    void Close();

    MappedFile();
    ~MappedFile();
};


// View of a portion of the memory mapped file
struct MappedView
{
    void* Map = nullptr;
    MappedFile* File = nullptr;
    uint8_t* Data = nullptr;
    uint64_t Offset = 0;
    uint32_t Length = 0;

    // Returns false on error
    bool Open(MappedFile* file);

    // Returns 0 on error, 0 length means whole file
    uint8_t* MapView(uint64_t offset = 0, uint32_t length = 0);

    void Close();

    MappedView();
    ~MappedView();
};


} // namespace cat
