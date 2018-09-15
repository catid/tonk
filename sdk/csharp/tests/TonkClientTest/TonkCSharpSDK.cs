/** \file
    \brief Tonk C# SDK
    \copyright Copyright (c) 2017 Christopher A. Taylor.  All rights reserved.

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

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

/// This code requires unsafe C# code for the OnData() handlers.
namespace TonkSDK
{
    //------------------------------------------------------------------------------
    // SDKResult

    public class SDKResult
    {
        /// Default result is success
        public Tonk.Result Result = Tonk.Result.Success;

        public SDKResult(Tonk.Result result = Tonk.Result.Success)
        {
            Result = result;
        }

        public bool Good()
        {
            return Tonk.IsGood(Result);
        }
        public bool Failed()
        {
            return Tonk.IsFailed(Result);
        }
        public override string ToString()
        {
            return Tonk.ResultToString(Result);
        }
    };


    //------------------------------------------------------------------------------
    // SDKJsonResult

    public class SDKJsonResult : SDKResult
    {
        public string ErrorJson = "";

        // Override ToString
        public override string ToString()
        {
            string stringResult = Tonk.ResultToString(Result);
            return string.Format("[{0} : {1}]", stringResult, ErrorJson);
        }
    };


    //------------------------------------------------------------------------------
    // SDKAddress

    public class SDKAddress
    {
        /// IPv4 or IPv6 human-readable IP address.  Guaranteed to be null-terminated
        public string IPAddress;

        /// UDP Port
        public UInt16 UDPPort;
    }

    //------------------------------------------------------------------------------
    // SDKSocket

    /// Represents a UDP socket.
    /// Call Create() to start the socket.
    /// This should be disposed explicitly via .Dispose();
    public class SDKSocket : IDisposable
    {
        /// Configuration to use.
        /// Note that the following members are ignored:
        /// + AppContextPtr
        /// + Version
        /// + OnP2PConnectionStart
        /// + OnIncomingConnection
        public Tonk.SocketConfig Config;

        public SDKSocket()
        {
            Tonk.Result result = Tonk.tonk_set_default_socket_config(
                Tonk.TONK_VERSION,
                out Config);

            // *****************************************************************
            // If this fails it indicates that the Tonk DLL is not in sync with
            // this SDK header and needs to be updated.
            Debug.Assert(result == Tonk.Result.Success);
            // *****************************************************************
        }

        /// Create a ConnectionConfig object
        private Tonk.ConnectionConfig MakeConnectionConfig(SDKConnection sdkConnection)
        {
            // Set the new delegate objects on the SDKConnection directly
            // so that they stick around until it is GC.
            sdkConnection.Config.OnConnect = delegate(IntPtr context, IntPtr connection)
            {
                sdkConnection.OnConnect();
            };
            sdkConnection.Config.OnTick = delegate(IntPtr context, IntPtr connection, UInt64 nowUsec)
            {
                sdkConnection.OnTick(nowUsec);
            };
            sdkConnection.Config.OnData = delegate(IntPtr context, IntPtr connection, UInt32 channel, IntPtr data, UInt32 bytes)
            {
                unsafe
                {
                    sdkConnection.OnData(channel, (byte*)data, bytes);
                }
            };
            sdkConnection.Config.OnClose = delegate(IntPtr context, IntPtr connection, Tonk.Result reason, string reasonJson)
            {
                SDKJsonResult result = new SDKJsonResult();
                result.ErrorJson = reasonJson;
                result.Result = reason;
                sdkConnection.OnClose(result);

                // Release self-reference and local ref, allowing object to go out of scope.
                sdkConnection.ReleaseSelfReference();
                sdkConnection = null;

                // Force a GC collect here to avoid hanging during shutdown
                //GC.Collect();
            };

            return sdkConnection.Config;
        }

        /**
            Creates a Tonk socket that can receive and initiate connections.

            Configure the settings and call this function to create the socket.

            If TonkSocketConfig::MaximumClients is non-zero, then the socket will
            accept new connections from remote hosts.  Note that the UDPPort should
            also be set to act as a server.

            Returns SDKJsonResult::Failed() on failure.
            Returns SDKJsonResult::Good() on success.
        */
        public SDKJsonResult Create()
        {
            Destroy();

            Config.Version = Tonk.TONK_VERSION;

            Config.OnP2PConnectionStart = delegate (IntPtr context, IntPtr connection, ref Tonk.Address address, out Tonk.ConnectionConfig conf)
            {
                SDKAddress sdkAddress = new SDKAddress();
                sdkAddress.IPAddress = address.GetIPString();
                sdkAddress.UDPPort = address.UDPPort;
                SDKConnection sdkConnection = this.OnP2PConnectionStart(sdkAddress);
                if (sdkConnection == null)
                {
                    conf = new Tonk.ConnectionConfig();
                    return Tonk.TONK_DENY_CONNECTION;
                }

                sdkConnection.MyConnection = connection;
                // Hold a self-reference if connection is started
                sdkConnection.HoldSelfReference();
                conf = MakeConnectionConfig(sdkConnection);
                return Tonk.TONK_ACCEPT_CONNECTION;
            };
            Config.OnIncomingConnection = delegate (IntPtr context, IntPtr connection, ref Tonk.Address address, out Tonk.ConnectionConfig conf)
            {
                SDKAddress sdkAddress = new SDKAddress();
                sdkAddress.IPAddress = address.GetIPString();
                sdkAddress.UDPPort = address.UDPPort;
                SDKConnection sdkConnection = this.OnIncomingConnection(sdkAddress);
                if (sdkConnection == null)
                {
                    conf = new Tonk.ConnectionConfig();
                    return Tonk.TONK_DENY_CONNECTION;
                }

                sdkConnection.MyConnection = connection;
                // Hold a self-reference if connection is started
                sdkConnection.HoldSelfReference();
                conf = MakeConnectionConfig(sdkConnection);
                return Tonk.TONK_ACCEPT_CONNECTION;
            };

            Tonk.ErrorJson errorJson;
            Tonk.Result result = Tonk.tonk_socket_create(
                ref Config,
                out MySocket,
                out errorJson);

            SDKJsonResult error = new SDKJsonResult();
            if (Tonk.IsFailed(result))
            {
                error.Result = result;
                error.ErrorJson = errorJson.ToString();
            }
            return error;
        }

        /**
            Connect to remote host asynchronously.

            If the connection request succeeds, then the provided SDKConnection* may be
            used to Send() data in the first datagram before the connection completes.

            The first connection request is sent from a background thread on the next
            timer tick or when Flush() is called, after hostname resolution completes.

            Returns SDKJsonResult.Failed() on failure.
            Returns SDKJsonResult.Good() on success.
        */
        public SDKJsonResult Connect(
            SDKConnection sdkConnection,
            string hostname, ///< [in] Hostname of the remote host
            UInt16 port ///< [in] Port for the remote host
        )
        {
            Tonk.ConnectionConfig config = MakeConnectionConfig(sdkConnection);

            Tonk.ErrorJson errorJson;
            Tonk.Result result = Tonk.tonk_connect(
                MySocket,
                config,
                hostname,
                port,
                out sdkConnection.MyConnection,
                out errorJson);

            SDKJsonResult error = new SDKJsonResult();
            if (Tonk.IsFailed(result))
            {
                error.Result = result;
                error.ErrorJson = errorJson.ToString();
            }
            else
            {
                // Hold a self-reference if connection is started
                sdkConnection.HoldSelfReference();
            }
            return error;
        }

        /**
            Destroy the socket object, shutting down background threads and freeing
            system sources.  This function can be called from any callback.  It will
            complete asynchronously in the background.
        */
        public void Destroy()
        {
            if (MySocket != IntPtr.Zero)
            {
                // Kick off a GC collection to allow tonk_free() to be called
                // by all the SDKConnection finalizers
                //GC.Collect();

                Tonk.tonk_socket_destroy(MySocket, 0/*do not block*/);
                MySocket = IntPtr.Zero;
            }
        }

        /**
            OnIncomingConnection() Callback

            Called when a new incoming connection completes, which was initiated by
            a remote peer on the network.

            By default no incoming connections are accepted.

            To deny a connection, return nullptr.
            Otherwise create and return a valid SDKConnection* to track the new connection.
        */
        protected virtual SDKConnection OnIncomingConnection(SDKAddress address)
        {
            // By default no incoming connections are accepted.
            return null;
        }

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
        protected virtual SDKConnection OnP2PConnectionStart(SDKAddress address)
        {
            // By default no incoming connections are accepted.
            return null;
        }

        private IntPtr MySocket = IntPtr.Zero;

        #region IDisposable Support
        private bool disposedValue = false; // To detect redundant calls

        protected virtual void Dispose(bool disposing)
        {
            if (!disposedValue)
            {
                Destroy();

                disposedValue = true;
            }
        }

        // override a finalizer: Dispose(bool disposing) above has code to free unmanaged resources.
        ~SDKSocket() {
            // Do not change this code. Put cleanup code in Dispose(bool disposing) above.
            Dispose(false);
        }

        // This code added to correctly implement the disposable pattern.
        public void Dispose()
        {
            // Do not change this code. Put cleanup code in Dispose(bool disposing) above.
            Dispose(true);
            // uncomment the following line if the finalizer is overridden above.
            GC.SuppressFinalize(this);
        }
        #endregion
    };


    //------------------------------------------------------------------------------
    // SDKConnection

    /// Represents a connection with a peer
    public class SDKConnection
    {
        ~SDKConnection()
        {
            Tonk.tonk_free(MyConnection);
        }

        /// Get connection status
        public Tonk.Status GetStatus()
        {
            Tonk.Status status;
            Tonk.tonk_status(MyConnection, out status);
            return status;
        }

        /// Get extended connection status
        public Tonk.StatusEx GetStatusEx()
        {
            Tonk.StatusEx status;
            Tonk.tonk_status_ex(MyConnection, out status);
            return status;
        }

        /**
            Send datagram on the given channel.

            Important: This function can fail if the message is too large.
            Call GetStatus() to retrieve the maximum send size.

            Channel is a number between 0 and TonkChannel_Last (inclusive).
        */
        public SDKResult Send(
            byte[] data, ///< [in] Pointer to message data
            UInt32 channel = Tonk.Channel.Reliable0  ///< [in] Channel to attach to message
        )
        {
            Tonk.Result result;
            unsafe
            {
                fixed (byte* p = data)
                {
                    result = Tonk.tonk_send(MyConnection, (IntPtr)p, (ulong)data.Length, channel);
                }
            }
            return new SDKResult(result);
        }

        /**
            Flush pending datagrams.

            This will send data immediately instead of waiting for more data to be sent.
        */
        public void Flush()
        {
            Tonk.tonk_flush(MyConnection);
        }

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
        public void Close()
        {
            Tonk.tonk_close(MyConnection);
        }

        //------------------------------------------------------------------------------
        // Time Synchronization API
        //
        // These functions allow for generation and transformation of timestamps between
        // local and remote hosts.

        /// Get current time in microseconds
        public static UInt64 TimeUsec()
        {
            return Tonk.tonk_time();
        }

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
        public UInt16 ToRemoteTime16(
            UInt64 localUsec  ///< [in] Local timestamp in microseconds
            )
        {
            return Tonk.tonk_to_remote_time_16(MyConnection, localUsec);
        }

        /**
            Returns local time given 16-bit local timestamp from a message
            in the OnData() callback.

            Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
            as the 'localUsec' parameter.

            Returns the timestamp in local time in microseconds.
        */
        public UInt64 FromLocalTime16(
            UInt16 networkTimestamp16  ///< [in] Local timestamp to interpret
            )
        {
            return Tonk.tonk_from_local_time_16(MyConnection, networkTimestamp16);
        }

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
        public UInt32 ToRemoteTime23(
            UInt64 localUsec  ///< [in] Local timestamp in microseconds
            )
        {
            return Tonk.tonk_to_remote_time_23(MyConnection, localUsec);
        }

        /**
            Returns local time given 23-bit local timestamp from a message
            in the OnData() callback.

            Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
            as the 'localUsec' parameter.

            Returns the timestamp in local time in microseconds.
        */
        public UInt64 FromLocalTime23(
            UInt32 networkTimestamp23  ///< [in] Local timestamp to interpret
            )
        {
            return Tonk.tonk_from_local_time_23(MyConnection, networkTimestamp23);
        }

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
        public SDKResult P2PConnect(
            SDKConnection bobConnection    ///< [in] Peer 2 connection
            )
        {
            if (bobConnection == null)
            {
                return new SDKResult(Tonk.Result.InvalidInput);
            }
            Tonk.Result result = Tonk.tonk_p2p_connect(MyConnection, bobConnection.MyConnection);
            return new SDKResult(result);
        }

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
        public virtual void OnConnect()
        {
        }

        /**
            [Optional] OnData() Callback

            Called when data arrives from a TonkConnection.

            The data pointer will be valid until the end of the function call.

            It is important not to modify any of the provided data, since that
            data is later used for FEC decoding of other data.

            It is safe to call any tonk_*() API calls during this function.
        */
        public virtual unsafe void OnData(
            UInt32 channel,  ///< Channel number attached to each message by sender
                byte* data,  ///< Pointer to a buffer containing the message data
                UInt32 bytes ///< Number of bytes of data
            )
        {
        }

        /**
            [Optional] OnTick() Callback

            Periodic callback for each TonkConnection.

            It is safe to call any tonk_*() API calls during this function.
        */
        public virtual void OnTick(
            UInt64 nowUsec   ///< Current timestamp in microseconds
            )
        {
        }

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
        public virtual void OnClose(SDKJsonResult reason)
        {
        }

        // This is filled in by the SDKSocket class
        public IntPtr MyConnection = IntPtr.Zero;

        // Keep a reference to the ConnectionConfig because it contains delegate
        // objects that must not go out of scope before the whole object is GC.
        public Tonk.ConnectionConfig Config = new Tonk.ConnectionConfig();

        // Keep a self-reference to enable thread-safe use of the library.
        // When Tonk calls OnClose() the SelfReference is cleared allowing the
        // object to go out of scope, which will invoke tonk_free() in the
        // SDKConnection destructor.  The tonk_free() call will inform Tonk that
        // the application is done using the TonkConnection object.
        private GCHandle GCReference;

        public void HoldSelfReference()
        {
            GCReference = GCHandle.Alloc(this);
        }

        public void ReleaseSelfReference()
        {
            GCReference.Free();
        }
    }
}

