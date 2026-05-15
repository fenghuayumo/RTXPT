/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "RenderSession.h"

#if RTXPT_WITH_PYTHON

#include "../AdvancedSample.h"
#include "../Sample.h"
#include "../SampleCommon/SampleCommon.h"

#include <donut/app/DeviceManager.h>
#include <donut/core/log.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/app/UserInterfaceUtils.h>

#include <GLFW/glfw3.h>
#include <array>
#include <chrono>
#include <thread>
#include <filesystem>

#if DONUT_WITH_DX12
#include <d3d12.h>
#include <wrl/client.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

using namespace donut;

namespace
{
    std::filesystem::path GetCurrentModuleDirectory()
    {
#ifdef _WIN32
        HMODULE module = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&GetCurrentModuleDirectory),
                &module))
        {
            std::array<wchar_t, 32768> path = {};
            DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
            if (length > 0 && length < path.size())
                return std::filesystem::path(path.data()).parent_path();
        }
#else
        Dl_info info = {};
        if (dladdr(reinterpret_cast<void*>(&GetCurrentModuleDirectory), &info) && info.dli_fname)
            return std::filesystem::path(info.dli_fname).parent_path();
#endif

        return donut::app::GetDirectoryWithExecutable();
    }

    std::filesystem::path ResolveRuntimeDirectory()
    {
        std::filesystem::path moduleDirectory = GetCurrentModuleDirectory();
        if (std::filesystem::exists(moduleDirectory / "ShaderPrecompiled"))
            return moduleDirectory;

        std::filesystem::path executableDirectory = donut::app::GetDirectoryWithExecutable();
        if (std::filesystem::exists(executableDirectory / "ShaderPrecompiled"))
            return executableDirectory;

        return moduleDirectory;
    }

    std::filesystem::path ResolveResourceRoot(const std::filesystem::path& runtimeDirectory)
    {
        if (std::filesystem::exists(runtimeDirectory / c_AssetsFolder))
            return runtimeDirectory;

        std::filesystem::path parentDirectory = runtimeDirectory.parent_path();
        if (std::filesystem::exists(parentDirectory / c_AssetsFolder))
            return parentDirectory;

        return donut::app::GetDirectoryWithExecutable();
    }

#if DONUT_WITH_DX12 && defined(RTXPT_D3D_AGILITY_SDK_VERSION)
    std::string GetAgilitySDKPath()
    {
        std::string sdkPath = (ResolveRuntimeDirectory() / "D3D12").string();
        if (!sdkPath.empty() && sdkPath.back() != '\\' && sdkPath.back() != '/')
            sdkPath += "\\";
        return sdkPath;
    }

    bool EnableD3D12ExperimentalShaderModels(ID3D12DeviceFactory* factory)
    {
        static const UUID D3D12ExperimentalShaderModels = { 0x76f5573e, 0xf13a, 0x40f5, {0xb2, 0x97, 0x81, 0xce, 0x9e, 0x18, 0x93, 0x3f} };
        UUID features[] = { D3D12ExperimentalShaderModels };

        HRESULT hr = factory
            ? factory->EnableExperimentalFeatures(_countof(features), features, nullptr, nullptr)
            : D3D12EnableExperimentalFeatures(_countof(features), features, nullptr, nullptr);
        if (FAILED(hr))
        {
            if (factory && hr == E_NOINTERFACE)
                return false;
            log::warning("RenderSession: D3D12 experimental shader models could not be enabled, HRESULT = 0x%08x", unsigned(hr));
            return false;
        }
        return true;
    }

    Microsoft::WRL::ComPtr<ID3D12DeviceFactory> CreateD3D12AgilityDeviceFactory()
    {
        const std::string sdkPath = GetAgilitySDKPath();

        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration1> sdkConfig1;
        HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig1));
        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<ID3D12DeviceFactory> factory;
            hr = sdkConfig1->CreateDeviceFactory(
                RTXPT_D3D_AGILITY_SDK_VERSION,
                sdkPath.c_str(),
                IID_PPV_ARGS(&factory));

            if (SUCCEEDED(hr) && factory)
            {
                EnableD3D12ExperimentalShaderModels(factory.Get());
                return factory;
            }

            log::warning("RenderSession: ID3D12SDKConfiguration1::CreateDeviceFactory('%s') failed, HRESULT = 0x%08x", sdkPath.c_str(), unsigned(hr));
        }
        else
        {
            log::warning("RenderSession: D3D12GetInterface(ID3D12SDKConfiguration1) failed, HRESULT = 0x%08x", unsigned(hr));
        }

        // Fallback for older runtimes. This works when the host process has
        // not already locked D3D12 to the system SDK.
        Microsoft::WRL::ComPtr<ID3D12SDKConfiguration> sdkConfig;
        hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig));
        if (FAILED(hr))
        {
            log::warning("RenderSession: D3D12GetInterface(ID3D12SDKConfiguration) failed, HRESULT = 0x%08x", unsigned(hr));
            return nullptr;
        }

        hr = sdkConfig->SetSDKVersion(RTXPT_D3D_AGILITY_SDK_VERSION, sdkPath.c_str());
        if (FAILED(hr))
        {
            log::warning("RenderSession: ID3D12SDKConfiguration::SetSDKVersion('%s') failed, HRESULT = 0x%08x", sdkPath.c_str(), unsigned(hr));
            return nullptr;
        }

        EnableD3D12ExperimentalShaderModels(nullptr);
        return nullptr;
    }
