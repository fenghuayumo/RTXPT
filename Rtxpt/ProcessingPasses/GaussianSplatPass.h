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

#include <donut/core/math/math.h>
#include <nvrhi/nvrhi.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../Shaders/SampleConstantBuffer.h"

namespace donut::engine
{
    class IView;
    class ShaderFactory;
}

class GPUSort;
class RenderTargets;

struct GaussianSplatRenderSettings
{
    bool enabled = true;
    bool depthTest = true;
    bool shadowsEnabled = false;
    float splatScale = 1.0f;
    float alphaScale = 1.0f;
    float brightness = 1.0f;
    float alphaCullThreshold = 1.0f / 255.0f;
    float shadowStrength = 0.75f;
    float shadowRayTMax = 100000.0f;
    donut::math::float3 shadowDirectionToLight = donut::math::float3(0.0f, 1.0f, 0.0f);
    donut::math::float4x4 objectToWorld = donut::math::float4x4::identity();
};

class GaussianSplatPass
{
public:
    GaussianSplatPass(
        nvrhi::IDevice* device,
        std::shared_ptr<donut::engine::ShaderFactory> shaderFactory);

    void SetGpuSort(std::shared_ptr<GPUSort> gpuSort);

    bool LoadFromFile(const std::filesystem::path& fileName, bool convertRdfToDonut);

    void CreatePipeline(const RenderTargets& renderTargets);
    void BuildAccelerationStructures(nvrhi::ICommandList* commandList);
    void ReleaseAccelerationStructures();

    void Render(
        nvrhi::ICommandList* commandList,
        const donut::engine::IView& view,
        nvrhi::rt::IAccelStruct* meshTopLevelAS,
        const RenderTargets& renderTargets,
        const GaussianSplatRenderSettings& settings);

    [[nodiscard]] bool HasSplats() const { return m_splatCount > 0; }
    [[nodiscard]] uint32_t GetSplatCount() const { return m_splatCount; }
    [[nodiscard]] const std::string& GetSourceFileName() const { return m_sourceFileName; }
    [[nodiscard]] nvrhi::rt::IAccelStruct* GetTopLevelAS() const { return m_splatTopLevelAS.Get(); }
    [[nodiscard]] nvrhi::IBuffer* GetSplatBuffer() const { return m_splatBuffer.Get(); }

private:
    void CreateBindingSets(const RenderTargets& renderTargets, nvrhi::rt::IAccelStruct* meshTopLevelAS);
    void UploadSplatDataIfNeeded(nvrhi::ICommandList* commandList);
    void SortSplats(nvrhi::ICommandList* commandList, const GaussianSplatConstants& constants);
    [[nodiscard]] bool CanReuseSort(const GaussianSplatConstants& constants) const;
    void InvalidateSortCache();

    nvrhi::DeviceHandle m_device;
    std::shared_ptr<donut::engine::ShaderFactory> m_shaderFactory;
    std::shared_ptr<GPUSort> m_gpuSort;

    nvrhi::BufferHandle m_constantBuffer;
    nvrhi::BufferHandle m_splatBuffer;
    nvrhi::BufferHandle m_shBuffer;
    nvrhi::BufferHandle m_indexBuffer;
    nvrhi::BufferHandle m_sortKeyBuffer;
    nvrhi::BufferHandle m_sortControlBuffer;
    nvrhi::BufferHandle m_splatAabbBuffer;

    nvrhi::BindingLayoutHandle m_rasterRenderBindingLayout;
    nvrhi::BindingLayoutHandle m_hybridRenderBindingLayout;
    nvrhi::BindingLayoutHandle m_sortKeyBindingLayout;
    nvrhi::BindingSetHandle m_rasterRenderBindingSet;
    nvrhi::BindingSetHandle m_hybridRenderBindingSet;
    nvrhi::BindingSetHandle m_sortKeyBindingSet;

    nvrhi::ShaderHandle m_rasterVertexShader;
    nvrhi::ShaderHandle m_rasterPixelShader;
    nvrhi::ShaderHandle m_hybridVertexShader;
    nvrhi::ShaderHandle m_hybridPixelShader;
    nvrhi::ShaderHandle m_sortKeyShader;
    nvrhi::GraphicsPipelineHandle m_rasterRenderPipeline;
    nvrhi::GraphicsPipelineHandle m_hybridRenderPipeline;
    nvrhi::ComputePipelineHandle m_sortKeyPipeline;
    nvrhi::rt::AccelStructHandle m_splatBottomLevelAS;
    nvrhi::rt::AccelStructHandle m_splatTopLevelAS;
    nvrhi::rt::IAccelStruct* m_hybridRenderMeshTopLevelAS = nullptr;

    std::vector<GaussianSplatData> m_splats;
    std::vector<donut::math::float4> m_shCoefficients;
    uint32_t m_splatCount = 0;
    uint32_t m_shDegree = 0;
    bool m_splatUploadPending = false;
    bool m_accelStructBuildPending = false;
    bool m_sortCacheValid = false;
    uint32_t m_cachedSortSplatCount = 0;
    donut::math::float4x4 m_cachedSortWorldToClipNoOffset = donut::math::float4x4::identity();
    donut::math::float4x4 m_cachedSortObjectToWorld = donut::math::float4x4::identity();
    std::string m_sourceFileName;
};
