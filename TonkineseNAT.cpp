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

#include "TonkineseNAT.h"

#ifdef _WIN32
#include <Iphlpapi.h>
#include <Ws2tcpip.h>
#pragma comment(lib, "Iphlpapi.lib")
#endif

#ifdef __linux__
#include <sys/types.h>
#include <ifaddrs.h>
#endif

#ifdef __APPLE__
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <net/route.h>
// FIXME: Port to OSX
#endif


namespace tonk {
namespace gateway {

static logger::Channel ModuleLogger("GatewayPortMapper", MinimumLogLevel);


//------------------------------------------------------------------------------
// Constants

/// IPv4 address at which to multicast SSDP requests
static const char* kUPnPMulticastAddress = "239.255.255.250";

/// Port for SSDP multicast
static const uint16_t kPortSSDP = 1900;

#define SSDP_REQUEST(ST) \
    "M-SEARCH * HTTP/1.1\r\n" \
    "HOST: 239.255.255.250:1900\r\n" \
    "ST: " ST "\r\n" \
    "MAN: \"ssdp:discover\"\r\n" \
    "MX: 2\r\n" \
    "\r\n"

/// List of SSDP requests to make for each multicast
static const char* const kSSDPRequestList[] = {
    //SSDP_REQUEST("urn:schemas-upnp-org:device:InternetGatewayDevice:2"),
    //SSDP_REQUEST("urn:schemas-upnp-org:service:WANIPConnection:2"),
    SSDP_REQUEST("urn:schemas-upnp-org:device:InternetGatewayDevice:1"),
    SSDP_REQUEST("urn:schemas-upnp-org:service:WANIPConnection:1"),
    SSDP_REQUEST("urn:schemas-upnp-org:service:WANPPPConnection:1"),
    //SSDP_REQUEST("upnp:rootdevice")
};


//------------------------------------------------------------------------------
// GatewayPortMapper : API

struct GatewayPortMapper
{
    std::unique_ptr<AsioHost> Host;
    std::unique_ptr<SSDPRequester> SSDP;
    std::unique_ptr<StateMachine> State;

    /// Initialize objects
    Result Initialize(const char* interfaceAddress);

