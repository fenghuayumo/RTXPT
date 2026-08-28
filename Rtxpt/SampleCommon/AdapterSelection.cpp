/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "AdapterSelection.h"

#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <tuple>

namespace
{
    constexpr uint32_t c_VendorNVIDIA    = 0x10DE;
    constexpr uint32_t c_VendorAMD       = 0x1002;
    constexpr uint32_t c_VendorIntel     = 0x8086;
    constexpr uint32_t c_VendorMicrosoft = 0x1414;   // WARP / Basic Render Driver

    // Below this, DXGI is reporting an integrated GPU's carve-out rather than real dedicated VRAM.
    constexpr uint64_t c_DiscreteVideoMemoryThreshold = 1ull << 30;

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
            [](unsigned char c) { return char(std::tolower(c)); });
        return text;
    }

    bool IsSoftwareAdapter(const donut::app::AdapterInfo& adapter)
    {
        if (adapter.vendorID == c_VendorMicrosoft)
            return true;

        const std::string name = ToLower(adapter.name);
        static const char* const softwareNames[] = { "basic render", "software", "llvmpipe", "lavapipe", "swiftshader" };
        for (const char* candidate : softwareNames)
        {
            if (name.find(candidate) != std::string::npos)
                return true;
        }
        return false;
    }

    // RTXPT leans on vendor specific paths (SER, OMM, DLSS, NVAPI), so an NVIDIA part is worth more
    // than a nominally comparable one from another vendor.
    int VendorRank(uint32_t vendorID)
    {
        switch (vendorID)
        {
        case c_VendorNVIDIA: return 3;
        case c_VendorAMD:    return 2;
        case c_VendorIntel:  return 1;
        default:             return 0;
        }
    }

    // Greater is better; the tuple orders class before vendor before memory size.
    // Note that dedicated video memory is only a proxy for compute throughput, and that the discrete
    // test is reliable on D3D12 only - Vulkan reports the whole device local heap, so a UMA part can
    // look 'discrete' here. The vendor rank carries the decision in that case.
    using AdapterRank = std::tuple<int, int, int, uint64_t>;

    AdapterRank RankAdapter(const rtxpt::AdapterDesc& adapter)
    {
        return { adapter.Software ? 0 : 1, adapter.Discrete ? 1 : 0,
                 VendorRank(adapter.VendorID), adapter.DedicatedVideoMemory };
    }

    bool TryParseIndex(const std::string& text, int& outIndex)
    {
        if (text.empty())
            return false;

        const char* first = text.data();
        const char* last = text.data() + text.size();
        int value = 0;
        const auto result = std::from_chars(first, last, value);
        if (result.ec != std::errc() || result.ptr != last || value < 0)
            return false;

        outIndex = value;
        return true;
    }

    // Among the adapters whose name contains `needle`, returns the best ranked one, or -1 for no match.
    int FindAdapterByName(const std::vector<rtxpt::AdapterDesc>& adapters, const std::string& needle)
    {
        const std::string lowerNeedle = ToLower(needle);

        int best = -1;
        AdapterRank bestRank;
        for (int i = 0; i < int(adapters.size()); ++i)
        {
            if (ToLower(adapters[i].Name).find(lowerNeedle) == std::string::npos)
                continue;

            const AdapterRank rank = RankAdapter(adapters[i]);
            if (best < 0 || bestRank < rank)
            {
                best = i;
                bestRank = rank;
            }
        }
        return best;
    }

    std::string DescribeAdapter(const rtxpt::AdapterDesc& adapter)
    {
        char buffer[512];
        snprintf(buffer, sizeof(buffer), "%s [%s, %s, %llu MB]",
            adapter.Name.c_str(), rtxpt::VendorName(adapter.VendorID), rtxpt::AdapterKindName(adapter),
            (unsigned long long)(adapter.DedicatedVideoMemory >> 20));
        return buffer;
    }
}

namespace rtxpt
{
    const char* VendorName(uint32_t vendorID)
    {
        switch (vendorID)
        {
        case c_VendorNVIDIA:    return "NVIDIA";
        case c_VendorAMD:       return "AMD";
        case c_VendorIntel:     return "Intel";
        case c_VendorMicrosoft: return "Microsoft";
        default:                return "unknown";
        }
    }

    const char* AdapterKindName(const AdapterDesc& adapter)
    {
        if (adapter.Software)
            return "software";
        return adapter.Discrete ? "discrete" : "integrated";
    }