#endif

    donut::app::DeviceCreationParameters MakeDeviceParams(const RenderSession::Config& cfg)
    {
        donut::app::DeviceCreationParameters p;
        p.backBufferWidth        = cfg.width;
        p.backBufferHeight       = cfg.height;
        p.swapChainSampleCount   = 1;
        p.swapChainBufferCount   = c_SwapchainCount;
        p.startFullscreen        = false;
        p.startBorderless        = false;
        p.vsyncEnabled           = false;       // headless => no need for vsync
        p.enableRayTracingExtensions = true;
        p.adapterIndex           = cfg.adapterIndex;
        p.headlessDevice         = cfg.headless;

        if (cfg.debug)
        {
            p.enableDebugRuntime         = true;
            p.enableNvrhiValidationLayer = true;
        }

        p.supportExplicitDisplayScaling = true;

#if DONUT_WITH_DX12 && defined(RTXPT_D3D_AGILITY_SDK_VERSION)
        p.featureLevel = D3D_FEATURE_LEVEL_12_2;
#elif DONUT_WITH_DX12
        p.featureLevel = D3D_FEATURE_LEVEL_12_1;
#endif

#if DONUT_WITH_VULKAN
        p.requiredVulkanDeviceExtensions.push_back("VK_KHR_buffer_device_address");
        p.requiredVulkanDeviceExtensions.push_back("VK_KHR_format_feature_flags2");
        p.ignoredVulkanValidationMessageLocations.push_back(0x0000000023e43bb7);
        p.ignoredVulkanValidationMessageLocations.push_back(0x000000000609a13b);
        p.ignoredVulkanValidationMessageLocations.push_back(0x00000000c5a3822a);
        p.ignoredVulkanValidationMessageLocations.push_back(0x00000000591f70f2);
        p.ignoredVulkanValidationMessageLocations.push_back(0x000000005e6e827d);
#endif

#if DONUT_WITH_STREAMLINE
        p.checkStreamlineSignature = true;
        p.streamlineAppId = 231313132;
#endif

        return p;
    }
}

RenderSession::RenderSession(const Config& cfg)
    : m_config(cfg)
{
    // Mirror command-line semantics: this is the configuration the rest of
    // the renderer (CaptureScriptManager, Sample::Init, ...) consumes.
    m_cmdLine.width             = uint32_t(cfg.width);
    m_cmdLine.height            = uint32_t(cfg.height);
    m_cmdLine.useVulkan         = cfg.useVulkan;
    m_cmdLine.adapterIndex      = cfg.adapterIndex;
    m_cmdLine.debug             = cfg.debug;
    m_cmdLine.nonInteractive    = cfg.nonInteractive;
    m_cmdLine.scene             = cfg.scene;
    m_cmdLine.OverrideToReferenceMode = !cfg.realtimeMode;
    m_cmdLine.OverrideToRealtimeMode  =  cfg.realtimeMode;
    m_cmdLine.ReferenceSamplesPerPixel = cfg.accumulationTarget;
    m_cmdLine.RealtimeAA = 0; // no DLSS unless caller explicitly requests it
    m_cmdLine.UseReSTIRDI = false;
    m_cmdLine.UseReSTIRGI = false;

    if (cfg.nonInteractive)
    {
        log::EnableOutputToMessageBox(false);
        log::EnableOutputToConsole(true);
        log::SetMinSeverity(log::Severity::Warning);
        HelpersSetNonInteractive();
    }

    if (!InitDevice())
    {
        log::error("RenderSession: failed to initialize the graphics device");
        return;
    }

    if (!InitRenderer())
    {
        log::error("RenderSession: failed to initialize the renderer");
        return;
    }

    m_initialized = true;

    // If a scene was specified up-front, InitRenderer already requested it.
    // Wait for the first rendered frame instead of reloading the same scene.
    if (!cfg.scene.empty())
        WaitUntilReady();
}