    /// Clean up background thread
    void Shutdown();
};

// Lock to guard singleton pattern and parallel requests
static std::mutex APILock;

// Number of API references outstanding before PortMapper is killed
static int API_References = 0;

// Singleton object instance
static GatewayPortMapper* m_PortMapper = nullptr;

static void Delete_PortMapper()
{
    if (m_PortMapper != nullptr)
    {
        m_PortMapper->Shutdown();
        delete m_PortMapper;
        m_PortMapper = nullptr;
    }
    API_References = 0;
}

MappedPortLifetime::~MappedPortLifetime()
{
    std::lock_guard<std::mutex> locker(APILock);

    // If there are no more references:
    TONK_DEBUG_ASSERT(API_References > 0);
    if (--API_References <= 0)
    {
        Delete_PortMapper();

        // TBD: Should we bother unmapping ports before termination?
        // Currently not doing this because it would delay shutdown.
        return;
    }

    // Otherwise take the time to request removing port mapping:
    TONK_DEBUG_ASSERT(m_PortMapper);
    if (m_PortMapper)
    {
        // Flip type to Remove
        Request.Type = RequestType::Remove;

        // Clear the lifetime pointer
        Request.API_LifetimePtr = nullptr;

        // Thread-safe API request from object
        m_PortMapper->State->API_Request(Request);
    }
}

std::shared_ptr<MappedPortLifetime> RequestPortMap(
    uint16_t localPort,
    const char* interfaceAddress)
{
    std::lock_guard<std::mutex> locker(APILock);

    // If initialization is needed:
    TONK_DEBUG_ASSERT(API_References >= 0);
    if (API_References <= 0)
    {
        TONK_DEBUG_ASSERT(m_PortMapper == nullptr);
        Delete_PortMapper();

        // Allocate port mapper
        m_PortMapper = new(std::nothrow) GatewayPortMapper;
        if (!m_PortMapper) {
            return nullptr; // OOM
        }

        // Initialize background thread and Asio objects
        Result result = m_PortMapper->Initialize(interfaceAddress);
        if (result.IsFail())
        {
            ModuleLogger.Error("Unable to initialize port mapper: ", result.ToJson());
            Delete_PortMapper();
            return nullptr;
        }
    }

    // Create a request lifetime object
    std::shared_ptr<MappedPortLifetime> ref = MakeSharedNoThrow<MappedPortLifetime>();
    if (!ref) {
        return nullptr; // OOM
    }

    // Put in request
    TONK_DEBUG_ASSERT(m_PortMapper);
    if (m_PortMapper)
    {
        AppRequest request;
        request.LocalPort = localPort;
        request.Type = RequestType::Add;
        request.API_LifetimePtr = ref.get();

        ref->Request = request;

        // Insert request
        m_PortMapper->State->API_Request(request);
    }

    // Increment references to API
    ++API_References;

    return ref;
}

Result GatewayPortMapper::Initialize(const char* interfaceAddress)
{
    TONK_DEBUG_ASSERT(!Host);

    Host = MakeUniqueNoThrow<AsioHost>();
    SSDP = MakeUniqueNoThrow<SSDPRequester>();
    State = MakeUniqueNoThrow<StateMachine>();

    // TBD: If two sessions specify different interface addresses,
    // we currently do not bind properly
    if (interfaceAddress) {
        State->InterfaceAddress = interfaceAddress;
    }

    SSDP->Host = Host.get();
    SSDP->State = State.get();
    State->Host = Host.get();
    State->SSDP = SSDP.get();
    Host->State = State.get();
    Host->SSDP = SSDP.get();

    // Start searching for gateways using SSDP
    SSDP->BeginSearch();

    // Initialize state
    State->Initialize();

    // Initialize Asio
    return Host->Initialize();
}

void GatewayPortMapper::Shutdown()
{
    Host->Shutdown();

    SSDP = nullptr;
    State = nullptr;
    Host = nullptr;
}


//------------------------------------------------------------------------------
// OS-Specific Gateway Address Query

#ifdef _WIN32

// Windows version
Result GetLANInfo(LANInfo& info)
{
    info.Gateway = asio::ip::address();
    info.Localhost = asio::ip::address();

    // Note: When the computer is offline this keeps returning the last valid
    // route, so it is not useful for detecting when the computer goes offline.
    // This function only fails if no route ever existed.
    MIB_IPFORWARDROW row{};
    const DWORD bestResult = ::GetBestRoute(
        INADDR_ANY, // source address
        INADDR_ANY, // dest address
        &row); // result row
    if (bestResult != NO_ERROR) {
        return Result("UpdateLANInfo", "GetBestRoute failed", ErrorType::Asio, bestResult);
    }
    else {
        info.Gateway = asio::ip::address_v4(htonl(row.dwForwardNextHop));
    }

    const ULONG kFamily = AF_INET; // IPv4 only
    const ULONG kFlags = /*GAA_FLAG_SKIP_UNICAST |*/ GAA_FLAG_SKIP_ANYCAST | \
        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_SKIP_FRIENDLY_NAME;

    ULONG size = 0;
    const DWORD sizeResult = ::GetAdaptersAddresses(
        kFamily,
        kFlags,
        nullptr, // reserved
        nullptr, // addresses
        &size);
    if (sizeResult != ERROR_BUFFER_OVERFLOW || size >= 1000000) {
        return Result("UpdateLANInfo", "GetAdaptersAddresses size check failed", ErrorType::Asio, sizeResult);
    }

    std::vector<uint8_t> data(size);
    PIP_ADAPTER_ADDRESSES addrs = (PIP_ADAPTER_ADDRESSES)data.data();
    const DWORD getResult = ::GetAdaptersAddresses(
        kFamily,
        kFlags,
        nullptr, // reserved
        addrs, // addresses
        &size);
    if (getResult != NO_ERROR) {
        return Result("UpdateLANInfo", "GetAdaptersAddresses failed", ErrorType::Asio, getResult);
    }

    for (; addrs; addrs = addrs->Next)
    {
        if (addrs->IfIndex != row.dwForwardIfIndex) {
            continue;
        }

        // If interface is up:
        if (addrs->OperStatus == IfOperStatusUp)
        {
            const unsigned family = addrs->FirstUnicastAddress->Address.lpSockaddr->sa_family;
            if (family == AF_INET)
            {
                sockaddr_in* sa_in4 = (sockaddr_in *)addrs->FirstUnicastAddress->Address.lpSockaddr;
                auto bytesCast4 = (asio::ip::address_v4::bytes_type*)&sa_in4->sin_addr.S_un.S_un_b;
                asio::ip::address_v4 addr4(*bytesCast4);
                info.Localhost = addr4;
            }
            else if (family == AF_INET6)
            {
                sockaddr_in6* sa_in6 = (sockaddr_in6 *)addrs->FirstUnicastAddress->Address.lpSockaddr;
                auto bytesCast6 = (asio::ip::address_v6::bytes_type*)sa_in6->sin6_addr.u.Byte;
                asio::ip::address_v6 addr6(*bytesCast6, sa_in6->sin6_scope_id);
                info.Localhost = addr6;
            }
            else {
                return Result("UpdateLANInfo", "Invalid interface unicast family", ErrorType::Asio, family);
            }
        }
        else
        {
            info.Gateway = asio::ip::address();
        }

        return Result::Success();
    }

    return Result("UpdateLANInfo", "Could not find the forwarding interface", ErrorType::Asio, row.dwForwardIfIndex);
}

#elif defined(__linux__)

// Linux version
static Result GetLANInfo(LANInfo& info)
{
    TONK_UNUSED(info);
    return Result::Success();
}

#else // Unsupported platform

static Result GetLANInfo(LANInfo& info)
{
    TONK_UNUSED(info);
    return Result::Success();
}

#endif


// Shared part of the update function
Result UpdateLANInfo(LANInfo& info, bool& updated)
{
    LANInfo latest;
    Result result = GetLANInfo(latest);

    updated = (latest.Gateway != info.Gateway) ||
        (latest.Localhost != info.Localhost);
    info = latest;

    return result;
}


//------------------------------------------------------------------------------
// Tools

static bool IsWANConnectionServiceType(const std::string& serviceType)
{
    if (serviceType.size() <= 2) {
        return false;
    }

    std::string noVersion = serviceType.substr(0, serviceType.size() - 2);

    return (0 == StrCaseCompare(noVersion.c_str(), "urn:schemas-upnp-org:service:WANIPConnection") ||
            0 == StrCaseCompare(noVersion.c_str(), "urn:schemas-upnp-org:service:WANPPPConnection"));
}

Result ParsedURL::Parse(const std::string& url)
{
    FullURL = url;

    size_t ipEnd, slash;

    // Find port offset
    size_t portStart = url.find(':');
    if (portStart == std::string::npos) {
        ipEnd = url.length();
        slash = std::string::npos;
    }
    else {
        ipEnd = portStart;
        ++portStart;
        slash = portStart;
    }

    size_t portEnd;
    std::string path;
    int port;

    // Find path offset relative to port
    slash = url.find('/', slash);
    if (slash == std::string::npos) {
        portEnd = url.length();
        path.clear();
    }
    else {
        portEnd = slash;
        if (ipEnd > slash) {
            ipEnd = slash;
        }
        path = url.substr(slash + 1);
    }

    // Parse out port field
    if (portStart == std::string::npos) {
        port = 80; // Default HTTP port
    }
    else {
        const std::string portStr = url.substr(portStart, portEnd - portStart);
        port = atoi(portStr.c_str());
        if (port <= 0) {
            return Result("Port did not parse");
        }
    }

    // Note that regardless of protocol we always connect on port 80 with
    // HTTP (no SSL encryption) unless a port is specified in the URL
    if (port == 443) {
        // Force port 80 instead of HTTPS since we do not support encryption
        port = 80;
    }

    // Parse out IP field
    std::string ip = url.substr(0, ipEnd);

    // Fill info
    IP = ip;
    Port = (uint16_t)port;
    XMLPath = path;

    asio::error_code ec;

    // Convert IP string to IP address
    IPAddr = asio::ip::make_address(ip, ec);

    if (ec) {
        return Result("retrieveDescription: Invalid IP string", ec.message(), ErrorType::Asio, ec.value());
    }

    TCPEndpoint = asio::ip::tcp::endpoint(IPAddr, Port);

    return Result::Success();
}

Result ParsedXML::Parse(const char* data, unsigned bytes)
{
    URLBase.clear();
    ServiceType.clear();
    ControlURL.clear();

    unsigned tag_start = 0, data_start = 0;
    bool inService = false;
    std::string serviceType, controlURL;

    for (unsigned i = 0; i < bytes; ++i)
    {
        char ch = data[i];
        if (tag_start != 0)
        {
            if (ch == '>')
            {
                std::string tagData(data + tag_start, i - tag_start);
                const char* tag = tagData.c_str();
                bool end = (tag[0] == '/');

                if (end)
                {
                    ++tag;
                    if (tag_start > data_start)
                    {
                        if (inService)
                        {
                            if (0 == StrCaseCompare(tag, "serviceType")) {
                                serviceType = std::string(data + data_start, tag_start - data_start - 1);
                            }
                            else if (0 == StrCaseCompare(tag, "controlURL")) {
                                controlURL = std::string(data + data_start, tag_start - data_start - 1);
                            }
                        }
                        else if (0 == StrCaseCompare(tag, "URLBase")) {
                            URLBase = std::string(data + data_start, tag_start - data_start - 1);
                        }
                    }
                }

                if (0 == StrCaseCompare(tag, "service"))
                {
                    if (end && IsWANConnectionServiceType(serviceType))
                    {
                        ServiceType = serviceType;
                        ControlURL = controlURL;
                    }
                    inService = !end;
                }

                tag_start = 0;
                data_start = i + 1;
            }
        }
        else if (ch == '<') {
            tag_start = i + 1;
        }
    }

    if (ServiceType.empty()) {
        return Result("Unable to find ServiceType");
    }
    if (ControlURL.empty()) {
        return Result("Unable to find ControlURL");
    }
    return Result::Success();
}


//------------------------------------------------------------------------------
// AsioHost

Result AsioHost::Initialize()
{
    Context = MakeUniqueNoThrow<asio::io_context>();
    if (!Context) {
        return Result::OutOfMemory();
    }
    Context->restart();

    AsioEventStrand = MakeUniqueNoThrow<asio::io_context::strand>(*Context);
    Ticker = MakeUniqueNoThrow<asio::steady_timer>(*Context);
    if (!Ticker || !AsioEventStrand) {
        return Result::OutOfMemory();
    }

    /*
        Asio bug work-around: If we do not keep some work queued for Asio it
        will internally call stop() for some reason and then use a ton of
        CPU expecting us to end our worker threads.  The best way I've found
        to prevent the work-queue to go to 0 items is to keep a timer queued
        from before the first worker starts running.  The timer OnTick()
        keeps re-queuing a timer tick so the work-queue is never empty.
    */
    TickAndPostNextTimer();

    Terminated = false;
    AsioThread = MakeUniqueNoThrow<std::thread>(&AsioHost::WorkerLoop, this);
    if (!AsioThread) {
        return Result::OutOfMemory();
    }

    return Result::Success();
}

void AsioHost::WorkerLoop()
{
    asio::error_code ec;

    while (!Terminated)
    {
        Context->run(ec);

        if (ec) {
            ModuleLogger.Warning("Worker loop context error: ", ec.message());
        }
    }
}

void AsioHost::DispatchTick()
{
    // Note: This is posted to a background thread and will execute after the
    // current thread of execution retires.
    AsioEventStrand->post([this]() {
        TickAndPostNextTimer();
    });
}

void AsioHost::TickAndPostNextTimer()
{
    // Run OnTick function in StateMachine
    State->OnTick();

    // This resets the existing timer so that we do not get two ticks in a row
    static const unsigned kTickIntervalUsec = 1000 * 1000; /// 1 second
    Ticker->expires_after(std::chrono::microseconds(kTickIntervalUsec));

    Ticker->async_wait(([this](const asio::error_code& error)
    {
        if (!error) {
            TickAndPostNextTimer();
        }
    }));
}

void AsioHost::Shutdown()
{
    if (Context) {
        // Note there is no need to stop or cancel timer ticks or sockets
        Context->stop();
    }

    // Note this has to be done after cancel/close of sockets above because
    // otherwise Asio can hang waiting for any outstanding callbacks
    Terminated = true;

    if (AsioThread)
    {
        try {
            if (AsioThread->joinable()) {
                AsioThread->join();
            }
        }
        catch (std::system_error& err) {
            ModuleLogger.Warning("Exception while joining thread: ", err.what());
        }
    }

    AsioThread = nullptr;
    Ticker = nullptr;
    AsioEventStrand = nullptr;

    // Keep the context object alive a bit longer while the other parts go
    // out of scope.
    //Context = nullptr;
}


//------------------------------------------------------------------------------
// SSDPRequester

void SSDPRequester::OnTick()
{
    // If initialization is needed:
    if (NeedsInitialization)
    {
        NeedsInitialization = false;

        // Attempt to initialize sockets
        Result result = InitializeSockets();

        // If attempt failed:
        if (result.IsFail())
        {
            ModuleLogger.Error("Socket initialization failed: ", result.ToJson());
            return;
        }
    }

    // If poll timer has expired:
    const uint64_t nowUsec = siamese::GetTimeUsec();
    if (nowUsec - LastLANInfoRequestUsec > protocol::kLANInfoIntervalUsec)
    {
        LastLANInfoRequestUsec = siamese::GetTimeUsec();

        // Check if LAN info changed
        PollLANInfo();
    }

    // If there are no multicasts requested:
    if (AttemptsRemaining <= 0) {
        return;
    }

    AttemptsRemaining--;

    // Request SSDP from gateway
    RequestSSDP();
}

void SSDPRequester::PollLANInfo()
{
    bool updated = false;
    Result lanResult = UpdateLANInfo(LAN, updated);
    if (lanResult.IsFail()) {
        ModuleLogger.Warning("Could not get LAN info: ", lanResult.ToJson());
    }
    if (!updated) {
        return;
    }
    HaveSeenAGatewayAddress |= !LAN.Gateway.is_unspecified();

    ModuleLogger.Info("Detected new LAN configuration. Gateway: ", LAN.Gateway.to_string(),
        " LocalIP: ", LAN.Localhost.to_string());

    // If network is not possibly up
    if (IsNetworkDefinitelyDown()) {
        return;
    }

    // Convert IP string to IP address
    asio::error_code ec;
    const asio::ip::address multicastIP = \
        asio::ip::make_address(kUPnPMulticastAddress, ec);
    TONK_DEBUG_ASSERT(!ec); // Should never happen

    // Send to multicast address
    SSDPAddresses.clear();
    SSDPAddresses.emplace_back(multicastIP, kPortSSDP);

    // If we were able to get the LAN gateway address:
    if (!LAN.Gateway.is_unspecified()) {
        SSDPAddresses.emplace_back(LAN.Gateway, kPortSSDP);
    }

    // Restart SSDP scanning
    BeginSearch();

    // Allow state machine to react to LAN change
    State->OnLANChange();
}

Result SSDPRequester::InitializeSockets()
{
    asio::error_code ec;

    Socket_UDP = MakeUniqueNoThrow<asio::ip::udp::socket>(*Host->Context);
    if (!Socket_UDP) {
        return Result::OutOfMemory();
    }

    UDPAddress bindAddress;

    // If application specified an interface address:
    if (State->InterfaceAddress.empty()) {
        // Note: Asio default endpoint address is the any address e.g. INADDR_ANY
        bindAddress = UDPAddress(asio::ip::udp::v4(), 0);
    }
    else
    {
        // Convert provided interface address to Asio address
        const asio::ip::address bindAddr = asio::ip::make_address(State->InterfaceAddress, ec);
        if (ec) {
            return Result("Provided InterfaceAddress was invalid", ec.message(), ErrorType::Asio, ec.value());
        }

        // Bind to the provided interface address
        bindAddress = UDPAddress(bindAddr, 0);
    }

    Socket_UDP->open(bindAddress.protocol(), ec);
    if (ec) {
        return Result("UDP socket open failed", ec.message(), ErrorType::Asio, ec.value());
    }
    Socket_UDP->set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        return Result("UDP socket set_option reuse_address failed", ec.message(), ErrorType::Asio, ec.value());
    }
    Socket_UDP->bind(bindAddress, ec);
    if (ec) {
        return Result("UDP bind failed", ec.message(), ErrorType::Asio, ec.value());
    }

