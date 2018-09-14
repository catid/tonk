/** \file
    \brief NAT Traversal using Internet Gateway Protocol
    \copyright Copyright (c) 2018 Christopher A. Taylor.  All rights reserved.

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

#pragma once

#include "thirdparty/IncludeAsio.h"
#include "TonkineseTools.h"
#include "TonkineseProtocol.h"

#include <cstdint>
#include <vector>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <atomic>

namespace tonk {
namespace gateway {


//------------------------------------------------------------------------------
// Application Request

struct MappedPortLifetime;

enum class RequestType
{
    Add,    ///< Add a port mapping
    Remove  ///< Remove a port mapping
};

/// Describes an application request to modify a port mapping
struct AppRequest
{
    /// Local port to map to an external port.
    /// We try to map the same number but may need to use a random number
    uint16_t LocalPort = 0;

    /// Request type
    RequestType Type;

    /// Pointer to the app Lifetime object.
    /// Must always be accessed via API_Requests.
    /// Protected by API_Lock
    MappedPortLifetime* API_LifetimePtr = nullptr;
};


//------------------------------------------------------------------------------
// GatewayPortMapper : API

/**
    The MappedPortLifetime serves several purposes:

    (1) It tracks the lifetime of a request to map a port.  When it goes out of
    scope, that port map request is changed to remove the mapping.

    (2) It tracks the usages of the subsystem.  When all port mappers go out of
    scope, the subsystem ends its background thread.

    (3) When the port mapping succeeds, the ExternalMappedPort member will be
    updated with the port that was mapped, which may be a different one than
    the requested port if the original requested port was unavailable.
*/
struct MappedPortLifetime
{
    /// Copy of the original request
    AppRequest Request;

    /// Updated from a background thread when the external mapped port changes
    std::atomic<uint16_t> ExternalMappedPort = ATOMIC_VAR_INIT(0);

    ~MappedPortLifetime();
};

/**
    RequestPortMap()

    This exposes the Tonk port on this computer to the Internet, enabling two
    clients to connect to eachother directly rather than being relayed through
    a server.  So long as at least one client is successful, both can connect.

    It starts a background worker thread that looks for UPnP-enabled gateways
    on the Local Area Network (LAN) using SSDP.  It then looks for a control
    URL that supports the SOAP AddPortMapping call.  The call is then made to
    add a mapping from the current computer on the LAN to an external port
    on the uplink side of the router.

    Parallel RequestPortMap() requests are supported, and a minimal number of
    network requests will be made.  All HTTP requests on the LAN are performed
    in parallel as well.  Requests are retried several times if needed.

    The requested port may not be available, so it retries with some random
    other port numbers.  If a port map request eventually succeeds, then the
    port that was mapped will be reported.

    This function returns a MappedPortLifetime object on success.
    Allow the object to go out of scope to remove the port mapping.

    On failure it returns nullptr.
*/
std::shared_ptr<MappedPortLifetime> RequestPortMap(
    uint16_t localPort,
    const char* interfaceAddress = nullptr);


//------------------------------------------------------------------------------
// OS-Specific Gateway Address Query

struct LANInfo
{
    // is_unspecified() will return true if these could not be retrieved
    asio::ip::address Gateway;
    asio::ip::address Localhost;
};

/**
    UpdateLANInfo()

    Update the given LANInfo struct to contain the new LAN gateway and localhost
    IP address.  Sets the updated flag to true if the route info has changed to
    a new valid route, and false if the route info is not available yet.

    This function is essential for UPnP to work because the client needs to know
    its own IP address in order to make the request, so it must be ported to
    each operating system that is supported.

    Returns a Result object that indicates success or a failure reason.
*/
Result UpdateLANInfo(LANInfo& info, bool& updated);


//------------------------------------------------------------------------------
// URL Helper

/// Parsed URL
struct ParsedURL
{
    /// Full URL (neglecting protocol - no http:// prefix)
    std::string FullURL;

    /// Resource path extracted from FullURL
    std::string XMLPath;

    /// IP extracted from FullURL (assumes numeric address)
    std::string IP;

    /// Port extracted from FullURL (assumes 80 if not present)
    uint16_t Port;

    /// Converted IP address
    asio::ip::address IPAddr;

    /// TCP endpoint with port included
    asio::ip::tcp::endpoint TCPEndpoint;

    /// Parse URL string
    Result Parse(const std::string& url);

    TONK_FORCE_INLINE bool operator==(const ParsedURL& other) const
    {
        return 0 == StrCaseCompare(other.FullURL.c_str(), FullURL.c_str());
    }
};


//------------------------------------------------------------------------------
// XML Helper