RenderSession::~RenderSession()
{
    Shutdown();
}

bool RenderSession::InitDevice()
{
    nvrhi::GraphicsAPI api = m_config.useVulkan
        ? nvrhi::GraphicsAPI::VULKAN
        : nvrhi::GraphicsAPI::D3D12;

#if DONUT_WITH_DX12 && defined(RTXPT_D3D_AGILITY_SDK_VERSION)
    if (api == nvrhi::GraphicsAPI::D3D12)
        m_d3d12DeviceFactory = CreateD3D12AgilityDeviceFactory();
#endif

    m_deviceManager.reset(donut::app::DeviceManager::Create(api));
    if (!m_deviceManager)
    {
        log::error("RenderSession: DeviceManager::Create returned null");
        return false;
    }
    m_deviceManager->SetFrameTimeUpdateInterval(1.0f);

    auto deviceParams = MakeDeviceParams(m_config);
#if DONUT_WITH_DX12 && defined(RTXPT_D3D_AGILITY_SDK_VERSION)
    if (api == nvrhi::GraphicsAPI::D3D12 && m_d3d12DeviceFactory)
        deviceParams.d3d12DeviceFactory = m_d3d12DeviceFactory.Get();
#endif

    // Even in headless mode, we currently rely on a real GLFW window so the
    // existing swap-chain & DLSS/Streamline code paths keep working.  We
    // simply create the window invisible (GLFW_VISIBLE = FALSE is the donut
    // default) and never call glfwShowWindow afterwards.
    if (!glfwInit())
    {
        log::error("RenderSession: glfwInit failed");
        return false;
    }

    if (!m_deviceManager->CreateWindowDeviceAndSwapChain(deviceParams, "rtxpt_py"))
    {
        log::error("RenderSession: failed to create device and swap chain");
        return false;
    }

    m_deviceManager->m_callbacks.beforePresent =
        [this](donut::app::DeviceManager& manager, uint32_t) {
            m_lastRenderedBackBufferIndex = manager.GetCurrentBackBufferIndex();
        };

    if (m_config.headless && m_deviceManager->GetWindow())
        glfwHideWindow(m_deviceManager->GetWindow());

    return true;
}

bool RenderSession::InitRenderer()
{
    // Shader factory pulls precompiled shaders from the host executable's
    // ShaderPrecompiled folder - this folder is assumed to live next to the
    // .pyd / Rtxpt.exe binary.
    const char* shaderTypeName = donut::app::GetShaderTypeName(m_deviceManager->GetGraphicsAPI());
    const std::filesystem::path appDirectory = ResolveRuntimeDirectory();
    SetRuntimeDirectoryOverride(appDirectory);
    SetLocalPathBaseOverride(ResolveResourceRoot(appDirectory));
    std::filesystem::path frameworkShaderPath = appDirectory / "ShaderPrecompiled/framework" / shaderTypeName;
    std::filesystem::path appShaderPath       = appDirectory / "ShaderPrecompiled/Rtxpt"     / shaderTypeName;
    std::filesystem::path nrdShaderPath       = appDirectory / "ShaderPrecompiled/nrd"       / shaderTypeName;
    std::filesystem::path ommShaderPath       = appDirectory / "ShaderPrecompiled/omm"       / shaderTypeName;

    auto rootFS = std::make_shared<donut::vfs::RootFileSystem>();
    rootFS->mount("/ShaderPrecompiled/donut", frameworkShaderPath);
    rootFS->mount("/ShaderPrecompiled/app",   appShaderPath);
    rootFS->mount("/ShaderPrecompiled/nrd",   nrdShaderPath);
    rootFS->mount("/ShaderPrecompiled/omm",   ommShaderPath);

    auto device = m_deviceManager->GetDevice();
    m_shaderFactory = std::make_shared<donut::engine::ShaderFactory>(device, rootFS, "/ShaderPrecompiled");

    m_renderer = std::make_unique<AdvancedPathTracer>(*m_deviceManager, m_cmdLine);

    // Pick whichever scene the user requested (or fall back to the donut
    // default).  This loads the actual scene file asynchronously inside
    // Sample::Init().
    std::string preferredScene = m_config.scene.empty()
        ? std::string("bistro-programmer-art.scene.json")
        : m_config.scene;

    m_renderer->Init(preferredScene, m_shaderFactory);
    m_deviceManager->AddRenderPassToBack(m_renderer.get());

    return true;
}

