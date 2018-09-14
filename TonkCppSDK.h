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

#pragma once

// If this is defined, the Tonk library must be loaded by the application
//#define TONK_DISABLE_SHIM

#if !defined(TONK_DISABLE_SHIM)
#undef TONK_BUILDING
#undef TONK_DLL
#endif // !TONK_DISABLE_SHIM

#include "tonk.h"
#include "tonk_file_transfer.h"

#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <string.h> // memcpy

namespace tonk {


//------------------------------------------------------------------------------
// SDKResult

struct SDKResult
{
    /// Default result is success
    TonkResult Result = Tonk_Success;

    SDKResult(TonkResult result = Tonk_Success)
        : Result(result)
    {}

    operator bool()
    {
        return Good();
    }
    bool Good() const
    {
        return TONK_GOOD(Result);
    }
    bool Failed() const
    {
        return TONK_FAILED(Result);
    }
    std::string ToString() const
    {
        return tonk_result_to_string(Result);
    }
};


//------------------------------------------------------------------------------
// SDKJsonResult

struct SDKJsonResult : SDKResult
{
    std::string ErrorJson;

    // Override ToString
    std::string ToString() const
    {
        return ErrorJson;
    }
};


//------------------------------------------------------------------------------
// SDKSocket

class SDKConnection; // Forward declaration

class SDKSocket
{
public:
    /// Configuration to use.
    /// Note that the following members are ignored:
    /// + AppContextPtr
    /// + Version
    /// + OnP2PConnectionStart
    /// + OnIncomingConnection
    TonkSocketConfig Config;

    SDKSocket();
    virtual ~SDKSocket();

    /**
        Creates a Tonk socket that can receive and initiate connections.

        Configure the settings and call this function to create the socket.

        If TonkSocketConfig::MaximumClients is non-zero, then the socket will
        accept new connections from remote hosts.  Note that the UDPPort should
        also be set to act as a server.

        Returns SDKJsonResult::Failed() on failure.
        Returns SDKJsonResult::Good() on success.
    */
    SDKJsonResult Create();

    /**
        Connect to remote host asynchronously.

        If the connection request succeeds, then the provided SDKConnection* may be
        used to Send() data in the first datagram before the connection completes.

        The first connection request is sent from a background thread on the next
        timer tick or when Flush() is called, after hostname resolution completes.

        Returns SDKJsonResult.Failed() on failure.
        Returns SDKJsonResult.Good() on success.
    */
    SDKJsonResult Connect(
        SDKConnection* appConnectionPtr,
        const std::string& hostname, ///< [in] Hostname of the remote host
        uint16_t port ///< [in] Port for the remote host
    );

    /**
        Destroy the socket object, shutting down background threads and freeing
        system sources.  This function can be called from any callback.  It will
        complete asynchronously in the background.
    */
    void Destroy();

    /**
        tonk_advertise()

        Sends an unreliable message to the provided IP address, or the entire subnet
        if TONK_BROADCAST is specified for `ipString`.  The `port` parameter must
        match the UDPListenPort set by the listening applications.  The message will
        be delivered to all applications outside of any TonkConnection, via the
        OnAdvertisement() callback.

        Applications that set UDPListenPort = 0 cannot receive messages sent to
        TONK_BROADCAST.  But if a peer sends tonk_advertise() and another peer gets
        that advertisement message in their OnAdvertisement() callback, then the
        source port will be available in the callback even if it was set to 0.

        Anywhere from 0 to TONK_MAX_UNRELIABLE_BYTES bytes of data may be sent,
        and the message is not guaranteed to reach any peers, so it should probably
        be sent periodically.

        One example application for tonk_advertise() is service discovery:
        Clients can call tonk_advertise() with TONK_BROADCAST to send a message
        to all services running on the given port.  Those services can respond by
        connecting to the client, or by sending a tonk_advertise() back to the
        source IP address of the OnAdvertise() message with more information,
        and allow the client to initiate the connection.
    */
    SDKResult Advertise(
        const std::string& ipString,
        uint16_t port,
        const void* data,
        unsigned bytes);

    /// Return the interal socket handle
    TonkSocket GetSocket() const
    {
        return MySocket;
    }

protected:
    /**
        OnIncomingConnection() Callback

        Called when a new incoming connection completes, which was initiated by
        a remote peer on the network.

        By default no incoming connections are accepted.

        To deny a connection, return nullptr.
        Otherwise create and return a valid SDKConnection* to track the new connection.
    */
    virtual SDKConnection* OnIncomingConnection(
        const TonkAddress& address ///< Address of the client requesting a connection
    );