/// Control URL from XML
struct ParsedXML
{
    // Parse result for new URL e.g. "192.168.1.1:5000"
    std::string URLBase;

    // Parse result for new service type e.g.
    // "urn:schemas-upnp-org:service:WANIPConnection:2"
    std::string ServiceType;

    // Parse result for new control endpoint URL e.g. "/ctl/IPConn"
    std::string ControlURL;


    /// Parse the given data buffer into service type, control URL, and URLBase
    Result Parse(const char* data, unsigned bytes);
};


//------------------------------------------------------------------------------
// SSDP - Gateway Discovery

struct StateMachine;
struct AsioHost;

/// Makes SSDP requests
struct SSDPRequester
{
    /// State
    StateMachine* State = nullptr;

    /// Host
    AsioHost* Host = nullptr;

    /// LAN info
    LANInfo LAN;

    /// Have seen a gateway address before?
    bool HaveSeenAGatewayAddress = false;

    /// Is the network definitely down?
    inline bool IsNetworkDefinitelyDown() const
    {
        // If we have never seen a gateway address:
        if (!HaveSeenAGatewayAddress) {
            // Perhaps the LANInfo query is broken?
            return true;
        }

        // Otherwise: Network is down if we have no gateway
        return LAN.Gateway.is_unspecified();
    }

    /// Last time when LANInfo was requested
    uint64_t LastLANInfoRequestUsec = 0;

    /// UDP socket used for multicasting SSDP request
    std::unique_ptr<asio::ip::udp::socket> Socket_UDP;

    /// Address associated with the last packet we received
    asio::ip::udp::endpoint SourceAddress;

    /// Addresses to send SSDP requests to
    std::vector<asio::ip::udp::endpoint> SSDPAddresses;

    /// Size of read buffer in bytes
    static const int kReadBufferBytes = protocol::kMaxPossibleDatagramByteLimit;

    /// Buffer that receives UDP data
    uint8_t ReadBuffer_UDP[kReadBufferBytes];

    /// Number of multicast retries remaining
    int AttemptsRemaining = 0;

    /// Do we still ened to call initializeSockets()?
    bool NeedsInitialization = true;


    /// Begin searching for gateways
    void BeginSearch()
    {
        AttemptsRemaining = protocol::kPortMappingRetries;
    }

    /// End searching for gateways
    void EndSearch()
    {
        AttemptsRemaining = 0;
    }

    /// Tick event
    void OnTick();

    /// Poll to check if LAN info has updated
    void PollLANInfo();

    /// Initialize UDP/TCP sockets
    Result InitializeSockets();

    /// Post next UDP socket read
    void PostNextRead_UDP();

    /// Handle a UDP datagram
    Result OnUDPDatagram(char* data, unsigned bytes);

    /// Multicast an SSDP request to discover gateways
    void RequestSSDP();
};


//------------------------------------------------------------------------------
// Streamlined HTTP Client

using HTTPCallback = std::function<void(
    const Result& result,
    unsigned httpResponseCode,
    const char* response,
    unsigned bytes)>;

 /// Parallel HTTP requests
class HTTPRequester
{ 
public:
    /// Make next HTTP request.
    /// This may fail right away or via the callback later
    Result Begin(
        const ParsedURL& url,
        AsioHost* host,
        const std::string& request,
        const HTTPCallback& callback);

    /// Called from a timer tick: Can this object be disposed?
    bool OnTick_IsDisposable(uint64_t nowUsec);

protected:
    /// Maximum content length allowed
    static const int kContentLengthMax = 256000;

    /// Timeout interval for connection
    static const uint64_t kTimeoutUsec = 15 * 1000000;

    /// Number of bytes to read in body at a time
    static const unsigned kBodyReadCount = 4000;


    /// URL
    ParsedURL URL;

    /// Host
    AsioHost* Host = nullptr;

    /// Request
    std::string Request;

    /// Callback
    HTTPCallback Callback;

    /// TCP socket used for configuration changes
    std::unique_ptr<asio::ip::tcp::socket> Socket_TCP;

    /// Stream buffer for incoming data on the TCP socket
    asio::streambuf TCPStreamBuf;

    /// Body content
    std::vector<uint8_t> BodyContent;

    /// Buffer read size
    size_t ContentLength = 0;

    /// Status code read from HTTP response header
    unsigned HTTPStatusCode = 0;

    /// Control TCP socket used for configuration changes
    std::unique_ptr<asio::ip::tcp::socket> Socket_TCP_Control;

    /// Stream buffer for incoming data on the control TCP socket
    std::unique_ptr<asio::streambuf> TCPStreamBuf_Control;

    /// Number of asynchrnous io operations in progress
    int AsyncRefs = 0;