void RenderSession::Shutdown()
{
    if (m_deviceManager && m_renderer)
        m_deviceManager->RemoveRenderPass(m_renderer.get());

    m_renderer.reset();
    m_shaderFactory.reset();

    if (m_deviceManager)
    {
        m_deviceManager->Shutdown();
        m_deviceManager.reset();
    }
    m_initialized = false;
}

bool RenderSession::LoadScene(const std::string& sceneName, bool waitUntilReady)
{
    if (!m_initialized || !m_renderer)
        return false;

    m_renderer->SetCurrentScene(sceneName, /*forceReload=*/true);

    if (waitUntilReady)
        return WaitUntilReady();
    return true;
}

bool RenderSession::WaitUntilReady(int maxFrames)
{
    if (!m_initialized || !m_deviceManager)
        return false;

    // Loading + first-frame setup may take quite a few frames - keep
    // pumping until the renderer reports its accumulation index moved
    // past 0 (= scene fully loaded and at least one image was produced).
    for (int i = 0; i < maxFrames; ++i)
    {
        Step(0.0f);
        if (m_renderer && m_renderer->IsSceneLoaded() && !m_renderer->IsSceneLoading())
            return true;
    }
    log::warning("RenderSession: scene did not finish loading within %d frames", maxFrames);
    return false;
}

bool RenderSession::Step(float dt)
{
    if (!m_initialized || !m_deviceManager)
        return false;

    if (dt < 0.0f)
    {
        // Negative dt -> use real-time elapsed since last call.
        // Donut tracks this internally via m_PreviousFrameTimestamp so we
        // simply forward to RunSingleFrame() and let it compute the delta.
    }

    return m_deviceManager->RunSingleFrame();
}

bool RenderSession::StepN(int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        if (!Step())
            return false;
    }
    return true;
}

int RenderSession::StepUntilAccumulated(int maxFrames)
{
    if (!m_initialized || !m_renderer)
        return 0;

    // Force reference / accumulation mode so we know "done" actually means
    // the SPP target has been reached.
    g_sampleUIData.ResetAccumulation = true;

    int target = (maxFrames > 0)
        ? maxFrames
        : std::max(1, g_sampleUIData.AccumulationTarget + 128);

    int frames = 0;
    while (frames < target)
    {
        if (!Step()) break;
        ++frames;
        if (m_renderer->AccumulationCompleted())
            break;
    }
    return frames;
}

bool RenderSession::SaveScreenshot(const std::string& outputPath)
{
    if (!m_initialized || !m_deviceManager || !m_renderer)
        return false;

    uint32_t backBufferIndex = m_lastRenderedBackBufferIndex;
    if (backBufferIndex == UINT32_MAX)
        backBufferIndex = m_deviceManager->GetCurrentBackBufferIndex();

    nvrhi::ITexture* tex = m_deviceManager->GetBackBuffer(backBufferIndex);
    if (!tex)
    {
        log::error("RenderSession: no current back buffer");
        return false;
    }

    auto commonPasses = m_renderer->GetCommonPasses();
    if (!commonPasses)
    {
        log::error("RenderSession: common passes not initialized yet");
        return false;
    }

    // SaveTextureToFile creates and tears down its own command list internally
    // - safe to call from the main thread between Step()s.
    std::filesystem::path p(outputPath);
    if (p.has_parent_path())
        EnsureDirectoryExists(p.parent_path());

    return donut::engine::SaveTextureToFile(
        m_deviceManager->GetDevice(),
        commonPasses.get(),
        tex,
        nvrhi::ResourceStates::Present,
        outputPath.c_str());
}

bool RenderSession::SetCamera(const donut::math::float3& pos,
                              const donut::math::float3& dir,
                              const donut::math::float3& up)
{
    if (!m_renderer) return false;

    auto v3 = [](const donut::math::float3& v) {
        return std::to_string(v.x) + "," + std::to_string(v.y) + "," + std::to_string(v.z);
    };
    std::string s = v3(pos) + "," + v3(dir) + "," + v3(up);
    return m_renderer->SetCurrentCameraPosDirUp(s);
}

void RenderSession::SetCameraFOV(float verticalFovDegrees)
{
    if (m_renderer)
        m_renderer->SetCameraVerticalFOV(verticalFovDegrees);
}

#endif // RTXPT_WITH_PYTHON