    /**
        [Optional] OnP2PConnectionStart() Callback

        Called just as a peer we are connected to has instructed us to connect
        to another peer using the tonk_p2p_connect() function.

        Unlike the other two On*Connection() functions, this function is called
        as a connection is starting rather than after it has completed.  This
        allows the application to provide data to send within the first round-
        trip of the peer2peer connection using tonk_send() in this function.

        By default no incoming connections are accepted.

        To deny a connection, return nullptr.
        Otherwise create and return a valid SDKConnection* to track the new connection.
    */
    virtual SDKConnection* OnP2PConnectionStart(
        const TonkAddress& address ///< Address of the other peer we are to connect to
    );

    /**
        [Optional] OnAdvertisement() Callback

        This receives a message sent by tonk_advertise(), providing the source
        IP address and source port.  This function may receive between 0 and
        TONK_MAX_UNRELIABLE_BYTES bytes of data at any time until the app calls
        the tonk_socket_destroy() function.
    */
    virtual void OnAdvertisement(
        const std::string& ipString, ///< Source IP address of advertisement
        uint16_t               port, ///< Source port address of advertisement
        const uint8_t*         data, ///< Pointer to a buffer containing the message data
        uint32_t              bytes  ///< Number of bytes in the message
    );

private:
    TonkSocket MySocket = 0;

    static TonkConnectionConfig MakeConnectionConfig(SDKConnection* sdkConnection);
};


//------------------------------------------------------------------------------
// SDKConnection

/// Represents a connection with a peer
class SDKConnection : public std::enable_shared_from_this<SDKConnection>
{
public:
    SDKConnection();
    virtual ~SDKConnection();

    /// Get connection status
    TonkStatus GetStatus();

    /// Get extended connection status
    TonkStatusEx GetStatusEx();

    /**
        Send datagram on the given channel.

        Important: This function can fail if the message is too large.
        Call GetStatus() to retrieve the maximum send size.

        Channel is a number between 0 and TonkChannel_Last (inclusive).
    */
    SDKResult Send(
        const void*               data, ///< [in] Pointer to message data
        uint64_t                 bytes, ///< [in] Message bytes
        uint32_t               channel  ///< [in] Channel to attach to message
    );

    /**
        Flush pending datagrams.

        This will send data immediately instead of waiting for more data to be sent.
    */
    void Flush();

    /**
        Asynchronously start closing the connection.

        The TonkConnection object is still alive and it is valid to call any of the
        normal API calls.  Those calls will start failing rather than adding more
        data to the outgoing queue.

        Callbacks such as OnTick() and OnData() will still be generated.
        OnClose() will definitely be called at some point after tonk_close(),
        and it will be the last callback received.  OnClose() is an excellent place
        to call tonk_free() to release object memory.
    */
    void Close();

    //------------------------------------------------------------------------------
    // Time Synchronization API
    //
    // These functions allow for generation and transformation of timestamps between
    // local and remote hosts.

    /// Get current time in microseconds
    static uint64_t TimeUsec();

    /**
        Returns 16-bit remote timestamp from a local timestamp in microseconds.

        This is a timestamp that is expected to be in the remote host's time.
        It becomes invalid after 25 seconds.  The precision is 512 microseconds.
        See the TONK_TIME16 constants above.

        Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
        as the 'localUsec' parameter.

        Note that if tonk_status() TonkStatus::Flags does not have the
        TonkFlag_TimeSync bit set then the result is undefined.

        Returns the converted 16-bit network timestamp.
    */
    uint16_t ToRemoteTime16(
        uint64_t             localUsec  ///< [in] Local timestamp in microseconds
    );

    /**
        Returns local time given 16-bit local timestamp from a message
        in the OnData() callback.

        Returns the timestamp in local time in microseconds.
    */
    uint64_t FromLocalTime16(
        uint16_t    networkTimestamp16  ///< [in] Local timestamp to interpret
    );

    /**
        Returns 23-bit remote timestamp from a local timestamp in microseconds.

        It is recommended to serialize this value into 3 instead of 4 bytes.

        This is a timestamp that is expected to be in the remote host's time.
        It becomes invalid after 50 seconds.  The precision is 8 microseconds.
        See the TONK_TIME23 constants above.

        Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
        as the 'localUsec' parameter.

        Note that if tonk_status() TonkStatus::Flags does not have the
        TonkFlag_TimeSync bit set then the result is undefined.

        Returns the converted 23-bit network timestamp.
    */
    uint32_t ToRemoteTime23(
        uint64_t             localUsec  ///< [in] Local timestamp in microseconds
    );