    // Start listening
    PostNextRead_UDP();

    return Result::Success();
}

void SSDPRequester::RequestSSDP()
{
    // If initialization failed:
    if (!Socket_UDP) {
        return;
    }

    // For each schema to try:
    for (const char* schemaRequest : kSSDPRequestList)
    {
        const size_t requestBytes = strlen(schemaRequest);

        for (const auto& addr : SSDPAddresses)
        {
            //ModuleLogger.Info("Requesting SSDP from ", addr.address().to_string());

            // Send the canned request
            Socket_UDP->async_send_to(
                asio::buffer(schemaRequest, requestBytes),
                addr,
                [requestBytes](const asio::error_code& ec, std::size_t bytes)
            {
                // Warn about any funky results
                if (ec) {
                    ModuleLogger.Warning("async_send_to returned with error: ", ec.message());
                }
                else if (bytes != requestBytes) {
                    ModuleLogger.Warning("async_send_to sent partial bytes = ", bytes);
                }
            });
        }
    }
}

void SSDPRequester::PostNextRead_UDP()
{
    SourceAddress = UDPAddress();

    // Launch an asynchronous recvfrom()
    Socket_UDP->async_receive_from(
        asio::buffer(ReadBuffer_UDP, kReadBufferBytes),
        SourceAddress,
        [this](const asio::error_code& ec, size_t bytes)
    {
        // If an error was reported:
        if (ec)
        {
            // If it was not just an ICMP error:
            if (SourceAddress.address().is_unspecified()) {
                // Stop reading
                return;
            }
        }

        // If the socket was closed gracefully:
        if (bytes <= 0) {
            // Stop reading
            return;
        }

        Result result = OnUDPDatagram((char*)ReadBuffer_UDP, (unsigned)bytes);
        if (result.IsFail()) {
            ModuleLogger.Warning("SSDP datagram rejected: ", result.ToJson());
        }

        PostNextRead_UDP();
    });
}

struct membuf : std::streambuf
{
    membuf(char* begin, char* end)
    {
        this->setg(begin, begin, end);
    }
};

// trim from start (in place)
static inline void ltrim(std::string &s)
{
    s.erase(s.begin(), std::find_if(s.begin(), s.end(),
        [](int ch)
    {
        return !std::isspace(ch);
    }));
}

// trim from end (in place)
static inline void rtrim(std::string &s)
{
    s.erase(std::find_if(s.rbegin(), s.rend(),
        [](int ch)
    {
        return !std::isspace(ch);
    }).base(), s.end());
}

// trim from both ends (in place)
static inline void trim(std::string &s)
{
    ltrim(s);
    rtrim(s);
}

Result SSDPRequester::OnUDPDatagram(char* data, unsigned bytes)
{
    membuf sbuf(data, data + bytes);
    std::istream is(&sbuf);

    std::string line;

    // Check if response starts with a success code
    if (!std::getline(is, line)) {
        return Result("Truncated UDP response");
    }
    if (line.find("200 OK") == std::string::npos) {
        return Result("Ignoring malformed UDP response (No \"200 OK\")");
    }

    //ModuleLogger.Info("**** ", SourceAddress.address().to_string(), ":", SourceAddress.port());

    // For each remaining line:
    while (std::getline(is, line))
    {
        //ModuleLogger.Info("LINE: ", line);

        const bool isLocation = (0 == StrCaseCompare(line.substr(0, 9).c_str(), "location:"));
        if (!isLocation) {
            continue;
        }

        // Parse out the first HTTP or HTTPS URL in the response.
        const std::string::size_type begin = line.find("://");
        if (begin == std::string::npos) {
            return Result("Ignoring malformed UDP response (No protocol)");
        }
        std::string url(line, begin + 3);
        trim(url);

        ParsedURL parsed;
        Result result = parsed.Parse(url);
        if (result.IsFail()) {
            return result;
        }

        State->OnSSDPResponse(parsed);
        return Result::Success();
    }

    return Result("No location found");
}


//------------------------------------------------------------------------------
// StateMachine

void StateMachine::Initialize()
{
    uint64_t seed[2];
    Result result = SecureRandom_Next((uint8_t*)&seed[0], sizeof(seed));
    if (result.IsFail()) {
        ModuleLogger.Warning("SecureRandom failed to generate random seed");
    }
    Rand.Seed(seed[0], seed[1]);
}

void StateMachine::API_Request(const AppRequest& request)
{
    TONK_DEBUG_ASSERT(request.LocalPort != 0);

    // This will be called from the application

    // Hold API lock while accessing the requests list
    {
        std::lock_guard<std::mutex> locker(API_Lock);

        bool found = false;
        for (auto& r : API_Requests)
        {
            // Note: Requests are uniquely identified by ports
            if (r.LocalPort == request.LocalPort)
            {
                r = request;
                found = true;
                break;
            }
        }

        if (!found) {
            API_Requests.push_back(request);
        }

        // Set the dirty flag
        API_Requests_Updated = true;
    }

    // Run OnTick() immediately
    Host->DispatchTick();
}

void StateMachine::API_CopyRequests()
{
    // This must be called from the background thread

    if (!API_Requests_Updated) {
        return;
    }

    std::lock_guard<std::mutex> locker(API_Lock);

    // Copy API_Requests into Requests
    const int requestCount = (int)API_Requests.size();
    Requests.resize(requestCount);
    memcpy(&Requests[0], &API_Requests[0], requestCount * sizeof(AppRequest));

    // Clear the dirty flag
    API_Requests_Updated = false;
}

void StateMachine::OnTick()
{
    API_CopyRequests();

    SSDP->OnTick();

    if (GatewayEndpoints.empty()) {
        return;
    }

    const uint64_t nowUsec = siamese::GetTimeUsec();

    const int gatewayCount = (int)GatewayEndpoints.size();
    for (int gatewayIndex = 0; gatewayIndex < gatewayCount; ++gatewayIndex)
    {
        auto& gateway = GatewayEndpoints[gatewayIndex];

        // If request is in progress:
        if (gateway.Requester)
        {
            // If the requester is dead:
            if (gateway.Requester->OnTick_IsDisposable(nowUsec)) {
                // Free it so we can try again
                gateway.Requester = nullptr;
            }
            else {
                continue;
            }
        }

        // If there are no more retries:
        if (gateway.RetriesRemaining <= 0) {
            continue; // Do not retry
        }

        BeginGatewayXMLRequest(gatewayIndex);
    }

    // Begin control SOAP requests for each endpoint
    const int controlCount = (int)ControlEndpoints.size();
    for (int controlIndex = 0; controlIndex < controlCount; ++controlIndex)
    {
        ControlEndpoint& control = ControlEndpoints[controlIndex];

        // If endpoint is not active:
        if (!control.EndpointActive) {
            continue;
        }

        BeginControlSOAPRequests(controlIndex);
    }
}

void StateMachine::BeginGatewayXMLRequest(int gatewayIndex)
{
    // Decrement retry count
    GatewayEndpoints[gatewayIndex].RetriesRemaining--;

    std::shared_ptr<HTTPRequester> requester = MakeSharedNoThrow<HTTPRequester>();
    if (!requester) {
        return;
    }

    ParsedURL url = GatewayEndpoints[gatewayIndex].URL;

    std::ostringstream oss;
    oss << "GET /" << url.XMLPath << " HTTP/1.1\r\n";
    oss << "Host: " << url.IP << ":" << url.Port << "\r\n";
    oss << "Accept: text/html,application/xhtml+xml,application/xml\r\n\r\n";
    std::string request = oss.str();

    Result result = requester->Begin(
        url,
        Host,
        request,
        [this, gatewayIndex, url](
            const Result& result,
            unsigned code,
            const char* response,
            unsigned bytes)
    {
        if (result.IsFail()) {
            ModuleLogger.Error("Request for XML description failed: ", result.ToJson());
            return;
        }

        // Stop trying if we got a response
        this->GatewayEndpoints[gatewayIndex].RetriesRemaining = 0;

        // Request is now considered completed
        this->GatewayEndpoints[gatewayIndex].Completed = true;

        // If HTTP response code was not 200 OK:
        if (code != 200) {
            ModuleLogger.Error("Request for XML description failed. Server returned error code: ", code);
            return;
        }

        // Parse XML response
        ParsedXML xml;
        Result parseResult = xml.Parse(response, bytes);
        if (parseResult.IsFail()) {
            ModuleLogger.Error("Failed to parse XML: ", parseResult.ToJson());
            return;
        }

        // Handle response
        this->OnXMLResponse(url, xml);
    });

    if (result.IsFail()) {
        ModuleLogger.Error("Failed to start web request: ", result.ToJson());
        return;
    }

    GatewayEndpoints[gatewayIndex].Requester = requester;
}

void StateMachine::OnSSDPResponse(const ParsedURL& url)
{
    const int gatewayCount = (int)GatewayEndpoints.size();

    // Find it if it is already in the list
    for (int gatewayIndex = 0; gatewayIndex < gatewayCount; ++gatewayIndex)
    {
        auto& gateway = GatewayEndpoints[gatewayIndex];

        // If it is already known:
        if (gateway.URL == url)
        {
            if (gateway.Completed) {
                return;
            }

            //ModuleLogger.Debug("Gateway already known - Requesting gateway descriptor again: ", url.FullURL);

            // If a gateway is advertised again, attempt to query its descriptor
            // to handle the case where we rejoined the same LAN
            gateway.RetriesRemaining = protocol::kPortMappingRetries;

            // If a request is not in progress:
            if (nullptr == gateway.Requester) {
                // Start a request now for XML descriptor
                BeginGatewayXMLRequest(gatewayIndex);
            }

            return;
        }
    }

    ModuleLogger.Info("Learned of gateway endpoint via SSDP: ", url.FullURL);

    GatewayEndpoint gateway;
    gateway.URL = url;

    GatewayEndpoints.push_back(gateway);
    const int addedGatewayIndex = (int)GatewayEndpoints.size() - 1;

    BeginGatewayXMLRequest(addedGatewayIndex);
}

void StateMachine::OnXMLResponse(const ParsedURL& sourceUrl, const ParsedXML& xml)
{
    // Default to using the source host/port
    ParsedURL url = sourceUrl;

    // But if a URLBase is specified, then use that
    if (!xml.URLBase.empty())
    {
        Result result = url.Parse(xml.URLBase);
        if (result.IsFail()) {
            ModuleLogger.Error("Failed to parse URLBase: ", result.ToJson());
            return;
        }
    }

    // Fix up the FullURL before using it to compare URLs
    std::stringstream full;
    full << url.IP << ":" << url.Port << xml.ControlURL;
    url.FullURL = full.str();

    // Find it if it is already in the list
    const int controlCount = (int)ControlEndpoints.size();
    for (int controlIndex = 0; controlIndex < controlCount; ++controlIndex)
    {
        auto& control = ControlEndpoints[controlIndex];

        // If the control URL was found:
        if (control.URL == url)
        {
            // Reactivate endpoint if it is found again on the LAN.
            // This is helpful if we leave and rejoin the same LAN
            control.EndpointActive = true;

            BeginControlSOAPRequests(controlIndex);

            return;
        }
    }

    ModuleLogger.Info("Found control endpoint via gateway: ", url.FullURL);

    ControlEndpoint control;
    control.URL = url;
    control.XML = xml;

    ControlEndpoints.push_back(control);
    const int controlIndex = (int)ControlEndpoints.size() - 1;

    BeginControlSOAPRequests(controlIndex);
}

void StateMachine::BeginControlSOAPRequests(int controlIndex)
{
    ControlEndpoint& control = ControlEndpoints[controlIndex];

    // Get current time
    const uint64_t nowUsec = siamese::GetTimeUsec();

    // Resize requests
    const int requestCount = (int)Requests.size();
    if (requestCount != (int)control.Requests.size()) {
        control.Requests.resize(requestCount);
    }

    // Check each request
    for (int requestIndex = 0; requestIndex < requestCount; ++requestIndex)
    {
        ControlRequest& controlRequest = control.Requests[requestIndex];
        const AppRequest app = Requests[requestIndex];

        // If HTTP request is already in progress:
        if (nullptr != controlRequest.Requester)
        {
            // If the requester is dead:
            if (controlRequest.Requester->OnTick_IsDisposable(nowUsec)) {
                // Free it so we can try again
                controlRequest.Requester = nullptr;
            }
            else {
                continue;
            }
        }

        // The application request type can fluctuate between Add/Remove at any time.

        // If the app request has changed:
        if (controlRequest.LastApp != app.Type)
        {
            controlRequest.LastApp = app.Type;

            if (app.Type == RequestType::Remove &&
                controlRequest.AddedExternalPort != 0)
            {
                // The app is requesting a remove and we had added a mapping
                controlRequest.RetriesRemaining = protocol::kPortMappingRetries;

                // Reset some state in case we want to add it again later
                controlRequest.AddAgainOnTimer = false;

                ModuleLogger.Debug("Removing the gateway port mapping for local:",
                    app.LocalPort, " <-> external:", controlRequest.AddedExternalPort);
            }
            else if (app.Type == RequestType::Add &&
                controlRequest.AddedExternalPort == 0)
            {
                // The app is requesting to add and we do not have a mapping
                controlRequest.RetriesRemaining = protocol::kPortMappingRetries;

                ModuleLogger.Debug("Adding the gateway port mapping for local:",
                    app.LocalPort);
            }
        }

        // If an external port was added successfully some time ago:
        if (app.Type == RequestType::Add &&
            controlRequest.AddAgainOnTimer &&
            controlRequest.AddedExternalPort != 0 &&
            nowUsec - controlRequest.LastSuccessUsec > protocol::kUPnPLeaseRenewSecs * 1000 * 1000)
        {
            /*
                Keep retrying forever after the lease renew interval elapses.

                This handles the case where the network goes down after a port
                is mapped, so the LAN info is still unchanged in Windows (same
                Local IP and same Gateway).  While the network is down, this
                causes continuous HTTP requests from the control endpoint about
                once per second until the uplink is restored or the LAN changes.

                LAN changes clear the AddAgainOnTimer flag to exit this loop.
            */
            controlRequest.RetriesRemaining = protocol::kPortMappingRetries;

            ModuleLogger.Debug("Time to renew the gateway port mapping for local:",
                app.LocalPort, " <-> external:", controlRequest.AddedExternalPort);
        }

        // If the network is definitely down avoid sending queries:
        if (SSDP->IsNetworkDefinitelyDown()) {
            continue;
        }

        // If there are no more retries:
        if (controlRequest.RetriesRemaining <= 0) {
            continue; // Do not retry
        }
        controlRequest.RetriesRemaining--;

        // If retries are remaining then we should perform a request
        RequestSOAP(controlIndex, requestIndex);
    }
}

void StateMachine::RequestSOAP(int controlIndex, int requestIndex)
{
    TONK_DEBUG_ASSERT(controlIndex < (int)ControlEndpoints.size());
    ControlEndpoint& control = ControlEndpoints[controlIndex];
    TONK_DEBUG_ASSERT(requestIndex < (int)control.Requests.size());
    ControlRequest& controlRequest = control.Requests[requestIndex];
    const AppRequest app = Requests[requestIndex];

    const ParsedURL& url = control.URL;
    const ParsedXML& xml = control.XML;

    std::string service_namespace = xml.ServiceType;
    char const* action;
    uint16_t external_port, local_port;

    if (app.Type == RequestType::Add)
    {
        action = "AddPortMapping";

        // If a prior request failed due to port assignment conflict:
        if (controlRequest.FailedRequestedPort) {
            // Pick a random port
            external_port = protocol::CalculateRandomNATPort(Rand);
        }
        else if (controlRequest.AddedExternalPort == 0) {
            external_port = app.LocalPort;
        }
        else {
            // Reuse a previous external port
            external_port = controlRequest.AddedExternalPort;
        }
    }
    else
    {
        action = "DeletePortMapping";
        external_port = controlRequest.AddedExternalPort;
    }

    // Note: Local Port will not change - It's a unique key for each request
    local_port = app.LocalPort;

    if (external_port == 0 || local_port == 0) {
        TONK_DEBUG_BREAK(); // Should never happen
        return;
    }

    // If localhost is unspecified then use an empty string
    std::string localaddr_str;
    if (!SSDP->LAN.Localhost.is_unspecified()) {
        localaddr_str = SSDP->LAN.Localhost.to_string();
    }

    std::shared_ptr<HTTPRequester> requester = MakeSharedNoThrow<HTTPRequester>();
    if (!requester) {
        return;
    }

    std::stringstream soap;
    soap << "<?xml version=\"1.0\"?>\n";
    soap << "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" ";
    soap << "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">";
    soap << "<s:Body><u:" << action << " xmlns:u=\"" << service_namespace << "\">";
    soap << "<NewRemoteHost></NewRemoteHost>";
    soap << "<NewExternalPort>" << external_port << "</NewExternalPort>";
    soap << "<NewProtocol>UDP</NewProtocol>";
    if (app.Type == RequestType::Add)
    {
        soap << "<NewInternalPort>" << local_port << "</NewInternalPort>";
        soap << "<NewInternalClient>" << localaddr_str << "</NewInternalClient>";
        soap << "<NewEnabled>1</NewEnabled>";
        soap << "<NewPortMappingDescription>Tonk</NewPortMappingDescription>";
        soap << "<NewLeaseDuration>" << protocol::kUPnPLeaseDurationSecs << "</NewLeaseDuration>";
    }
    soap << "</u:" << action << "></s:Body></s:Envelope>";
    std::string soapStr = soap.str();

    std::stringstream http;
    http << "POST " << xml.ControlURL << " HTTP/1.1\r\n";
    http << "Host: " << url.IP << ":" << url.Port << "\r\n";
    http << "Content-Type: text/xml; charset=\"utf-8\"\r\n";
    http << "Content-Length: " << soapStr.size() << "\r\n";
    http << "Soapaction: \"" << service_namespace << "#" << action << "\"\r\n\r\n";
    http << soapStr;
    std::string httpStr = http.str();

    Result result = requester->Begin(
        url,
        Host,
        httpStr,
        [this, controlIndex, requestIndex, external_port, app](
            const Result& result,
            unsigned code,
            const char* /*response*/,
            unsigned /*bytes*/)
    {
        ControlRequest& controlRequest = this->ControlEndpoints[controlIndex].Requests[requestIndex];

        // Reset the FailedRequestedPort on any resolution
        controlRequest.FailedRequestedPort = false;

        // If the request failed (could not connect etc):
        if (result.IsFail())
        {
            ModuleLogger.Error("Control request failed: ", result.ToJson());

            // The retry counter will have us make another request on a timer
            return;
        }

        // Reset the AddAgainOnTimer state if the server was reachable
        controlRequest.AddAgainOnTimer = false;

        // If the HTTP response code is not 200 OK:
        if (code != 200)
        {
            ModuleLogger.Error("Control request failed. Server returned error code: ", code);

            // I've seen error code 500 used to indicate this

            if (app.Type == RequestType::Add)
            {
                // Set FailedRequestedPort to indicate the error
                controlRequest.FailedRequestedPort = true;

                // Tick faster so we retry faster
                Host->DispatchTick();
            }

            return;
        }

        // Reset the last success timestamp
        controlRequest.LastSuccessUsec = siamese::GetTimeUsec();

        // Stop retrying
        controlRequest.RetriesRemaining = 0;

        // Note: If the request type changed (even repeatedly e.g. ABA) then
        // this still works.  The timer loop will notice that the current
        // state is not the desired state, and it will kick off more retries.

        // If the request was to add a mapping:
        if (app.Type == RequestType::Add)
        {
            ModuleLogger.Info("Successfully mapped local port ", app.LocalPort,
                " to external port ", external_port);

            controlRequest.AddedExternalPort = external_port;

            // Re-add the mapping before it expires
            controlRequest.AddAgainOnTimer = true;
        }
        else
        {
            ModuleLogger.Info("Successfully unmapped local port ", app.LocalPort,
                " from external port ", controlRequest.AddedExternalPort);

            controlRequest.AddedExternalPort = 0;
        }

        // Stop searching for more gateways to reduce traffic
        this->SSDP->EndSearch();

        // Select what value to provide via the API for `ExternalMappedPort`
        uint16_t apiValue = (app.Type == RequestType::Add) ? external_port : 0;

        //----------------------------------------------------------------------
        // API Lock
        {
            std::lock_guard<std::mutex> locker(APILock);

            MappedPortLifetime* lp = API_Requests[requestIndex].API_LifetimePtr;
            if (lp) {
                lp->ExternalMappedPort = apiValue;
            }
        }
        //----------------------------------------------------------------------
    });

    if (result.IsFail()) {
        ModuleLogger.Error("Failed to start control web request: ", result.ToJson());
        return;
    }

    // Keep requester alive
    controlRequest.Requester = requester;
}

void StateMachine::OnLANChange()
{
    // For all gateways:
    for (auto& gateway : GatewayEndpoints)
    {
        // Gateway completed
        gateway.Completed = false;
    }

    // For all endpoints:
    for (auto& control : ControlEndpoints)
    {
        // Disable all requests from all endpoints until we find them again
        control.EndpointActive = false;

        // Attempt to reset all requests
        for (auto& request : control.Requests)
        {
            request.AddAgainOnTimer = false;
            request.AddedExternalPort = 0;
            request.FailedRequestedPort = false;
            request.LastApp = RequestType::Remove;
            request.LastSuccessUsec = 0;
            request.RetriesRemaining = protocol::kPortMappingRetries;

            if (nullptr != request.Requester) {
                // TBD: Can this cause issues?
                ModuleLogger.Warning("LAN change cleared an active request: ", control.URL.FullURL);
            }
        }
    }
}


//------------------------------------------------------------------------------
// HTTPRequester

bool HTTPRequester::OnTick_IsDisposable(uint64_t nowUsec)
{
    // If requester is done and no reference remain:
    if (Done && AsyncRefs <= 0) {
        // It can be deleted now
        return true;
    }

    // Check if timeout has elapsed
    const uint64_t deltaUsec = nowUsec - StartUsec;
    if (deltaUsec >= kTimeoutUsec) {
        Result result("Connection timeout");
        OnDone(result);
    }

    // Check again on the next tick
    return false;
}

Result HTTPRequester::Begin(
    const ParsedURL& url,
    AsioHost* host,
    const std::string& request,
    const HTTPCallback& callback)
{
    URL = url;
    Host = host;
    Request = request;
    Callback = callback;

    // Record the start time
    StartUsec = siamese::GetTimeUsec();

    // Create TCP socket
    Socket_TCP = MakeUniqueNoThrow<asio::ip::tcp::socket>(*Host->Context);
    if (!Socket_TCP) {
        return Result::OutOfMemory();
    }

    asio::error_code ec;
    const asio::ip::tcp::endpoint bindAddress(asio::ip::tcp::v4(), 0);

    Socket_TCP->open(bindAddress.protocol(), ec);
    if (ec) {
        return Result("retrieveDescription: tcpSocket open failed", ec.message(), ErrorType::Asio, ec.value());
    }

    Socket_TCP->set_option(asio::socket_base::reuse_address(true), ec);
    if (ec) {
        return Result("retrieveDescription: tcpSocket set_option reuse_address failed", ec.message(), ErrorType::Asio, ec.value());
    }

    // Connect
    ++AsyncRefs;
    Socket_TCP->async_connect(
        url.TCPEndpoint,
        [this](const asio::error_code& ec)
    {
        --AsyncRefs;

        // If connection failed:
        if (ec)
        {
            Result result = Result("Connection failed", ec.message(), ErrorType::Asio, ec.value());
            OnDone(result);
            return;
        }

        ModuleLogger.Info("Making HTTP request: ", URL.FullURL);

        // Send request
        ++AsyncRefs;
        Socket_TCP->async_send(
            asio::buffer(Request.c_str(), Request.size()),
            [this](const asio::error_code& ec, std::size_t bytes)
        {
            --AsyncRefs;

            // Warn about any funky results
            if (ec) {
                Result result = Result("async_send returned with error", ec.message(), ErrorType::Asio, ec.value());
                OnDone(result);
            }
            else if (bytes != Request.size()) {
                Result result = Result("async_send sent bytes does not match request size", std::to_string(bytes), ErrorType::Asio);
                OnDone(result);
            }
        });

        // Read the header
        ++AsyncRefs;
        asio::async_read_until(
            *Socket_TCP,
            TCPStreamBuf,
            "\r\n\r\n",
            [this](const asio::error_code &ec, std::size_t bytes)
        {
            --AsyncRefs;

            // If the socket was closed:
            if (bytes <= 0 || ec)
            {
                Result result = Result("Connection closed", ec.message(), ErrorType::Asio, ec.value());
                OnDone(result);
                return;
            }

            std::istream is(&TCPStreamBuf);
            std::string line;

            // Read a line at a time:
            while (std::getline(is, line))
            {
                // On a double-newline:
                if (line.size() <= 1)
                {
                    BodyContent.reserve(ContentLength);
                    BodyContent.resize(0);
                    break;
                }

                const bool isHTTP = (0 == StrCaseCompare(line.substr(0, 5).c_str(), "HTTP/"));
                if (isHTTP)
                {
                    // HTTP/x.x ### OK
                    int statusCode = atoi(line.c_str() + 9);
                    if (statusCode >= 0) {
                        HTTPStatusCode = statusCode;
                    }
                    continue;
                }

                const bool isContentLength = (0 == StrCaseCompare(line.substr(0, 15).c_str(), "Content-Length:"));
                if (isContentLength)
                {
                    // Content-Length: ###
                    int contentLength = atoi(line.c_str() + 15);
                    if (contentLength > 0 && contentLength < kContentLengthMax) {
                        ContentLength = contentLength;
                    }
                    continue;
                }
            }

            ContinueReadingBody();
        });
    });

    return Result::Success();
}

void HTTPRequester::ContinueReadingBody()
{
    if (Done) {
        return;
    }

    std::istream is(&TCPStreamBuf);

    while (TCPStreamBuf.in_avail() > 0)
    {
        // Append bytes from the multiple buffers backing TCPStreamBuf
        // into the single contiguous buffer of body content we will return
        const size_t priorBytes = BodyContent.size();
        const size_t availBytes = (size_t)TCPStreamBuf.in_avail();
        BodyContent.resize(priorBytes + availBytes);
        is.read((char*)BodyContent.data() + priorBytes, availBytes);

        // If we are done:
        if (ContentLength <= BodyContent.size()) {
            break;
        }
    }

    // If we are done:
    if (ContentLength <= BodyContent.size())
    {
        const Result success = Result::Success();
        OnDone(success, HTTPStatusCode, (char*)BodyContent.data(), (unsigned)ContentLength);
        return;
    }

    // Read enough to fill the BodyContent buffer
    unsigned readCount = static_cast<unsigned>(ContentLength - BodyContent.size());

    // Request to read exactly the amount left in the body
    ++AsyncRefs;
    asio::async_read(
        *Socket_TCP,
        TCPStreamBuf,
        asio::transfer_exactly(readCount),
        [this](const asio::error_code& ec, size_t bytes)
    {
        --AsyncRefs;

        // If the socket was closed:
        if (bytes <= 0 || ec)
        {
            Result result = Result("Connection closed", ec.message(), ErrorType::Asio, ec.value());
            OnDone(result);
            return;
        }

        ContinueReadingBody();
    });
}

void HTTPRequester::OnDone(
    const Result& result,
    unsigned httpResponseCode,
    const char* response,
    unsigned bytes)
{
    if (Done) {
        return;
    }

    Callback(result, httpResponseCode, response, bytes);
    Done = true;

    // Close TCP socket so any stuck reads/writes get canceled
    if (Socket_TCP_Control) {
        asio::error_code ec;
        Socket_TCP_Control->close(ec);
    }
}


} // namespace gateway
} // namespace tonk