public static class Tonk
{
    /// Library TONK_VERSION must match tonk.h
    /// Mismatched TonkVersion indicates that the linked library does not match this wrapper.
    public const int TONK_VERSION = 9;

    /// TonkResult
    /// These are the result codes that can be returned from the tonk_*() functions.
    public enum Result
    {
        /// Success code
        Success = 0,

        /// A function parameter was invalid
        InvalidInput = 1,

        /// An error occurred during the request
        Error = 2,

        /// Hostname lookup failed
        HostnameLookup = 3,

        /// Provided TonkChannel number is invalid.
        /// Accepted input is TonkChannel_Unreliable,
        /// TonkChannel_Reliable0 + N, and TonkChannel_LowPri0 + N,
        /// where N = [0 ... TonkChannel_Last]
        InvalidChannel = 4,

        /// Message was too large.
        /// Reliable messages must be less than 64000 TONK_MAX_RELIABLE_BYTES bytes.
        /// Unreliable messages must not exceed 1280 TONK_MAX_UNRELIABLE_BYTES bytes
        MessageTooLarge = 5,

        /// Network library (asio) returned a failure code
        NetworkFailed = 6,

        /// PRNG library (cymric) returned a failure code
        PRNGFailed = 7,

        /// FEC library (siamese) returned a failure code
        FECFailed = 8,