    /// Connection start time
    uint64_t StartUsec = 0;

    /// Has the callback been called?
    bool Done = false;


    /// Post next TCP socket read
    void ContinueReadingBody();

    /// Call callback
    void OnDone(
        const Result& result,
        unsigned httpResponseCode = 0,
        const char* response = nullptr,
        unsigned bytes = 0);
};


//------------------------------------------------------------------------------
// Asio Host

/// Hosts Asio timer and context
struct AsioHost
{
    /// State
    StateMachine* State = nullptr;

    /// SSDP subsystem
    SSDPRequester* SSDP = nullptr;

    /// Asio context
    std::unique_ptr<asio::io_context> Context;

    /// Timer for retries and timeouts (and to avoid using 100% CPU)
    std::unique_ptr<asio::steady_timer> Ticker;

    /// Thread that processes Asio events
    std::unique_ptr<std::thread> AsioThread;

    /// Strand that serializes read, write, and tick events
    std::unique_ptr<asio::io_context::strand> AsioEventStrand;

    /// This is set during Shutdown() to cause the thread to stop
    std::atomic<bool> Terminated = ATOMIC_VAR_INIT(false);


    /// Initialize
    Result Initialize();

    /// Worker loop for AsioThread
    void WorkerLoop();

    /// Run OnTick() for state machine near-immediately in background thread
    void DispatchTick();

    /// Call OnTick() and post a timer tick event
    void TickAndPostNextTimer();

    /// Clean up background thread
    void Shutdown();
};


//------------------------------------------------------------------------------
// State Machine

/// Device in result list from discovery with SSDP
struct GatewayEndpoint
{
    /// URL to contact
    ParsedURL URL;

    /// Retries remaining.  If this is 0, it may also have been successful
    int RetriesRemaining = protocol::kPortMappingRetries;

    /// In progress requester
    std::shared_ptr<HTTPRequester> Requester;

    /// Is the query completed?  This is reset when LAN changes
    bool Completed = false;
};

/// Request to control endpoint state
struct ControlRequest
{
    /// Last request made by the application
    RequestType LastApp = RequestType::Remove;

    /// Retries remaining
    int RetriesRemaining = protocol::kPortMappingRetries;

    /// In progress requester
    std::shared_ptr<HTTPRequester> Requester;

    /// External port we are going to attempt to add
    bool FailedRequestedPort = false;

    /// Which port was added?  0 = None
    uint16_t AddedExternalPort = 0;

    /// Should the mapping be re-added again?
    bool AddAgainOnTimer = false;

    /// Time since port mapping was added
    uint64_t LastSuccessUsec = 0;
};

/// Control URL endpoint
struct ControlEndpoint
{
    /// URL for request
    ParsedURL URL;

    /// Control endpoint descriptor XML
    ParsedXML XML;

    /// Requests for this control endpoint
    std::vector<ControlRequest> Requests;

    /// Is this endpoint still active?
    bool EndpointActive = true;
};

/// State machine
struct StateMachine
{
    /// Host
    AsioHost* Host = nullptr;

    /// SSDP
    SSDPRequester* SSDP = nullptr;

    /// Lock to guard the public API
    std::mutex API_Lock;

    /// Is the requests vector updated?
    std::atomic<bool> API_Requests_Updated = ATOMIC_VAR_INIT(false);

    /// Requests made by the application.  Protected by RequestsLock
    std::vector<AppRequest> API_Requests;

    /// Request made by the application.  Mirror owned by background thread
    std::vector<AppRequest> Requests;

    /// List of discovered gateway endpoints
    std::vector<GatewayEndpoint> GatewayEndpoints;

    /// List of discovered control endpoints
    std::vector<ControlEndpoint> ControlEndpoints;

    /// Last interface address provided
    std::string InterfaceAddress;

    /// Random number generator
    siamese::PCGRandom Rand;


    /// Initialize
    void Initialize();

    /// Make request to add a port mapping
    void API_Request(const AppRequest& request);

    /// Copy requests from public vector to control endpoint requests
    void API_CopyRequests();

    /// Handle timer tick
    void OnTick();

    /// Handle SSDP response
    void OnSSDPResponse(const ParsedURL& url);

    /// Handle XML response
    void OnXMLResponse(const ParsedURL& sourceUrl, const ParsedXML& xml);

    /// Begin an XML request from a gateway endpoint
    void BeginGatewayXMLRequest(int index);

    /// Begin any SOAP requests that are needed
    void BeginControlSOAPRequests(int controlIndex);

    /// Make a control request
    void RequestSOAP(int controlIndex, int requestIndex);

    /// Called when the LAN changes
    void OnLANChange();
};


} // namespace gateway
} // namespace tonk