    bool EnumerateAdapters(donut::app::DeviceManager& deviceManager, std::vector<AdapterDesc>& outAdapters, int& outBestIndex)
    {
        outAdapters.clear();
        outBestIndex = -1;

        std::vector<donut::app::AdapterInfo> backendAdapters;
        if (!deviceManager.EnumerateAdapters(backendAdapters) || backendAdapters.empty())
            return false;

        outAdapters.reserve(backendAdapters.size());
        for (int i = 0; i < int(backendAdapters.size()); ++i)
        {
            const donut::app::AdapterInfo& source = backendAdapters[i];

            AdapterDesc adapter;
            adapter.Index = i;
            adapter.Name = source.name;
            adapter.VendorID = source.vendorID;
            adapter.DeviceID = source.deviceID;
            adapter.DedicatedVideoMemory = source.dedicatedVideoMemory;
            adapter.Software = IsSoftwareAdapter(source);
            adapter.Discrete = !adapter.Software && (source.dedicatedVideoMemory >= c_DiscreteVideoMemoryThreshold);
            outAdapters.push_back(std::move(adapter));
        }

        outBestIndex = 0;
        AdapterRank bestRank = RankAdapter(outAdapters[0]);
        for (int i = 1; i < int(outAdapters.size()); ++i)
        {
            const AdapterRank rank = RankAdapter(outAdapters[i]);
            if (bestRank < rank)
            {
                outBestIndex = i;
                bestRank = rank;
            }
        }

        return true;
    }

    AdapterSelection SelectAdapter(donut::app::DeviceManager& deviceManager, const AdapterRequest& request)
    {
        AdapterSelection selection;

        std::vector<AdapterDesc> adapters;
        int bestIndex = -1;
        if (!EnumerateAdapters(deviceManager, adapters, bestIndex))
        {
            // Enumeration needs an instance and is not implemented by every backend. Pass an explicit
            // index straight through so the backend can report a precise error if it is wrong.
            if (request.Index >= 0)
            {
                selection.Index = request.Index;
                selection.UserRequested = true;
            }
            else if (!request.Name.empty())
            {
                donut::log::warning("Cannot enumerate graphics adapters, ignoring the requested adapter '%s'.", request.Name.c_str());
            }
            return selection;
        }

        const int adapterCount = int(adapters.size());

        int resolved = -1;
        if (request.Index >= 0)
        {
            if (!request.Name.empty())
                donut::log::warning("Both an adapter index and an adapter name were given; using the index %d.", request.Index);

            if (request.Index < adapterCount)
                resolved = request.Index;
            else
                donut::log::warning("Adapter index %d does not exist (%d adapters present), selecting automatically.", request.Index, adapterCount);
        }
        else if (!request.Name.empty())
        {
            int asIndex = 0;
            if (TryParseIndex(request.Name, asIndex))
            {
                if (asIndex < adapterCount)
                    resolved = asIndex;
                else
                    donut::log::warning("Adapter index %d does not exist (%d adapters present), selecting automatically.", asIndex, adapterCount);
            }
            else
            {
                resolved = FindAdapterByName(adapters, request.Name);
                if (resolved < 0)
                    donut::log::warning("No adapter name contains '%s', selecting automatically.", request.Name.c_str());
            }
        }

        selection.UserRequested = (resolved >= 0);
        if (resolved < 0)
            resolved = bestIndex;

        selection.Index = resolved;
        selection.Name = adapters[resolved].Name;

        donut::log::info("Using graphics adapter %d: %s%s", resolved,
            DescribeAdapter(adapters[resolved]).c_str(),
            selection.UserRequested ? " (requested)" : " (best available)");

        return selection;
    }

    void LogAdapters(donut::app::DeviceManager& deviceManager, int selectedIndex)
    {
        std::vector<AdapterDesc> adapters;
        int bestIndex = -1;
        if (!EnumerateAdapters(deviceManager, adapters, bestIndex))
        {
            donut::log::info("No graphics adapters could be enumerated.");
            return;
        }

        donut::log::info("Available graphics adapters:");
        for (const AdapterDesc& adapter : adapters)
        {
            donut::log::info("  %c %d: %s",
                (adapter.Index == selectedIndex) ? '*' : ' ', adapter.Index, DescribeAdapter(adapter).c_str());
        }
    }
}
