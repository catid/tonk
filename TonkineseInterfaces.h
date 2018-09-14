/** \file
    \brief Tonk Implementation: Class Interfaces
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

#include "TonkineseTools.h"
#include "TonkineseProtocol.h"

namespace tonk {


//------------------------------------------------------------------------------
// IIncomingToOutgoing

/**
    This interface defines the functions needed from SessionOutgoing from the
    SessionIncoming object.
*/
class IIncomingToOutgoing
{
public:
    virtual ~IIncomingToOutgoing() = default;

    /// Queue a message for sending.  This only supports message types that will
    /// not split into multiple datagrams, so it excludes Reliable/LowPri types
    virtual Result QueueUnreliable(
        protocol::MessageType messageType,
        const uint8_t* data,
        size_t bytes) = 0;

    /// Queue an unordered reliable message for sending.  Does not split messages
    virtual Result QueueUnordered(
        const uint8_t* data,
        size_t bytes) = 0;

    /// Queue a Control reliable message for sending.  Does not split messages
    virtual Result QueueControl(
        const uint8_t* data,
        size_t bytes) = 0;

    /// Queue a reliable message for sending.  This will split up messages
    virtual Result QueueReliable(
        unsigned messageType,
        const uint8_t* data,
        size_t bytes) = 0;

    /// An acknowledgement message has been received (pass to SessionOutgoing)
    virtual Result OnAcknowledgements(
        const uint8_t* data,
        unsigned bytes,
        Counter64 ackNonce,
        uint64_t receiveUsec) = 0;
};


//------------------------------------------------------------------------------
// IIncomingHandler

/// Result of IIncomingHandler::OnUnauthenticatedHandshake()
enum class HandshakeResult
{
    DropDatagram, // Ignore the rest of this datagram
    ContinueProcessing // Continue processing this datagram
};

/**
    IIncomingHandler can be implemented by another object to receive callbacks
    when datagrams/messages are received that cannot be handled immediately by
    the SessionIncoming object.
*/
class IIncomingHandler
{
public:
    virtual ~IIncomingHandler() = default;

    /// Handle an unauthenticated handshake field from a datagram
    /// This happens prior to all other callbacks
    virtual HandshakeResult OnUnauthenticatedHandshake(const uint8_t* data, const unsigned bytes) = 0;

    /**
        OnAuthenticatedDatagram()

        This is called if the datagram decrypted successfully but before
        processing any contained messages.  This is used to tweak some state
        of the Tonk library before calling the OnData() callbacks for the
        application for a few reasons:

        + Reset no-data timers before a long application operation
        + Mark a connection as established and invoke OnConnect()
        + Stop ConnectionId from being attached to outgoing data
        + And a few more..
    */
    virtual void OnAuthenticatedDatagram(const UDPAddress& addr, uint64_t receiveUsec) = 0;

    /// Reliable, Unreliable, and Recovery data are handled by SessionIncoming.
    /// Other message types are handled outside of SessionIncoming:

    /// A control channel message has been received internal to Tonk
    virtual Result OnControlChannel(const uint8_t* data, unsigned bytes) = 0;

    /// A time synchronization message containing received bandwidth and other
    /// information from the perspective of the remote peer has been received
    virtual Result OnTimeSync(const uint8_t* data, unsigned bytes) = 0;
};


//------------------------------------------------------------------------------
// UDP Socket Send Interface

/**
    This is an interface to sending UDP datagrams.
*/
class IUDPSender
{
public:
    virtual ~IUDPSender() = default;

    /**
        Send()

        Send a UDP datagram.

        This function takes a long time to complete so should be done from a
        background thread where possible.

        The refCounter must be incremented prior to the call, and then it
        will be decremented after the sendto() operation completes and
        all parameters are out of scope.
    */
    virtual void Send(
        const UDPAddress& destAddress,  ///< [in] Destination address for send
        DatagramBuffer       datagram,  ///< [in] Buffer to send
        RefCounter*        refCounter   ///< [in] Reference count to decrement when done
    ) = 0;
};


//------------------------------------------------------------------------------
// NAT Port Pool

/// Special socket index to pass to SendNATProbe to send from MainSocket
static const unsigned kNATPortMainSocketIndex = 65536;

/**
    This is an interface to the NAT Traversal Port Pool object on ApplicationSession.
*/
class INATPortPool
{
public:
    virtual ~INATPortPool() = default;

    /// Start a P2P connection
    virtual void OnP2PStartConnect(
        uint16_t myRendezvousVisibleSourcePort,
        uint32_t rendezvousServerConnectionId,
        const protocol::P2PConnectParams& params
    ) = 0;

    /// Send a P2P probe
    virtual void SendNATProbe(
        unsigned socketIndex, ///< or kNATPortMainSocketIndex
        uint8_t* probeData,
        const UDPAddress& destAddress,
        RefCounter* refCounter
    ) = 0;
};


//------------------------------------------------------------------------------
// Connection Interface

/**
    This is an interface for the Connection object from the connection maps.
*/
class IConnection
{
public:
    virtual ~IConnection() = default;

    /// Reference count for this Connection object
    RefCounter SelfRefCount;
};


} // namespace tonk
