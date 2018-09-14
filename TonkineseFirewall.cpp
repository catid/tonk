/** \file
    \brief Tonk Implementation: Firewall
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

#include "TonkineseFirewall.h"

#ifdef _WIN32

#define _WIN32_WINNT 0x0400
#include <windows.h>
#include <netfw.h>
#include <atlbase.h>

#pragma comment( lib, "ole32.lib" )
#pragma comment( lib, "oleaut32.lib" )

#endif // _WIN32


namespace tonk {

static logger::Channel ModuleLogger("Firewall", MinimumLogLevel);


//------------------------------------------------------------------------------
// Windows port

#ifdef _WIN32

// Module data
static HRESULT m_comInit = E_FAIL;
static CComPtr<INetFwProfile> m_fwProfile;

bool WindowsFirewallInitialize(OUT INetFwProfile** fwProfile)
{
    CComPtr<INetFwMgr> fwMgr;
    CComPtr<INetFwPolicy> fwPolicy;

    TONK_DEBUG_ASSERT(fwProfile != nullptr);

    *fwProfile = nullptr;

    // Create an instance of the firewall settings manager.
    HRESULT hr = fwMgr.CoCreateInstance(
        __uuidof(NetFwMgr),
        nullptr,
        CLSCTX_INPROC_SERVER);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwMgr.CoCreateInstance failed: ", tonk::HexString(hr));
        return false;
    }

    // Retrieve the local firewall policy.
    hr = fwMgr->get_LocalPolicy(&fwPolicy);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwMgr->get_LocalPolicy failed: ", tonk::HexString(hr));
        return false;
    }

    // Retrieve the firewall profile currently in effect.
    hr = fwPolicy->get_CurrentProfile(fwProfile);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwPolicy->get_CurrentProfile failed: ", tonk::HexString(hr));
        return false;
    }

    return true;
}

bool WindowsFirewallAddApp(
    IN INetFwProfile* fwProfile,
    IN const wchar_t* fwProcessImageFileName,
    IN const wchar_t* fwName)
{
    TONK_DEBUG_ASSERT(fwProfile != nullptr);
    TONK_DEBUG_ASSERT(fwProcessImageFileName != nullptr);
    TONK_DEBUG_ASSERT(fwName != nullptr);

    // Retrieve the authorized application collection.
    CComPtr<INetFwAuthorizedApplications> fwApps;
    HRESULT hr = fwProfile->get_AuthorizedApplications(&fwApps);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwProfile->get_AuthorizedApplications failed: ", tonk::HexString(hr));
        return false;
    }

    // Create NetFwAuthorizedApplication
    CComPtr<INetFwAuthorizedApplication> fwApp;
    hr = fwApp.CoCreateInstance(
        __uuidof(NetFwAuthorizedApplication),
        nullptr,
        CLSCTX_INPROC_SERVER);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwApp.CoCreateInstance failed: ", tonk::HexString(hr));
        return false;
    }

    // Set the process image file name.
    CComBSTR fwBstrProcessImageFileName = fwProcessImageFileName;
    hr = fwApp->put_ProcessImageFileName(fwBstrProcessImageFileName);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwApp->put_ProcessImageFileName failed: ", tonk::HexString(hr));
        return false;
    }

    // Set the application friendly name.
    CComBSTR fwBstrName = fwName;
    hr = fwApp->put_Name(fwBstrName);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwApp->put_Name failed: ", tonk::HexString(hr));
        return false;
    }

    // Add the application to the collection.
    hr = fwApps->Add(fwApp);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwApps->Add failed: ", tonk::HexString(hr));
        return false;
    }

    ModuleLogger.Debug("Successfully added this application to the firewall allowed list");
    return true;
}

bool WindowsFirewallPortAdd(
    IN INetFwProfile* fwProfile,
    IN LONG portNumber,
    IN NET_FW_IP_PROTOCOL ipProtocol,
    IN const wchar_t* name)
{
    TONK_DEBUG_ASSERT(fwProfile != nullptr);
    TONK_DEBUG_ASSERT(portNumber != 0);
    TONK_DEBUG_ASSERT(name != nullptr);

    CComPtr<INetFwOpenPort> fwOpenPort;
    CComPtr<INetFwOpenPorts> fwOpenPorts;

    // Retrieve the collection of globally open ports.
    HRESULT hr = fwProfile->get_GloballyOpenPorts(&fwOpenPorts);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwProfile->get_GloballyOpenPorts failed: ", tonk::HexString(hr));
        return false;
    }

    // Create an instance of an open port.
    hr = fwOpenPort.CoCreateInstance(
        __uuidof(NetFwOpenPort),
        nullptr,
        CLSCTX_INPROC_SERVER);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwOpenPort.CoCreateInstance failed: ", tonk::HexString(hr));
        return false;
    }

    // Set the port number.
    hr = fwOpenPort->put_Port(portNumber);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwOpenPort->put_Port failed: ", tonk::HexString(hr));
        return false;
    }

    // Set the IP protocol.
    hr = fwOpenPort->put_Protocol(ipProtocol);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwOpenPort->put_Protocol failed: ", tonk::HexString(hr));
        return false;
    }

    // Set the friendly name of the port.
    CComBSTR fwBstrName = name;
    hr = fwOpenPort->put_Name(fwBstrName);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwOpenPort->put_Name failed: ", tonk::HexString(hr));
        return false;
    }

    // Opens the port and adds it to the collection.
    hr = fwOpenPorts->Add(fwOpenPort);
    if (FAILED(hr))
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("fwOpenPorts->Add failed: ", tonk::HexString(hr));
        return false;
    }

    return true;
}

bool Initialize()
{
    TONK_DEBUG_ASSERT(!m_fwProfile);

    // Initialize COM.
    HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
    m_comInit = hr;

    // Ignore RPC_E_CHANGED_MODE; this just means that COM has already been
    // initialized with a different mode. Since we don't care what the mode is,
    // we'll just use the existing mode.
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        TONK_DEBUG_BREAK();
        ModuleLogger.Error("CoInitializeEx failed: ", tonk::HexString(hr));
        return false;
    }

    const bool initResult = WindowsFirewallInitialize(&m_fwProfile);
    if (!initResult) {
        return false;
    }

    wchar_t wpath[MAX_PATH];
    ::GetModuleFileNameW(::GetModuleHandleW(NULL), wpath, sizeof(wpath));

    // Add Windows Messenger to the authorized application collection.
    return WindowsFirewallAddApp(m_fwProfile, wpath, L"Tonk");
}

bool AddPort(uint16_t port)
{
    const bool addResult = WindowsFirewallPortAdd(
        m_fwProfile,
        port,
        NET_FW_IP_PROTOCOL_UDP,
        L"Tonk");

    return addResult;
}

void Shutdown()
{
    m_fwProfile.Release();

    // Uninitialize COM.
    if (SUCCEEDED(m_comInit)) {
        CoUninitialize();
    }
}

#else // _WIN32

void Initialize()
{
    ModuleLogger.Debug("Platform does not support firewall modifications");
}

void AddPort(uint16_t)
{
}

void Shutdown()
{
}

#endif // _WIN32


//------------------------------------------------------------------------------
// API

void Firewall_Initialize()
{
    Initialize();
}

Result Firewall_AddPort(uint16_t port)
{
    uint64_t t0 = siamese::GetTimeUsec();

    AddPort(port);

    uint64_t t1 = siamese::GetTimeUsec();
    uint64_t dt = t1 - t0;

    ModuleLogger.Debug("Firewall_AddPort in ", dt / 1000, " msec for port = ", port);

    return Result::Success();
}

void Firewall_Shutdown()
{
    Shutdown();
}


} // namespace tonk
