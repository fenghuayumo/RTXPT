/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace donut::app
{
    class DeviceManager;
}

namespace rtxpt
{
    // One GPU as reported by the graphics backend, plus the classification used to rank it.
    struct AdapterDesc
    {
        int         Index = -1;
        std::string Name;
        uint32_t    VendorID = 0;
        uint32_t    DeviceID = 0;
        uint64_t    DedicatedVideoMemory = 0;
        bool        Software = false;   // WARP / Basic Render Driver / llvmpipe and friends
        bool        Discrete = false;   // reliable on D3D12 only, see the note in AdapterSelection.cpp
    };

    // What the user asked for, from the command line or the Python API.
    struct AdapterRequest
    {
        int         Index = -1;     // explicit adapter index; negative means 'not specified'
        std::string Name;           // case-insensitive substring of the adapter name; a plain number is treated as an index
    };

    struct AdapterSelection
    {
        int         Index = -1;         // resolved index, or -1 to leave the choice to the graphics backend
        std::string Name;               // name of the resolved adapter, empty when unresolved
        bool        UserRequested = false;
    };

    // Enumerates the adapters present in the system, in backend order. `outBestIndex` receives the
    // index that automatic selection would pick, or -1 when nothing could be enumerated.
    // Requires DeviceManager::CreateInstance() to have been called.
    bool EnumerateAdapters(donut::app::DeviceManager& deviceManager, std::vector<AdapterDesc>& outAdapters, int& outBestIndex);

    // "NVIDIA", "AMD", "Intel", "Microsoft", or "unknown".
    const char* VendorName(uint32_t vendorID);

    // "discrete", "integrated", or "software".
    const char* AdapterKindName(const AdapterDesc& adapter);

    // Resolves `request` against the adapters present in the system. With an empty request this picks
    // the adapter with the best expected compute throughput rather than simply taking the first one,
    // which on laptops would often be the integrated GPU.
    //
    // Requires DeviceManager::CreateInstance() to have been called, and must run before device creation.
    // Returns index -1 when enumeration is unavailable, leaving the backend's own default in place.
    AdapterSelection SelectAdapter(donut::app::DeviceManager& deviceManager, const AdapterRequest& request);

    // Writes every adapter to the log, marking the one at `selectedIndex`. Pass -1 to mark none.
    void LogAdapters(donut::app::DeviceManager& deviceManager, int selectedIndex);
}