        /// Authenticated peer network data was invalid
        InvalidData = 9,

        /// Connection rejected by remote host
        ConnectionRejected = 10,

        /// Connection ignored by remote host
        ConnectionTimeout = 11,

        /// The application requested a disconnect
        AppRequest = 12,

        /// The remote application requested a disconnect
        RemoteRequest = 13,

        /// The remote application stopped communicating
        RemoteTimeout = 14,

        /// Received network data could not be authenticated
        BogonData = 15,

        /// Out of memory
        OOM = 16,

        /// Tonk dynamic library not found on disk
        DLL_Not_Found = 27,
    }

    /// Evaluates true if the result is a success code
    public static bool IsGood(Result result)
    {
        return Result.Success == result;
    }

    /// Evaluates true if the result is a failure code
    public static bool IsFailed(Result result)
    {
        return Result.Success != result;
    }

    /// Convert a TonkResult result code into a string
    public static string ResultToString(Result result)
    {
        return Marshal.PtrToStringAnsi(tonk_result_to_string(result));
    }

    // Note that C# marshaller assumes char* returns are allocated with
    // CoTaskMemAlloc, which is not the case here.
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern IntPtr tonk_result_to_string(Result result);

    /// Maximum number of bytes to return, including string null terminator
    public const int TONK_ERROR_JSON_MAX_BYTES = 1024;

    /// JSON string representation of an error
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct ErrorJson
    {
        /// Guaranteed to be a null-terminated UTF-8 string
        /// Warning: Data may be invalid Json if truncated
        [MarshalAs(UnmanagedType.ByValArray, ArraySubType = UnmanagedType.LPStr, SizeConst = TONK_ERROR_JSON_MAX_BYTES)]
        private char[] JsonString;

        public override string ToString()
        {
            return new string(JsonString).TrimEnd('\0');
        }
    }

    [Flags]
    /// The TonkStatus::Flags bitfield is a combination of these flags
    public enum Flags
    {
        /// When set: The connection initated from the local socket
        Initiated = 0x10000000,

        /// When set: Time synchronization is ready
        TimeSync = 0x00000001,

        /// When set: UPnP/NATPMP port mapping on the router was successful
        NATMap_Local = 0x00000100,

        /// When set: UPnP/NATPMP port mapping was successful for the peer
        NATMap_Remote = 0x00000200,

        /// When set: Connection attempt is in progress
        Connecting = 0x01000000,

        /// When set: Connected
        Connected = 0x02000000,

        /// When set: Disconnected
        Disconnected = 0x04000000,
    };

    /// Number of bytes in the NetworkString field (including null '\0' terminator)
    public const int TONK_IP_STRING_BYTES = 40;