    /**
        Returns local time given 23-bit local timestamp from a message
        in the OnData() callback.

        Returns the timestamp in local time in microseconds.
    */
    uint64_t FromLocalTime23(
        uint32_t    networkTimestamp23  ///< [in] Local timestamp to interpret
    );

    //------------------------------------------------------------------------------
    // Peer2Peer Rendezvous Server

    /**
        Instruct two peers to connect with eachother using a custom NAT-traversal
        protocol described in HolePunch.README.txt.  It works when one side is on
        a friendly ISP like residential Comcast, but often fails if both sides are
        on e.g. corporate or phone connections.

        If either Alice or Bob are not time-synchronized, the function will fail and
        it will return Tonk_InvalidInput.

        Returns Tonk_Success on success.
        Returns other TonkResult codes on error.
    */
    SDKResult P2PConnect(
        SDKConnection* bobConnection    ///< [in] Peer 2 connection
    );

    void SetSelfReference()
    {
        SelfReference = shared_from_this();
    }

    /// Get raw connection pointer
    TonkConnection GetRawConnection() const
    {
        return MyConnection;
    }

protected:
    /**
        [Optional] OnConnect() Callback

        Called in any case where the application has successfully connected to
        a remote peer:

        (1) tonk_connect() has successfully completed an outgoing connection.
        (2) TonkSocketConfig::OnIncomingConnection accepted a connection.
        (3) OnP2PConnectionStart() was called and now connection completed.

        If this is not specified (=0) then connection status can be checked via
        the tonk_status() function.
    */
    virtual void OnConnect();

    /**
        [Optional] OnData() Callback

        Called when data arrives from a TonkConnection.

        The data pointer will be valid until the end of the function call.

        It is safe to call any tonk_*() API calls during this function.
    */
    virtual void OnData(
        uint32_t          channel,  ///< Channel number attached to each message by sender
        const uint8_t*       data,  ///< Pointer to a buffer containing the message data
        uint32_t            bytes   ///< Number of bytes in the message
    );

    /**
        [Optional] OnTick() Callback

        Periodic callback for each TonkConnection.

        It is safe to call any tonk_*() API calls during this function.
    */
    virtual void OnTick(
        uint64_t          nowUsec   ///< Current timestamp in microseconds
    );

    /**
        OnClose() Callback

        Called after the TonkConnection has closed.

        After this, other callbacks will not be called for this TonkConnection.

        After this, the TonkConnection object will still be valid:
        + tonk_status() call flags will indicate disconnected.
        + tonk_send() and other calls will fail but not crash.

        The application must call tonk_free() to free the TonkConnection object,
        which will invalidate the TonkConnection handle.
    */
    virtual void OnClose(
        const tonk::SDKJsonResult& reason
    );

private:
    friend class SDKSocket;

    // This is filled in by the SDKSocket class
    TonkConnection MyConnection = 0;

    // Keep a self-reference to enable thread-safe use of the library.
    // When Tonk calls OnClose() the SelfReference is cleared allowing the
    // object to go out of scope, which will invoke tonk_free() in the
    // SDKConnection destructor.  The tonk_free() call will inform Tonk that
    // the application is done using the TonkConnection object.
    std::shared_ptr<SDKConnection> SelfReference;
};


//------------------------------------------------------------------------------
// SDKConnectionList

/// This helper object keeps a list of all the connections with an SDKSocket.
/// This list type may be used to safety store multiple connection lists.
template<class T>
class SDKConnectionList
{
public:
    /// Insert a connection into the list
    /// Returns true if the connection was not in the list
    /// Returns false if the connection was already in the list
    bool Insert(SDKConnection* connection)
    {
        std::lock_guard<std::mutex> locker(ConnectionListLock);
        for (auto& ii : ConnectionList)
            if (ii.get() == connection)
                return false;
        ConnectionList.push_back(connection->shared_from_this());
        return true;
    }

    /// Remove a connection from the list
    void Remove(SDKConnection* connection)
    {
        std::lock_guard<std::mutex> locker(ConnectionListLock);
        if (ConnectionList.empty()) {
            return;
        }
        ConnectionList.erase(
            std::remove_if(ConnectionList.begin(), ConnectionList.end(),
                [connection](std::shared_ptr<SDKConnection> const& i)
        {
            return i.get() == connection;
        }), ConnectionList.end());
    }

