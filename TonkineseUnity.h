/*
    Copyright (c) 2017-2018 Christopher A. Taylor.  All rights reserved.

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

#ifndef TONK_UNITY_PLUGIN_H
#define TONK_UNITY_PLUGIN_H

/*
    Tonk Unity Native Plugin

    Unity native plugins have a C API that is exported through the DLL.
    The Tonk library already has a C API, so the Unity native plugin for
    Tonk mainly needs to add some boilerplate that Unity requires.
*/

// Enable this to enable using this DLL as a Unity plugin.
// This requires that Unity is installed 
//#define TONK_ENABLE_UNITY_PLUGIN

#ifdef TONK_ENABLE_UNITY_PLUGIN

// UnityPluginLoad and UnityPluginUnload are defined in IUnityInterface.h
#include "C:\Unity2018.1.2f1\Editor\Data\PluginAPI\IUnityInterface.h"
//******************************************************************************
// If this include fails then the Unity SDK is missing.
// You can disable the Unity Plugin by undefining TONK_ENABLE_UNITY_PLUGIN.
//******************************************************************************

#endif // TONK_ENABLE_UNITY_PLUGIN

#endif // TONK_UNITY_PLUGIN_H