    /// IP string and UDP port
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Ansi)]
    public struct Address
    {
        /// IPv4 or IPv6 human-readable IP address.  Guaranteed to be null-terminated
        [MarshalAs(UnmanagedType.ByValArray, ArraySubType = UnmanagedType.LPStr, SizeConst = TONK_IP_STRING_BYTES)]
        private char[] IPString;

        /// UDP Port
        public UInt16 UDPPort;

        public string GetIPString()
        {
            return new string(IPString).TrimEnd('\0');
        }

        public override string ToString()
        {
            return string.Format("{0} : {1}", GetIPString(), UDPPort);
        }
    }

    /// Before the ID assignment message is received, tonk_status() returns this ID
    public static UInt32 TONK_INVALID_ID = 0x7fffffff;

    /// This is a data-structure returned by tonk_status().
    /// This structure details the current state of the TonkConnection
    [StructLayout(LayoutKind.Sequential)]
    public struct Status
    {
        /// Combination of TonkFlags
        public Tonk.Flags Flags;

        /// This is the ID for the remote host assigned by the local host
        public UInt32 LocallyAssignedIdForRemoteHost;

        /// Requested app send rate in bytes per second (BPS).
        /// Tonk will probe available bandwidth at higher BPS using redundant data
        /// and when it detects a higher send rate is possible, AppBPS increases.
        public UInt32 AppBPS;

        /// Estimated time for new Reliable messages to be sent.
        /// This includes the Unreliable/Unordered messages that will go out first
        public UInt32 ReliableQueueMsec; ///< milliseconds

        /// Estimated time for new LowPri messages to be sent.
        /// This includes the Reliable messages that will go out before these.
        /// Apps can use this to keep the network fed for file transfer purposes
        public UInt32 LowPriQueueMsec; ///< milliseconds

        /// Interval between timer OnTick() calls in microseconds
        public UInt32 TimerIntervalUsec; ///< microseconds
    }

    /// This is a data-structure returned by tonk_status().
    /// This structure details the current state of the TonkConnection
    [StructLayout(LayoutKind.Sequential)]
    public struct StatusEx
    {
        /// Shared 64-bit random key for this connection.
        /// This will not change
        public UInt64 ConnectionKey;

        /// This is the network address of the local UDP socket provided by the peer.
        /// This may change infrequently
        public Address Local;

        /// This is the latest network address of the remote UDP socket.
        /// This may change infrequently
        public Address Remote;

        /// External (Internet) port that is mapped to the local port.
        /// Set to 0 if no mapping has succeeded so far
        public UInt16 LocalNATMapExternalPort;

        /// External (Internet) port that is mapped to the remote port.
        /// Set to 0 if no mapping has succeeded so far
        public UInt16 RemoteNATMapExternalPort;

        /// This is the ID for the local host assigned by the remote host.
        /// This may change infrequently
        public UInt32 RemoteAssignedIdForLocalHost;

        /// Rate of data sent to peer, reported by peer, in BPS
        public UInt32 PeerSeenBPS; ///< bytes per second

        /// Packetloss of messages sent to peer, reported by peer
        public float PeerSeenLossRate; ///< 0..1

        /// One-way trip time for messages sent to peer, reported by peer
        public UInt32 PeerTripUsec; ///< microseconds

        /// Rate of data received from peer, in BPS
        public UInt32 IncomingBPS; ///< bytes per second

        /// Packetloss of messages received from peer
        public float IncomingLossRate; ///< 0..1

        /// Average Maximum One-Way Delay (OWD) Network Trip time from peer
        public UInt32 TripUsec; ///< microseconds
    }

    /**
        TonkChannel

        Each message that is sent between TonkConnections is assigned to a numbered
        channel that is provided to the OnData() callback on the receiver.

        There are four types of message delivery:


        (1) Unreliable sent with TonkChannel_Unreliable.
        Unreliable messages can be up to TONK_MAX_UNRELIABLE_BYTES = 1280 bytes.

        Unreliable messages are delivered out of order.
        Unlike UDP datagrams, unreliable messages will never be duplicated.


        (2) Unordered sent with TonkChannel_Unordered.
        Unordered messages can be up to TONK_MAX_UNORDERED_BYTES = 1280 bytes.

        Unordered messages are delivered out of order.
        Eventually all unordered messages will be delivered.


        (3) Reliable messages sent with TonkChannel_Reliable0 + N.
        There are up to TonkChannel_Count = 6 reliable channels.
        Reliable messages can be up to TONK_MAX_RELIABLE_BYTES = 64000 bytes.

        Reliable messages are delivered in order with other reliable messages.
        If a message is sent on (TonkChannel_Reliable0 + 1) followed by a message
        sent on (TonkChannel_Reliable0 + 2), then both are delivered in that order.
        Messages that cannot fit into one UDP datagram will be split up.

        The + N channel number from 0..TonkChannel_Last is given meaning by the app.
        As examples, it can be used to indicate the type of data being sent or
        it can be used to indicate that some data is part of a file transfer.


        (4) Low-priority messages sent with TonkChannel_LowPri0 + N.
        There are up to TonkChannel_Count = 6 LowPri reliable channels.
        LowPri messages can be up to TONK_MAX_LOWPRI_BYTES = 64000 bytes.

        LowPri messages are delivered in order with other LowPri messages.
        If a message is sent on (TonkChannel_LowPri0 + 1) followed by a message sent
        on (TonkChannel_LowPri0 + 2), then both are delivered in that order.
        Messages that cannot fit into one UDP datagram will be split up.


        Delivery Order:

        Unreliable, Reliable, and LowPri data are not delivered in order with
        respect to eachother.  For example an Unreliable message that is sent
        after a Reliable message may be delivered before the Reliable message.
        Also a LowPri message sent before a Reliable message may be delivered
        after the Reliable message.

        When deciding what messages to deliver first, the Unordered and Unreliable
        messages are sent first, and then the Reliable messages, and then the
        LowPri messages.  The details of the message interleaving are complicated,
        and are currently undocumented.
    */
    public const int TONK_MAX_UNRELIABLE_BYTES = 1280;
    public const int TONK_MAX_UNORDERED_BYTES = 1280;
    public const int TONK_MAX_RELIABLE_BYTES = 64000;
    public const int TONK_MAX_LOWPRI_BYTES = 64000;

    public static class Channel
    {
        /// Number of reliable channels
        public const uint Count = 6;

        /// Last reliable channel
        public const uint Last = Count - 1;

        /// In-order reliable message channels
        public const uint Reliable0 = 50;
        public const uint Reliable1 = 51;
        public const uint Reliable2 = 52;
        public const uint Reliable3 = 53;
        public const uint Reliable4 = 54;
        public const uint Reliable5 = 55;

        /// Low-priority in-order reliable message channels
        public const uint LowPri0 = 150;
        public const uint LowPri1 = 151;
        public const uint LowPri2 = 152;
        public const uint LowPri3 = 153;
        public const uint LowPri4 = 154;
        public const uint LowPri5 = 155;

        /// Unordered (Reliable) message channel
        public const uint Unordered = 200;

        /// Unreliable (Out-of-band) message channel
        public const uint Unreliable = 255;
    }

    /// ConnectionConfig delegates
    public delegate void PTonkConnection_OnConnect(
        IntPtr context,     ///< Application-provided context pointer
        IntPtr connection   ///< Object that can be passed to tonk_*() API calls
        );
    public delegate void PTonkConnection_OnData(
        IntPtr context,     ///< Application-provided context pointer
        IntPtr connection,  ///< Object that can be passed to tonk_*() API calls
        UInt32 channel,     ///< Channel number attached to each message by sender
        IntPtr data,        ///< Pointer to a buffer containing the message data
        UInt32 bytes        ///< Number of bytes in the message
        );
    public delegate void PTonkConnection_OnTick(
        IntPtr context,     ///< Application-provided context pointer
        IntPtr connection,  ///< Object that can be passed to tonk_*() API calls
        UInt64 nowUsec      ///< Current timestamp in microseconds
        );
    public delegate void PTonkConnection_OnClose(
        IntPtr context,     ///< Application-provided context pointer
        IntPtr connection,  ///< Object that can be passed to tonk_*() API calls
        Result reason,      ///< TonkResult code indicating why the connection closed
        [MarshalAs(UnmanagedType.LPStr)] string reasonJson   ///< JSON string detailing why the connection closed
        );

    /// TonkConnection Configuration
    [StructLayout(LayoutKind.Sequential)]
    public struct ConnectionConfig
    {
        /// Application context that will be returned by the callbacks below
        public IntPtr AppContextPtr;

        /**
            [Optional] OnConnect() Callback

            Called in any case where the application has successfully connected to
            a remote peer:

            (1) tonk_connect() has successfully completed an outgoing connection.
            (2) SocketConfig::OnIncomingConnection accepted a connection.
            (3) OnP2PConnectionStart() was called and now connection completed.

            If this is not specified (=0) then connection status can be checked via
            the tonk_status() function.
        */
        public PTonkConnection_OnConnect OnConnect;

        /**
            [Optional] OnData() Callback

            Called when data arrives from a TonkConnection.

            The data pointer will be valid until the end of the function call.

            It is important not to modify any of the provided data, since that
            data is later used for FEC decoding of other data.

            It is safe to call any tonk_*() API calls during this function.
        */
        public PTonkConnection_OnData OnData;

        /**
            [Optional] OnTick() Callback

            Periodic callback for each TonkConnection.

            It is safe to call any tonk_*() API calls during this function.
        */
        public PTonkConnection_OnTick OnTick;

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
        public PTonkConnection_OnClose OnClose;
    }

    public const UInt32 TONK_NO_DATA_TIMEOUT_USEC_MIN = 2000 * 1000 /**< 2 second */;
    public const UInt32 TONK_NO_DATA_TIMEOUT_USEC_DEFAULT = 20 * 1000 * 1000 /**< 20 seconds */;
    public const UInt32 TONK_NO_DATA_TIMEOUT_USEC_MAX = 10 * 60 * 1000 * 1000 /**< 10 minutes */;

    /// Return value for OnIncomingConnection() that accepts a new connection
    public const UInt32 TONK_ACCEPT_CONNECTION = 1;

    /// Return value for OnIncomingConnection() that denies a new connection
    public const UInt32 TONK_DENY_CONNECTION = 0;

    /// Option: Enable forward error correction?
    /// This option reduces latency but will increase CPU usage, reducing the
    /// maximum number of clients that can connect at once.
    public const UInt32 TONK_FLAGS_ENABLE_FEC = 1;

    /// Option: Enable UPnP for this socket?
    /// This option is recommended if peer2peer connections are expected.
    public const UInt32 TONK_FLAGS_ENABLE_UPNP = 2;

    /// Option: Disable compression for this socket?
    /// Compression can cost a lot of extra CPU so it may be a good idea
    public const UInt32 TONK_FLAGS_DISABLE_COMPRESSION = 4;

    public delegate UInt32 PTonkSocket_OnIncomingConnection(
        IntPtr context, ///< Application context from SocketConfig
        IntPtr connection, ///< Object that can be passed to tonk_*() API calls
        ref Address address, ///< Address of the client requesting a connection
        out Tonk.ConnectionConfig configOut ///< Connection configuration
        );

    public delegate UInt32 PTonkSocket_OnP2PConnectionStart(
        IntPtr context, ///< Application context from SocketConfig
        IntPtr connection, ///< Object that can be passed to tonk_*() API calls
        ref Address address, ///< Address of the client requesting a connection
        out Tonk.ConnectionConfig configOut ///< Connection configuration
        );

    public delegate void PTonkSocket_OnAdvertisement(
        IntPtr context, ///< Application context from SocketConfig
        IntPtr tonkSocket, ///< TonkSocket
        [MarshalAs(UnmanagedType.LPStr)] string ipString,
        UInt16 port, ///< Address of the client requesting a connection
        IntPtr data, ///< Pointer to a buffer containing the message data
        UInt32 bytes ///< Number of bytes in the message
        );

    public delegate void PTonkSocket_SendToHook(
        IntPtr context, ///< Application context from SocketConfig
        UInt16 destPort, ///< Address of the client requesting a connection
        IntPtr data, ///< Pointer to a buffer containing the message data
        UInt32 bytes ///< Number of bytes in the message
        );

    /// TonkSocket Configuration
    [StructLayout(LayoutKind.Sequential)]
    public struct SocketConfig
    {
        /// Version of tonk.h - Set to TONK_VERSION
        public UInt32 Version;

        /// Application context
        public IntPtr AppContextPtr;

        /// Suggested: 0 = Match CPU core count
        public UInt32 WorkerCount;

        /// Kernel buffer sizes for UDP ports
        public UInt32 UDPSendBufferSizeBytes;
        public UInt32 UDPRecvBufferSizeBytes;

        /// Socket UDP port to listen on for connections
        /// Choose 0 for a random (client) port
        public UInt32 UDPListenPort;

        /// Client limit
        /// 0 = No incoming connections allowed
        public UInt32 MaximumClients;

        /// Maximum number of clients from the same IP address.
        /// Connections beyond this limit will be rejected.
        public UInt32 MaximumClientsPerIP;

        /// Number of connection attempts per minute before a connection flood is
        /// detected.  In response, Tonk will activate SYN cookie defenses.
        public UInt32 MinuteFloodThresh;

        /// OnTick() rate and send interval for retransmissions
        public UInt32 TimerIntervalUsec;

        /// Timeout before disconnection if no data is received
        public UInt32 NoDataTimeoutUsec;

        /// Time between client sending UDP Connection Requests
        public UInt32 UDPConnectIntervalUsec;

        /// Timeout before disconnection if connection is not achieved
        public UInt32 ConnectionTimeoutUsec;

        /// Interface network address to listen and send on.
        /// This can enable using 2+ WiFi interfaces to upload data simultaneously.
        /// Provide null (0) for all interfaces
        [MarshalAs(UnmanagedType.LPStr)] public string InterfaceAddress;

        /// Limit on how much bandwidth can be used by each connection.
        /// 0 = No limit
        public UInt32 BandwidthLimitBPS;

        /// Some combination of the TONK_FLAGS_* above.
        public UInt32 Flags;

        /**
            OnIncomingConnection() Callback

            Called when a new incoming connection completes, which was initiated by
            a remote peer on the network.

            If this function is not defined (function pointer is null) then the
            TonkSocket will not accept incoming client connections.

            Return a config with valid context pointer for this connection.
            Otherwise return config with pointer=0 to deny the connection.
        */
        public PTonkSocket_OnIncomingConnection OnIncomingConnection;

        /**
            [Optional] OnP2PConnectionStart() Callback

            Called just as a peer we are connected to has instructed us to connect
            to another peer using the tonk_p2p_connect() function.

            Unlike the other two On*Connection() functions, this function is called
            as a connection is starting rather than after it has completed.  This
            allows the application to provide data to send within the first round-
            trip of the peer2peer connection using tonk_send() in this function.

            If this function is not defined (function pointer is null) then the
            TonkSocket will not accept peer2peer connection requests.

            The connection address of the other peer is provided, which can be used
            to check if this connection should be permitted or denied.

            Return a config with valid context pointer for this connection.
            Otherwise return config with pointer=0 to deny the connection.
        */
        public PTonkSocket_OnP2PConnectionStart OnP2PConnectionStart;

        /**
            [Optional] OnAdvertisement() Callback

            This receives a message sent by tonk_advertise(), providing the source
            IP address and source port.  This function may receive between 0 and
            TONK_MAX_UNRELIABLE_BYTES bytes of data at any time until the app calls
            the tonk_socket_destroy() function.
        */
        public PTonkSocket_OnAdvertisement OnAdvertisement;

        /// SendToHook context
        public IntPtr SendToAppContextPtr;

        /**
            [Optional] SendTo() Hook

            If this is specified, then instead of sending data using Asio sockets,
            Tonk will instead call this function to send data.  The application
            will be responsible for delivering this data.

            The `destPort` will be the destination port.
            It should match the port on injected data for incoming connections
            or the port passed to tonk_connect() for outgoing connections.

            This function may be called from multiple threads simultaneously.
            The application is responisble for thread-safety.
        */
        public PTonkSocket_SendToHook SendToHook;
    }

    //------------------------------------------------------------------------------
    // TonkSocket API
    //
    // Functions to create and shutdown TonkSocket objects.

    /**
        tonk_socket_create()

        Creates a Tonk socket that can receive and initiate connections.

        SocketConfig specifies the callbacks that will be called when events
        happen on the socket.

        If SocketConfig::MaximumClients is non-zero, then the socket will
        accept new connections from remote hosts.  Note that the UDPPort should
        also be set to act as a server.

        Returns a TonkSocket object on success.
        Returns 0 on failure.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_socket_create(
        ref SocketConfig config, ///< [in] Configuration for the socket
        out IntPtr socketOut,  ///< [out] Receives detailed error message on error
        out ErrorJson errorJsonOut  ///< [out] Receives detailed error message on error
    );

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

        Returns Tonk_Success on success.
        Returns other codes on failure.
    */

    /// IP string that can be passed to tonk_advertise() to broadcast to the subnet
    public const string TONK_BROADCAST = "";