    /// Return the current connection list
    std::vector< std::shared_ptr<T> > GetList()
    {
        std::lock_guard<std::mutex> locker(ConnectionListLock);

        const unsigned count = static_cast<unsigned>(ConnectionList.size());
        std::vector< std::shared_ptr<T> > result(count);

        for (unsigned i = 0; i < count; ++i)
            result[i] = std::static_pointer_cast<T, SDKConnection>(ConnectionList[i]);

        return result;
    }

protected:
    /// List mutex
    std::mutex ConnectionListLock;

    /// List of connections
    std::vector< std::shared_ptr<SDKConnection> > ConnectionList;
};


//------------------------------------------------------------------------------
// Portability macros

// Compiler-specific debug break
#if defined(_DEBUG) || defined(DEBUG)
#define TONK_CPP_SDK_DEBUG
#ifdef _WIN32
#define TONK_CPP_SDK_DEBUG_BREAK() __debugbreak()
#else
#define TONK_CPP_SDK_DEBUG_BREAK() __builtin_trap()
#endif
#define TONK_CPP_SDK_DEBUG_ASSERT(cond) { if (!(cond)) { TONK_CPP_SDK_DEBUG_BREAK(); } }
#else
#define TONK_CPP_SDK_DEBUG_BREAK() do {} while (false);
#define TONK_CPP_SDK_DEBUG_ASSERT(cond) do {} while (false);
#endif

// Compiler-specific force inline keyword
#ifdef _MSC_VER
#define TONK_CPP_SDK_FORCE_INLINE inline __forceinline
#else
#define TONK_CPP_SDK_FORCE_INLINE inline __attribute__((always_inline))
#endif

// Architecture check
#if defined(ANDROID) || defined(IOS) || defined(LINUX_ARM)
// Use aligned accesses on ARM
#define TONK_CPP_SDK_ALIGNED_ACCESSES
#endif // ANDROID


//------------------------------------------------------------------------------
// Serialization Tools

inline uint16_t ReadU16_LE(const uint8_t* data)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    return ((uint16_t)data[1] << 8) | data[0];
#else
    return *(uint16_t*)data;
#endif
}

inline uint32_t ReadU24_LE(const uint8_t* data)
{
    return ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
}

/// This version uses one memory read on Intel but requires at least 4 bytes in the buffer
inline uint32_t ReadU24_LE_Min4Bytes(const uint8_t* data)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    return ReadU24_LE(data);
#else
    return *(uint32_t*)data & 0xFFFFFF;
#endif
}

inline uint32_t ReadU32_LE(const uint8_t* data)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0];
#else
    return *(uint32_t*)data;
#endif
}

inline uint64_t ReadU64_LE(const uint8_t* data)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    return ((uint64_t)data[7] << 56) | ((uint64_t)data[6] << 48) | ((uint64_t)data[5] << 40) |
        ((uint64_t)data[4] << 32) | ((uint64_t)data[3] << 24) | ((uint64_t)data[2] << 16) |
        ((uint64_t)data[1] << 8) | data[0];
#else
    return *(uint64_t*)data;
#endif
}

inline void WriteU16_LE(uint8_t* data, uint16_t value)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint16_t*)data = value;
#endif
}

inline void WriteU24_LE(uint8_t* data, uint32_t value)
{
    data[2] = (uint8_t)(value >> 16);
    WriteU16_LE(data, (uint16_t)value);
}

inline void WriteU24_LE_Min4Bytes(uint8_t* data, uint32_t value)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    WriteU24_LE(data, value);
#else
    *(uint32_t*)data = value;
#endif
}

inline void WriteU32_LE(uint8_t* data, uint32_t value)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    data[3] = (uint8_t)(value >> 24);
    data[2] = (uint8_t)(value >> 16);
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint32_t*)data = value;
#endif
}

inline void WriteU64_LE(uint8_t* data, uint64_t value)
{
#ifdef TONK_CPP_SDK_ALIGNED_ACCESSES
    data[7] = (uint8_t)(value >> 56);
    data[6] = (uint8_t)(value >> 48);
    data[5] = (uint8_t)(value >> 40);
    data[4] = (uint8_t)(value >> 32);
    data[3] = (uint8_t)(value >> 24);
    data[2] = (uint8_t)(value >> 16);
    data[1] = (uint8_t)(value >> 8);
    data[0] = (uint8_t)value;
#else
    *(uint64_t*)data = value;
#endif
}