#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_advertise(
        IntPtr tonkSocket,     ///< [in] Socket to send advertisement from
        [MarshalAs(UnmanagedType.LPStr)] string ipString, ///< [in] Destination IP address
        UInt16           port, ///< [in] Destination port
        IntPtr           data, ///< [in] Message data
        UInt32           bytes ///< Number of bytes in the message
    );

    /**
        tonk_inject()

        This is an interface that enables an application to specify when and how
        data is received rather than using real sockets.  The application provides
        the source port of the datagram and its contents.

        Returns Tonk_Success on success.
        Returns other codes on failure.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_inject(
        IntPtr tonkSocket, ///< [in] Socket to shutdown
        UInt16 port, ///< [in] Destination port
        IntPtr data, ///< [in] Message data
        UInt32 bytes ///< Number of bytes in the message
    );

    /**
        tonk_socket_destroy()

        Destroy the socket object, shutting down background threads and freeing
        system sources.  This function can be called from any callback.  It will
        complete asynchronously in the background.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern void tonk_socket_destroy(
        IntPtr tonkSocket, ///< [in] Socket to shutdown
        UInt32 shouldBlock ///< [in] Should block?
    );

    /**
        tonk_sockets_alive()

        This returns the number of active sockets remaining.

        If tonk_socket_destroy() was called without blocking, then this will return
        zero after the socket has been destroyed in the background.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern UInt64 tonk_sockets_alive();


    //------------------------------------------------------------------------------
    // TonkConnection API

    /**
        tonk_connect()

        Connect to remote host asynchronously.

        The returned TonkConnection can be used with tonk_send() to send data in the
        first connection datagram before a connection request has been sent.

        The first connection request is sent from a background thread on the next
        timer tick or when tonk_flush() is called.

        Optional ErrorJson parameter receives detailed error messages.

        Returns a TonkConnection object to use in further API calls,
        Returns 0 on error.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_connect(
        IntPtr tonkSocket,           ///< [in] Socket to connect from
        ConnectionConfig config, ///< [in] Configuration for the connection
        [MarshalAs(UnmanagedType.LPStr)] string hostname,             ///< [in] Hostname of the remote host
        UInt16 port,                 ///< [in] Port for the remote host
        out IntPtr connectionOut,    ///< [out] Connection returned
        out ErrorJson errorJsonOut   ///< [out] Receives detailed error message on error
    );

    /**
        tonk_status()

        Check status of the connection.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_status(
        IntPtr connection,           ///< [in] Connection to query
        out Status statusOut    ///< [out] Connection status
    );

    /**
        tonk_status_ex()

        Check extended status of the connection.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_status_ex(
        IntPtr connection,           ///< [in] Connection to query
        out StatusEx statusExOut ///< [out] Extended connection status
    );

    /**
        tonk_send()

        Send datagram on the given channel.

        This function can fail if the message is too large.
        Call tonk_status() to retrieve the maximum send size.

        Returns Tonk_Success on success.
        Returns other TonkResult codes on error.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_send(
        IntPtr connection,  ///< [in] Connection to send on
        IntPtr data,        ///< [in] Pointer to message data
        UInt64 bytes,       ///< [in] Message bytes
        UInt32 channel      ///< [in] Channel to attach to message
    );

    /**
        tonk_flush()

        Flush pending datagrams.

        This will send data immediately instead of waiting for more data to be sent.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern void tonk_flush(
        IntPtr connection  ///< [in] Connection to flush
    );

    /**
        tonk_close()

        Asynchronously start closing the connection.

        The TonkConnection object is still alive and it is valid to call any of the
        normal API calls.  Those calls will start failing rather than adding more
        data to the outgoing queue.

        Callbacks such as OnTick() and OnData() will still be generated.
        OnClose() will definitely be called at some point after tonk_close(),
        and it will be the last callback received.  OnClose() is an excellent place
        to call tonk_free() to release object memory.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern void tonk_close(
        IntPtr connection  ///< [in] Connection to close
    );

    /**
        tonk_free()

        Free the TonkConnection object memory.

        This informs the Tonk library that the application will no longer reference
        a TonkConnection object.

        If the connection is alive, then it will continue to receive OnTick() and
        OnData() callbacks until the OnClose() callback completes.  To ensure that
        the session is disconnected, call tonk_close() and then wait for the
        OnClose() callback to be received.

        It is recommended to call tonk_free() from OnClose() or after OnClose().

        This function can also be called from the destructor of the application's
        UserContext object.  To support broadcasts of data from other users it is
        often useful to have a reference-counted UserContext.  Tonk supports this
        by allowing the application to keep the TonkConnection object alive a bit
        longer than the OnClose() callback.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern void tonk_free(
        IntPtr connection  ///< [in] Connection to free
    );


    //------------------------------------------------------------------------------
    // Time Synchronization API
    //
    // These functions allow for generation and transformation of timestamps between
    // local and remote hosts.

    /// Get current time in microseconds
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern UInt64 tonk_time(); ///< Returns in microseconds

    /**
        The time synchronization protocol uses an LSB of 8 microseconds.
        For a 24-bit timestamp centered around 0:

            8 usec * 2^24 / 2 = 67,108,864 usec ahead & behind

        This means that timeouts must be capped at 67 seconds in the protocol or
        else there will be wrap-around.  As a side-effect of the time synchronization
        math, we lose one bit of data, so the timestamps sent by users are truncated
        to just 23 bits.

        We can bias the value forward since timestamps in the past are more useful:

            8 usec * 2^23 / 2 - 16777216 = 16,777,216 usec ahead (TONK_23_MAX_FUTURE_USEC)
            8 usec * 2^23 / 2 + 16777216 = 50,331,648 usec behind (TONK_23_MAX_PAST_USEC)

        --> This is the range represented by the 23-bit timestamp API.

        For ease of use, 16-bit timestamps are available too.

        For a 16-bit timestamp centered around 0, we choose LSB = 512 usec:

            512 usec * 2^16 / 2 = 16,777,216 usec ahead & behind

        We can bias the value forward since timestamps in the past are more useful:

            512 usec * 2^16 / 2 - 8388608 = 8,388,608 usec ahead (TONK_16_MAX_FUTURE_USEC)
            512 usec * 2^16 / 2 + 8388608 = 25,165,824 usec behind (TONK_16_MAX_PAST_USEC)

        --> This is the 16-bit biased timestamp format we use.
    */

    /// 16-bit general purpose timestamps with half-millisecond resolution:
    public const UInt32 TONK_TIME16_MAX_FUTURE_USEC = 8388607 /**< 8 seconds */;
    public const UInt32 TONK_TIME16_MAX_PAST_USEC = 25165824 /**< 25 seconds */;
    public const UInt32 TONK_TIME16_LSB_USEC = 512 /**< microseconds LSB */;

    /**
        tonk_to_remote_time_16()

        Returns 16-bit remote timestamp from a local timestamp in microseconds.

        This is a timestamp that is expected to be in the remote host's time.
        It becomes invalid after 25 seconds.  The precision is 512 microseconds.
        See the TONK_TIME16 constants above.

        Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
        as the 'localUsec' parameter.

        Note that if tonk_status() TonkStatus::Flags does not have the
        TimeSync bit set then the result is undefined.

        Returns the converted 16-bit network timestamp.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern UInt16 tonk_to_remote_time_16(
        IntPtr connection, ///< [in] Connection with remote host
        UInt64 localUsec  ///< [in] Local timestamp in microseconds
    );

    /**
        tonk_from_local_time_16()

        Returns local time given 16-bit local timestamp from a message
        in the OnData() callback.

        Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
        as the 'localUsec' parameter.

        Returns the timestamp in local time in microseconds.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern UInt64 tonk_from_local_time_16(
        IntPtr connection, ///< [in] Connection with remote host
        UInt16 networkTimestamp16  ///< [in] Local timestamp to interpret
    );

    /// 23-bit general purpose timestamps with 8-microsecond resolution:
    public const UInt32 TONK_TIME23_MAX_FUTURE_USEC = 16777215 /**< 16 seconds */;
    public const UInt32 TONK_TIME23_MAX_PAST_USEC = 50331648 /**< 50 seconds */;
    public const UInt32 TONK_TIME23_LSB_USEC = 8 /**< microseconds LSB */;

    /**
        tonk_to_remote_time_23()

        Returns 23-bit remote timestamp from a local timestamp in microseconds.

        It is recommended to serialize this value into 3 instead of 4 bytes.

        This is a timestamp that is expected to be in the remote host's time.
        It becomes invalid after 50 seconds.  The precision is 8 microseconds.
        See the TONK_TIME23 constants above.

        Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
        as the 'localUsec' parameter.

        Note that if tonk_status() TonkStatus::Flags does not have the
        TimeSync bit set then the result is undefined.

        Returns the converted 23-bit network timestamp.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern UInt32 tonk_to_remote_time_23(
        IntPtr connection, ///< [in] Connection with remote host
        UInt64 localUsec  ///< [in] Local timestamp in microseconds
    );

    /**
        tonk_from_local_time_23()

        Returns local time given 23-bit local timestamp from a message
        in the OnData() callback.

        Use tonk_time() or 'nowUsec' from the OnTick() or OnData() callbacks
        as the 'localUsec' parameter.

        Returns the timestamp in local time in microseconds.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern UInt64 tonk_from_local_time_23(
        IntPtr connection, ///< [in] Connection with remote host
        UInt32 networkTimestamp23  ///< [in] Local timestamp to interpret
    );


    //------------------------------------------------------------------------------
    // Peer2Peer

    /**
        tonk_p2p_connect()

        Instruct two peers to connect with eachother using a custom NAT-traversal
        protocol described in HolePunch.README.txt.  It works when one side is on
        a friendly ISP like residential Comcast, but often fails if both sides are
        on e.g. corporate or phone connections.

        If either Alice or Bob are not time-synchronized, the function will fail and
        it will return Tonk_InvalidInput.

        Returns Tonk_Success on success.
        Returns other TonkResult codes on error.
    */
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_p2p_connect(
        IntPtr aliceConnection, ///< [in] Peer 1 connection
        IntPtr bobConnection    ///< [in] Peer 2 connection
    );


    //------------------------------------------------------------------------------
    // Extra DLL Exports

    /// Fill in socket configuration defaults.
    /// Provide the TONK_VERSION in the first argument.
    /// This is useful when calling from other languages that do not support default
    /// values for structures (for example: C#)