//------------------------------------------------------------------------------
// WriteByteStream

/// Helper class to serialize POD types to a byte buffer
struct WriteByteStream
{
    /// Wrapped data pointer
    uint8_t* Data = nullptr;

    /// Number of wrapped buffer bytes
    unsigned BufferBytes = 0;

    /// Number of bytes written so far by Write*() functions
    unsigned WrittenBytes = 0;

    explicit WriteByteStream()
    {
    }
    explicit WriteByteStream(uint8_t* data, uint64_t bytes)
        : Data(data)
        , BufferBytes((unsigned)bytes)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(data != nullptr && bytes > 0);
    }

    uint8_t* Peek()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes <= BufferBytes);
        return Data + WrittenBytes;
    }
    inline unsigned Remaining()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes <= BufferBytes);
        return BufferBytes - WrittenBytes;
    }

    inline void Write8(uint8_t value)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes + 1 <= BufferBytes);
        Data[WrittenBytes] = value;
        WrittenBytes++;
    }
    inline void Write16(uint16_t value)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes + 2 <= BufferBytes);
        WriteU16_LE(Data + WrittenBytes, value);
        WrittenBytes += 2;
    }
    inline void Write24(uint32_t value)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes + 3 <= BufferBytes);
        WriteU24_LE(Data + WrittenBytes, value);
        WrittenBytes += 3;
    }
    inline void Write32(uint32_t value)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes + 4 <= BufferBytes);
        WriteU32_LE(Data + WrittenBytes, value);
        WrittenBytes += 4;
    }
    inline void Write64(uint64_t value)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes + 8 <= BufferBytes);
        WriteU64_LE(Data + WrittenBytes, value);
        WrittenBytes += 8;
    }
    inline void WriteBuffer(const void* source, size_t bytes)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(source != nullptr || bytes == 0);
        TONK_CPP_SDK_DEBUG_ASSERT(WrittenBytes + bytes <= BufferBytes);
        memcpy(Data + WrittenBytes, source, bytes);
        WrittenBytes += static_cast<unsigned>(bytes);
    }
    inline void WriteString(const std::string& str)
    {
        // Write string bytes without null terminator or length (length must be written separately)
        WriteBuffer(str.c_str(), str.size());
    }
};


//------------------------------------------------------------------------------
// ReadByteStream

/// Helper class to deserialize POD types from a byte buffer
struct ReadByteStream
{
    /// Wrapped data pointer
    const uint8_t* const Data;

    /// Number of wrapped buffer bytes
    const unsigned BufferBytes;

    /// Number of bytes read so far by Read*() functions
    unsigned BytesRead;


    ReadByteStream(const uint8_t* data, uint64_t bytes)
        : Data(data)
        , BufferBytes((unsigned)bytes)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(data != nullptr);
        BytesRead = 0;
    }

    inline const uint8_t* Peek()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead <= BufferBytes);
        return Data + BytesRead;
    }
    inline unsigned Remaining()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead <= BufferBytes);
        return BufferBytes - BytesRead;
    }
    inline void Skip(unsigned bytes)
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + bytes <= BufferBytes);
        BytesRead += bytes;
    }

    inline const uint8_t* Read(unsigned bytes)
    {
        const uint8_t* data = Peek();
        Skip(bytes);
        return data;
    }
    inline uint8_t Read8()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + 1 <= BufferBytes);
        uint8_t value = *Peek();
        BytesRead++;
        return value;
    }
    inline uint16_t Read16()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + 2 <= BufferBytes);
        uint16_t value = ReadU16_LE(Peek());
        BytesRead += 2;
        return value;
    }
    inline uint32_t Read24()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + 3 <= BufferBytes);
        uint32_t value = ReadU24_LE(Peek());
        BytesRead += 3;
        return value;
    }
    inline uint32_t Read32()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + 4 <= BufferBytes);
        uint32_t value = ReadU32_LE(Peek());
        BytesRead += 4;
        return value;
    }
    inline uint64_t Read64()
    {
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + 8 <= BufferBytes);
        uint64_t value = ReadU64_LE(Peek());
        BytesRead += 8;
        return value;
    }
    inline std::string ReadString(unsigned bytes)
    {
        // Read string without null terminator (length must be provided)
        TONK_CPP_SDK_DEBUG_ASSERT(BytesRead + bytes <= BufferBytes);
        const uint8_t* peek = Peek();
        BytesRead += bytes;
        return std::string(peek, peek + bytes);
    }
};


} // namespace tonk