#if UNITY_IPHONE && !UNITY_EDITOR
    [DllImport("__Internal")]
#else
    [DllImport("tonk", CallingConvention = CallingConvention.Cdecl)]
#endif
    [System.Security.SuppressUnmanagedCodeSecurityAttribute()]
    public static extern Result tonk_set_default_socket_config(
        UInt32 tonk_version,
        out SocketConfig configOut);


    //------------------------------------------------------------------------------
    // Serialization Tools

    public static unsafe UInt16 ReadU16_LE(byte* data)
    {
        return *(ushort*)data;
    }
    public static unsafe UInt32 ReadU24_LE(byte* data)
    {
        return ((uint)data[2] << 16) | ((uint)data[1] << 8) | data[0];
    }
    public static unsafe UInt32 ReadU32_LE(byte* data)
    {
        return *(uint*)data;
    }
    public static unsafe UInt64 ReadU64_LE(byte* data)
    {
        return *(ulong*)data;
    }

    public static unsafe void WriteU16_LE(byte* data, UInt16 value)
    {
        *(ushort*)data = value;
    }
    public static unsafe void WriteU24_LE(byte* data, UInt32 value)
    {
        data[2] = (byte)(value >> 16);
        data[1] = (byte)(value >> 8);
        data[0] = (byte)value;
    }
    public static unsafe void WriteU32_LE(byte* data, UInt32 value)
    {
        *(uint*)data = value;
    }
    public static unsafe void WriteU64_LE(byte* data, UInt64 value)
    {
        *(ulong*)data = value;
    }


    //------------------------------------------------------------------------------
    // DLL Loading Methods

    /**
     * There are a few ways to get the Tonk DLL to load when using the SDK:
     * (1) Build for one platform (32-bit or 64-bit) and include the appropriate
     *     pre-compiled tonk.dll with your application.
     * (2) Invoke OverrideDLLDirectory() to override the DLL search path for all
     *     DLLs to be /x86/ or /x86_64/ next to your application EXE.
     * (3) Hook System.AppDomain.CurrentDomain.AssemblyResolve as in
     * https://stackoverflow.com/questions/108971/using-side-by-side-assemblies-to-load-the-x64-or-x32-version-of-a-dll/156024#156024
     */

    public static void OverrideDLLDirectory()
    {
        string path = System.IO.Path.GetDirectoryName(System.Reflection.Assembly.GetEntryAssembly().Location);
        path = System.IO.Path.Combine(path, IntPtr.Size == 8 ? "x86_64" : "x86");
        SetDllDirectory(path);
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern bool SetDllDirectory(string path);

    /// Replace tonk.dll with TonkDebug.dll, enabling debug of native code
    public static void ReplaceWithDebugDll()
    {
        string path = System.IO.Path.GetDirectoryName(System.Reflection.Assembly.GetEntryAssembly().Location);
        path = System.IO.Path.Combine(path, IntPtr.Size == 8 ? "x86_64" : "x86");

        string debugDllPath = System.IO.Path.Combine(path, "tonk_debug.dll");
        if (System.IO.File.Exists(debugDllPath))
        {
            string releaseDllPath = System.IO.Path.Combine(path, "tonk.dll");
            string oldDllPath = System.IO.Path.Combine(path, "tonk.dll.old");

            System.IO.File.Replace(debugDllPath, releaseDllPath, oldDllPath);
        }
    }
}
