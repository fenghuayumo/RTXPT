/*
* Copyright (c) 2025, NVIDIA CORPORATION.  All rights reserved.
*
* NVIDIA CORPORATION and its licensors retain all intellectual property
* and proprietary rights in and to this software, related documentation
* and any modifications thereto.  Any use, reproduction, disclosure or
* distribution of this software and related documentation without an express
* license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#if RTXPT_WITH_PYTHON

#include "PythonBindingsCore.h"

#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/optional.h>
#include <nanobind/operators.h>

#include "../Sample.h"
#include "../SampleUI.h"
#include "../SampleCommon/ExtendedScene.h"
#include "../Materials/MaterialsBaker.h"
#include "../Lighting/LightsBaker.h"

#if DONUT_WITH_STREAMLINE
#include <donut/app/StreamlineInterface.h>
#endif

#include <donut/engine/Scene.h>
#include <donut/engine/SceneTypes.h>
#include <donut/engine/SceneGraph.h>
#include <donut/core/log.h>
#include <donut/core/math/math.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace nb = nanobind;
using namespace donut::engine;
using donut::math::float2;
using donut::math::float3;
using donut::math::float4;
using donut::math::double3;

// Singleton consumed by embed mode (set by PythonScripting before Py_Initialize).
// In extension mode this stays nullptr - Renderer manages its own Sample.
Sample* g_pythonSampleSingleton = nullptr;

// Distinct C++ enum types so nanobind can register them as separate Python
// enums (nb::enum_<T> requires T to be unique across the module).  All map
// 1:1 to ints already used by the underlying Sample / OIDN / Streamline UI.
namespace py_enums
{
    enum class PathTracerMode : int { Realtime = 0, Reference = 1 };
    enum class RealtimeAA     : int { Off = 0, TAA = 1, DLSS = 2, DLSS_RR = 3 };
    enum class DLSSMode       : int { Off = 0, MaxPerformance = 1, Balanced = 2, MaxQuality = 3, UltraPerformance = 4, UltraQuality = 5, DLAA = 6 };
    enum class DLSSFGMode     : int { Off = 0, On = 1, Auto = 2 };
    enum class DLSSRRPreset   : int { Default = 0, PresetA = 1, PresetB = 2, PresetC = 3, PresetD = 4, PresetE = 5, PresetF = 6, PresetG = 7, PresetH = 8 };
    enum class ReflexMode     : int { Off = 0, LowLatency = 1, LowLatencyWithBoost = 2 };
    enum class OidnPasses     : int { ColorOnly = 0, Albedo = 1, AlbedoNormal = 2 };
    enum class OidnPrefilter  : int { None_ = 0, Fast = 1, Accurate = 2 };
    enum class OidnQuality    : int { Fast = 0, Balanced = 1, High = 2 };
}

namespace
{
    float3 ToFloat3(const nb::object& src)
    {
        if (nb::isinstance<float3>(src))
            return nb::cast<float3>(src);
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<float> v;
        for (auto h : seq) v.push_back(nb::cast<float>(nb::handle(h)));
        if (v.size() != 3)
            throw std::runtime_error("Expected an iterable of 3 floats");
        return float3(v[0], v[1], v[2]);
    }

    nb::tuple Float3ToTuple(const float3& v) { return nb::make_tuple(v.x, v.y, v.z); }
    nb::tuple Double3ToTuple(const double3& v) { return nb::make_tuple(v.x, v.y, v.z); }

    double3 ToDouble3(const nb::object& src)
    {
        nb::sequence seq = nb::cast<nb::sequence>(src);
        std::vector<double> v;
        for (auto h : seq) v.push_back(nb::cast<double>(nb::handle(h)));
        if (v.size() != 3)
            throw std::runtime_error("Expected an iterable of 3 floats");
        return double3(v[0], v[1], v[2]);
    }
}

namespace rtxpt_py
{

void RegisterCoreBindings(nb::module_& m)
{
    // --- helpers ----------------------------------------------------------
    m.def("log_info",    [](const std::string& s) { donut::log::info("[py] %s", s.c_str()); },
          nb::arg("message"), "Forward a message to the host log at INFO level.");
    m.def("log_warning", [](const std::string& s) { donut::log::warning("[py] %s", s.c_str()); },
          nb::arg("message"), "Forward a message to the host log at WARNING level.");
    m.def("log_error",   [](const std::string& s) { donut::log::error("[py] %s", s.c_str()); },
          nb::arg("message"), "Forward a message to the host log at ERROR level.");

    using namespace py_enums;

    // All enums use `is_arithmetic()` so users can write `int(value)` /
    // `value | other` and Python -> C++ implicit conversion to the underlying
    // int field works seamlessly.

    // --- Path tracer mode (Reference vs Realtime) --------------------------
    nb::enum_<PathTracerMode>(m, "PathTracerMode",
        "Selects between accumulating reference rendering and a realtime path tracer.",
        nb::is_arithmetic())
        .value("Realtime",  PathTracerMode::Realtime,  "Realtime mode - 1 SPP per frame, denoiser, DLSS, RTXDI.")
        .value("Reference", PathTracerMode::Reference, "Reference / accumulation mode - converges to ground truth.")
        .export_values();

    // --- AA / super-resolution / denoising preset --------------------------
    nb::enum_<RealtimeAA>(m, "RealtimeAA",
        "Realtime-mode AA / SR / denoising preset (mirrors the UI 'AA/SR/Denoising' combo).",
        nb::is_arithmetic())
        .value("Off",     RealtimeAA::Off,     "No AA / no upscaling.")
        .value("TAA",     RealtimeAA::TAA,     "Temporal anti-aliasing (no DLSS).")
        .value("DLSS",    RealtimeAA::DLSS,    "DLSS Super Resolution.")
        .value("DLSS_RR", RealtimeAA::DLSS_RR, "DLSS Ray Reconstruction (DLSS + denoising).")
        .export_values();

    // --- DLSS quality enums (mirrors SI::DLSSMode) -------------------------
    nb::enum_<DLSSMode>(m, "DLSSMode",
        "Quality preset for DLSS (used by both DLSS and DLSS-RR).",
        nb::is_arithmetic())
        .value("Off",              DLSSMode::Off)
        .value("MaxPerformance",   DLSSMode::MaxPerformance)
        .value("Balanced",         DLSSMode::Balanced)
        .value("MaxQuality",       DLSSMode::MaxQuality)
        .value("UltraPerformance", DLSSMode::UltraPerformance)
        .value("UltraQuality",     DLSSMode::UltraQuality)
        .value("DLAA",             DLSSMode::DLAA)
        .export_values();

    nb::enum_<DLSSFGMode>(m, "DLSSFGMode", "Frame generation (DLSS-G) mode.",
        nb::is_arithmetic())
        .value("Off",  DLSSFGMode::Off)
        .value("On",   DLSSFGMode::On)
        .value("Auto", DLSSFGMode::Auto)
        .export_values();

    nb::enum_<DLSSRRPreset>(m, "DLSSRRPreset",
        "DLSS-RR neural network preset (DLSSRRPreset).",
        nb::is_arithmetic())
        .value("Default", DLSSRRPreset::Default)
        .value("PresetA", DLSSRRPreset::PresetA)
        .value("PresetB", DLSSRRPreset::PresetB)
        .value("PresetC", DLSSRRPreset::PresetC)
        .value("PresetD", DLSSRRPreset::PresetD)
        .value("PresetE", DLSSRRPreset::PresetE)
        .value("PresetF", DLSSRRPreset::PresetF)
        .value("PresetG", DLSSRRPreset::PresetG)
        .value("PresetH", DLSSRRPreset::PresetH)
        .export_values();

    nb::enum_<ReflexMode>(m, "ReflexMode", "NVIDIA Reflex low-latency mode.",
        nb::is_arithmetic())
        .value("Off",                 ReflexMode::Off)
        .value("LowLatency",          ReflexMode::LowLatency)
        .value("LowLatencyWithBoost", ReflexMode::LowLatencyWithBoost)
        .export_values();

    // --- OIDN denoiser enums (mirror OidnDenoiser::Passes/Prefilter/Quality)
    nb::enum_<OidnPasses>(m, "OidnPasses",
        "Auxiliary guide passes used by OIDN (Color Only / Albedo / Albedo+Normal).",
        nb::is_arithmetic())
        .value("ColorOnly",    OidnPasses::ColorOnly)
        .value("Albedo",       OidnPasses::Albedo)
        .value("AlbedoNormal", OidnPasses::AlbedoNormal)
        .export_values();

    nb::enum_<OidnPrefilter>(m, "OidnPrefilter", "OIDN auxiliary prefilter quality.",
        nb::is_arithmetic())
        .value("None_",    OidnPrefilter::None_)
        .value("Fast",     OidnPrefilter::Fast)
        .value("Accurate", OidnPrefilter::Accurate)
        .export_values();

    nb::enum_<OidnQuality>(m, "OidnQuality", "OIDN beauty filter quality / performance trade-off.",
        nb::is_arithmetic())
        .value("Fast",     OidnQuality::Fast)
        .value("Balanced", OidnQuality::Balanced)
        .value("High",     OidnQuality::High)
        .export_values();

    // --- PTMaterial -------------------------------------------------------
    nb::class_<PTMaterial>(m, "Material",
        "RTXPT material wrapper (PTMaterial). All edits flag the material as\n"
        "dirty so the GPU buffer is re-uploaded the following frame.")
        .def_ro("name",         &PTMaterial::Name)
        .def_ro("model_name",   &PTMaterial::ModelName)
        .def_ro("unique_name",  &PTMaterial::UniqueName)

        .def_prop_rw("base_color",
            [](PTMaterial& self) { return Float3ToTuple(self.BaseOrDiffuseColor); },
            [](PTMaterial& self, nb::object v) { self.BaseOrDiffuseColor = ToFloat3(v); self.GPUDataDirty = true; },
            "Metal-rough base color or spec-gloss diffuse color (linear RGB).")
        .def_prop_rw("specular_color",
            [](PTMaterial& self) { return Float3ToTuple(self.SpecularColor); },
            [](PTMaterial& self, nb::object v) { self.SpecularColor = ToFloat3(v); self.GPUDataDirty = true; })
        .def_prop_rw("emissive_color",
            [](PTMaterial& self) { return Float3ToTuple(self.EmissiveColor); },
            [](PTMaterial& self, nb::object v) { self.EmissiveColor = ToFloat3(v); self.GPUDataDirty = true; })

        .def_prop_rw("emissive_intensity",
            [](PTMaterial& self) { return self.EmissiveIntensity; },
            [](PTMaterial& self, float v) { self.EmissiveIntensity = v; self.GPUDataDirty = true; })
        .def_prop_rw("metalness",
            [](PTMaterial& self) { return self.Metalness; },
            [](PTMaterial& self, float v) { self.Metalness = v; self.GPUDataDirty = true; })
        .def_prop_rw("roughness",
            [](PTMaterial& self) { return self.Roughness; },
            [](PTMaterial& self, float v) { self.Roughness = v; self.GPUDataDirty = true; })
        .def_prop_rw("opacity",
            [](PTMaterial& self) { return self.Opacity; },
            [](PTMaterial& self, float v) { self.Opacity = v; self.GPUDataDirty = true; })
        .def_prop_rw("transmission_factor",
            [](PTMaterial& self) { return self.TransmissionFactor; },
            [](PTMaterial& self, float v) { self.TransmissionFactor = v; self.GPUDataDirty = true; })
        .def_prop_rw("diffuse_transmission_factor",
            [](PTMaterial& self) { return self.DiffuseTransmissionFactor; },
            [](PTMaterial& self, float v) { self.DiffuseTransmissionFactor = v; self.GPUDataDirty = true; })
        .def_prop_rw("normal_texture_scale",
            [](PTMaterial& self) { return self.NormalTextureScale; },
            [](PTMaterial& self, float v) { self.NormalTextureScale = v; self.GPUDataDirty = true; })
        .def_prop_rw("ior",
            [](PTMaterial& self) { return self.IoR; },
            [](PTMaterial& self, float v) { self.IoR = v; self.GPUDataDirty = true; })
        .def_prop_rw("alpha_cutoff",
            [](PTMaterial& self) { return self.AlphaCutoff; },
            [](PTMaterial& self, float v) { self.AlphaCutoff = v; self.GPUDataDirty = true; })

        .def_prop_rw("volume_attenuation_distance",
            [](PTMaterial& self) { return self.VolumeAttenuationDistance; },
            [](PTMaterial& self, float v) { self.VolumeAttenuationDistance = v; self.GPUDataDirty = true; })
        .def_prop_rw("volume_attenuation_color",
            [](PTMaterial& self) { return Float3ToTuple(self.VolumeAttenuationColor); },
            [](PTMaterial& self, nb::object v) { self.VolumeAttenuationColor = ToFloat3(v); self.GPUDataDirty = true; })
        .def_prop_rw("nested_priority",
            [](PTMaterial& self) { return self.NestedPriority; },
            [](PTMaterial& self, int v) { self.NestedPriority = v; self.GPUDataDirty = true; })

        .def_prop_rw("use_specular_gloss",
            [](PTMaterial& self) { return self.UseSpecularGlossModel; },
            [](PTMaterial& self, bool v) { self.UseSpecularGlossModel = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_alpha_testing",
            [](PTMaterial& self) { return self.EnableAlphaTesting; },
            [](PTMaterial& self, bool v) { self.EnableAlphaTesting = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_transmission",
            [](PTMaterial& self) { return self.EnableTransmission; },
            [](PTMaterial& self, bool v) { self.EnableTransmission = v; self.GPUDataDirty = true; })
        .def_prop_rw("thin_surface",
            [](PTMaterial& self) { return self.ThinSurface; },
            [](PTMaterial& self, bool v) { self.ThinSurface = v; self.GPUDataDirty = true; })
        .def_prop_rw("exclude_from_nee",
            [](PTMaterial& self) { return self.ExcludeFromNEE; },
            [](PTMaterial& self, bool v) { self.ExcludeFromNEE = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_as_analytic_light_proxy",
            [](PTMaterial& self) { return self.EnableAsAnalyticLightProxy; },
            [](PTMaterial& self, bool v) { self.EnableAsAnalyticLightProxy = v; self.GPUDataDirty = true; })
        .def_prop_rw("skip_render",
            [](PTMaterial& self) { return self.SkipRender; },
            [](PTMaterial& self, bool v) { self.SkipRender = v; self.GPUDataDirty = true; })
        .def_prop_rw("metalness_in_red_channel",
            [](PTMaterial& self) { return self.MetalnessInRedChannel; },
            [](PTMaterial& self, bool v) { self.MetalnessInRedChannel = v; self.GPUDataDirty = true; })

        .def_prop_rw("enable_base_texture",
            [](PTMaterial& self) { return self.EnableBaseTexture; },
            [](PTMaterial& self, bool v) { self.EnableBaseTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_orm_texture",
            [](PTMaterial& self) { return self.EnableOcclusionRoughnessMetallicTexture; },
            [](PTMaterial& self, bool v) { self.EnableOcclusionRoughnessMetallicTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_normal_texture",
            [](PTMaterial& self) { return self.EnableNormalTexture; },
            [](PTMaterial& self, bool v) { self.EnableNormalTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_emissive_texture",
            [](PTMaterial& self) { return self.EnableEmissiveTexture; },
            [](PTMaterial& self, bool v) { self.EnableEmissiveTexture = v; self.GPUDataDirty = true; })
        .def_prop_rw("enable_transmission_texture",
            [](PTMaterial& self) { return self.EnableTransmissionTexture; },
            [](PTMaterial& self, bool v) { self.EnableTransmissionTexture = v; self.GPUDataDirty = true; })

        .def("mark_dirty", [](PTMaterial& self) { self.GPUDataDirty = true; },
             "Force this material's GPU buffer slot to be refreshed next frame.")
        .def("__repr__", [](const PTMaterial& self) {
                return std::string("<rtxpt.Material '") + self.Name + "'>";
            });

    // --- Lights -----------------------------------------------------------
    nb::class_<Light>(m, "Light", "Base class for all scene lights.")
        .def_prop_ro("light_type", [](Light& self) { return self.GetLightType(); })
        .def_prop_rw("color",
            [](Light& self) { return Float3ToTuple(self.color); },
            [](Light& self, nb::object v) { self.color = ToFloat3(v); })
        .def_prop_ro("name", [](Light& self) -> std::string {
                return self.GetNode() ? self.GetNode()->GetName() : std::string{};
            })
        .def_prop_rw("position",
            [](Light& self) { return Double3ToTuple(self.GetPosition()); },
            [](Light& self, nb::object v) { self.SetPosition(ToDouble3(v)); })
        .def_prop_rw("direction",
            [](Light& self) { return Double3ToTuple(self.GetDirection()); },
            [](Light& self, nb::object v) { self.SetDirection(ToDouble3(v)); })
        .def("__repr__", [](Light& self) {
                std::string n = self.GetNode() ? self.GetNode()->GetName() : "<unnamed>";
                return std::string("<rtxpt.Light '") + n + "'>";
            });

    nb::class_<DirectionalLight, Light>(m, "DirectionalLight",
        "Distant directional light source (sun-style).")
        .def_rw("irradiance", &DirectionalLight::irradiance,
                "Target illuminance (lm/m^2) - multiplied by `color`.")
        .def_rw("angular_size", &DirectionalLight::angularSize,
                "Apparent angular size of the light source, in degrees.");

    nb::class_<SpotLight, Light>(m, "SpotLight", "Spot light with inner / outer cones.")
        .def_rw("intensity", &SpotLight::intensity)
        .def_rw("radius", &SpotLight::radius)
        .def_rw("range", &SpotLight::range)
        .def_rw("inner_angle", &SpotLight::innerAngle)
        .def_rw("outer_angle", &SpotLight::outerAngle);

    nb::class_<PointLight, Light>(m, "PointLight", "Omnidirectional point light.")
        .def_rw("intensity", &PointLight::intensity)
        .def_rw("radius", &PointLight::radius)
        .def_rw("range", &PointLight::range);

    nb::class_<EnvironmentLight, Light>(m, "EnvironmentLight",
        "RTXPT environment map / IBL light.")
        .def_prop_rw("radiance_scale",
            [](EnvironmentLight& self) { return Float3ToTuple(self.radianceScale); },
            [](EnvironmentLight& self, nb::object v) { self.radianceScale = ToFloat3(v); })
        .def_rw("rotation", &EnvironmentLight::rotation)
        .def_rw("path", &EnvironmentLight::path);

    // --- Runtime UI / sampling parameters --------------------------------
    nb::class_<EnvironmentMapRuntimeParameters>(m, "EnvironmentMapParams",
        "Runtime tweakables applied on top of the EnvironmentLight in the\n"
        "current scene. Mirror of the UI controls in 'Environment'.")
        .def_prop_rw("tint_color",
            [](EnvironmentMapRuntimeParameters& s) { return Float3ToTuple(s.TintColor); },
            [](EnvironmentMapRuntimeParameters& s, nb::object v) { s.TintColor = ToFloat3(v); })
        .def_rw("intensity", &EnvironmentMapRuntimeParameters::Intensity)
        .def_prop_rw("rotation_xyz",
            [](EnvironmentMapRuntimeParameters& s) { return Float3ToTuple(s.RotationXYZ); },
            [](EnvironmentMapRuntimeParameters& s, nb::object v) { s.RotationXYZ = ToFloat3(v); })
        .def_rw("enabled", &EnvironmentMapRuntimeParameters::Enabled);

    nb::class_<SampleUIData>(m, "Settings",
        "Live UI state of the renderer. Mutating attributes is equivalent\n"
        "to moving the corresponding ImGui widget.")
        .def_rw("show_ui",                       &SampleUIData::ShowUI)
        .def_rw("enable_animations",             &SampleUIData::EnableAnimations)
        .def_rw("enable_vsync",                  &SampleUIData::EnableVsync)
        .def_rw("fps_limiter",                   &SampleUIData::FPSLimiter)

        // --- Path tracer top-level mode ----------------------------------
        .def_rw("realtime_mode",                 &SampleUIData::RealtimeMode,
                "True for realtime mode, False for reference / accumulation mode.\n"
                "See `Settings.path_tracer_mode` for an enum-flavored version.")
        .def_prop_rw("path_tracer_mode",
            [](SampleUIData& s) -> int { return s.RealtimeMode ? 0 /*Realtime*/ : 1 /*Reference*/; },
            [](SampleUIData& s, int mode) {
                bool wasRealtime = s.RealtimeMode;
                s.RealtimeMode = (mode == 0);
                if (wasRealtime != s.RealtimeMode)
                    s.ResetAccumulation = true;
            },
            "Convenience wrapper around `realtime_mode`.\n"
            "Set to rtxpt.PathTracerMode.Reference or .Realtime.")

        .def_rw("realtime_samples_per_pixel",    &SampleUIData::RealtimeSamplesPerPixel)
        .def_rw("accumulation_target",           &SampleUIData::AccumulationTarget)
        .def_rw("reset_accumulation",            &SampleUIData::ResetAccumulation)
        .def_rw("accumulation_aa",               &SampleUIData::AccumulationAA)
        .def_rw("accumulation_prewarm_realtime_caches", &SampleUIData::AccumulationPreWarmRealtimeCaches)

        .def_rw("bounce_count",                  &SampleUIData::BounceCount)
        .def_rw("diffuse_bounce_count",          &SampleUIData::DiffuseBounceCount)
        .def_rw("enable_russian_roulette",       &SampleUIData::EnableRussianRoulette)
        .def_rw("texture_lod_bias",              &SampleUIData::TexLODBias)

        .def_rw("use_nee",                       &SampleUIData::UseNEE)
        .def_rw("nee_type",                      &SampleUIData::NEEType,
                "0 = uniform, 1 = power-based, 2 = NEE-AT")
        .def_rw("nee_candidate_samples",         &SampleUIData::NEECandidateSamples)
        .def_rw("nee_full_samples",              &SampleUIData::NEEFullSamples)
        .def_rw("nee_mis_type",                  &SampleUIData::NEEMISType)

        .def_rw("use_restir_di",                 &SampleUIData::UseReSTIRDI)
        .def_rw("use_restir_gi",                 &SampleUIData::UseReSTIRGI)

        .def_rw("camera_aperture",               &SampleUIData::CameraAperture)
        .def_rw("camera_focal_distance",         &SampleUIData::CameraFocalDistance)
        .def_rw("camera_move_speed",             &SampleUIData::CameraMoveSpeed)

        .def_rw("realtime_firefly_filter_enabled", &SampleUIData::RealtimeFireflyFilterEnabled)
        .def_rw("realtime_firefly_filter_threshold", &SampleUIData::RealtimeFireflyFilterThreshold)
        .def_rw("reference_firefly_filter_enabled",  &SampleUIData::ReferenceFireflyFilterEnabled)
        .def_rw("reference_firefly_filter_threshold",&SampleUIData::ReferenceFireflyFilterThreshold)

        .def_rw("enable_tone_mapping",           &SampleUIData::EnableToneMapping)
        .def_rw("enable_bloom",                  &SampleUIData::EnableBloom)
        .def_rw("bloom_intensity",               &SampleUIData::BloomIntensity)
        .def_rw("bloom_radius",                  &SampleUIData::BloomRadius)

        .def_rw("enable_gaussian_splats",        &SampleUIData::EnableGaussianSplats)
        .def_rw("gaussian_splat_depth_test",     &SampleUIData::GaussianSplatDepthTest)
        .def_rw("gaussian_splat_scale",          &SampleUIData::GaussianSplatScale)
        .def_rw("gaussian_splat_alpha_scale",    &SampleUIData::GaussianSplatAlphaScale)
        .def_rw("gaussian_splat_brightness",     &SampleUIData::GaussianSplatBrightness)
        .def_rw("gaussian_splat_alpha_cull_threshold", &SampleUIData::GaussianSplatAlphaCullThreshold)
        .def_ro("gaussian_splat_count",          &SampleUIData::GaussianSplatCount)
        .def_ro("gaussian_splat_file_name",      &SampleUIData::GaussianSplatFileName)

        // --- AA / DLSS / DLSS-RR / DLSS-G / Reflex (realtime only) -------
        .def_rw("realtime_aa",                   &SampleUIData::RealtimeAA,
                "Realtime AA mode (rtxpt.RealtimeAA enum):\n"
                "  0 = Off, 1 = TAA, 2 = DLSS, 3 = DLSS-RR")

#if DONUT_WITH_STREAMLINE
        // DLSS quality (rtxpt.DLSSMode enum -> SI::DLSSMode underlying uint32)
        .def_prop_rw("dlss_mode",
            [](SampleUIData& s) { return int(s.DLSSMode); },
            [](SampleUIData& s, int v) { s.DLSSMode = donut::app::StreamlineInterface::DLSSMode(v); },
            "DLSS quality preset (rtxpt.DLSSMode).\n"
            "Off, MaxPerformance, Balanced, MaxQuality, UltraPerformance, UltraQuality, DLAA.")
        .def_rw("dlss_lod_bias_use_override", &SampleUIData::DLSSLodBiasUseOverride)
        .def_rw("dlss_lod_bias_override",     &SampleUIData::DLSSLodBiasOverride)
        .def_rw("dlss_always_use_extents",    &SampleUIData::DLSSAlwaysUseExtents)

        // DLSS-G (frame generation)
        .def_prop_rw("dlss_fg_mode",
            [](SampleUIData& s) { return int(s.DLSSFGMode); },
            [](SampleUIData& s, int v) { s.DLSSFGMode = donut::app::StreamlineInterface::DLSSGMode(v); },
            "DLSS frame generation mode (rtxpt.DLSSFGMode).")
        .def_rw("dlss_fg_multiplier",            &SampleUIData::DLSSFGMultiplier)
        .def_rw("dlss_fg_num_frames_to_generate",&SampleUIData::DLSSFGNumFramesToGenerate)
        .def_rw("dlss_fg_max_num_frames_to_generate",&SampleUIData::DLSSFGMaxNumFramesToGenerate)

        // DLSS-RR (ray reconstruction)
        .def_prop_rw("dlss_rr_preset",
            [](SampleUIData& s) { return int(s.DLSRRPreset); },
            [](SampleUIData& s, int v) { s.DLSRRPreset = donut::app::StreamlineInterface::DLSSRRPreset(v); },
            "DLSS-RR preset (rtxpt.DLSSRRPreset).")
        .def_rw("dlss_rr_micro_jitter",          &SampleUIData::DLSSRRMicroJitter)
        .def_rw("dlss_rr_brightness_clamp_k",    &SampleUIData::DLSSRRBrightnessClampK)
        .def_rw("disable_restirs_with_dlss_rr",  &SampleUIData::DisableReSTIRsWithDLSSRR)

        // Reflex (low latency)
        .def_rw("reflex_mode",                   &SampleUIData::ReflexMode,
                "NVIDIA Reflex mode (rtxpt.ReflexMode).")
        .def_rw("reflex_capped_fps",             &SampleUIData::ReflexCappedFps)

        // --- Read-only support flags -------------------------------------
        .def_ro("is_dlss_supported",     &SampleUIData::IsDLSSSuported)
        .def_ro("is_dlss_fg_supported",  &SampleUIData::IsDLSSFGSupported)
        .def_ro("is_dlss_rr_supported",  &SampleUIData::IsDLSSRRSupported)
        .def_ro("is_reflex_supported",   &SampleUIData::IsReflexSupported)
#endif // DONUT_WITH_STREAMLINE

        // --- Standalone NRD denoiser (realtime, RealtimeAA != DLSS-RR) ---
        .def_rw("standalone_denoiser",           &SampleUIData::StandaloneDenoiser,
                "Enable NRD denoiser in realtime mode (no effect with DLSS-RR).")
        .def_rw("denoiser_radiance_clamp_k",     &SampleUIData::DenoiserRadianceClampK)

        // --- OIDN reference-mode denoiser --------------------------------
        .def_rw("oidn_enabled",            &SampleUIData::ReferenceOIDNDenoiser,
                "(Reference mode) Run Intel Open Image Denoise after accumulation reaches the SPP target.")
        .def_rw("oidn_use_gpu",            &SampleUIData::ReferenceOIDNUseGPU,
                "Use OIDN GPU device (CUDA/HIP/SYCL) when available, else CPU.")
        .def_rw("oidn_passes",             &SampleUIData::ReferenceOIDNPasses,
                "Auxiliary guide passes (rtxpt.OidnPasses).")
        .def_rw("oidn_prefilter",          &SampleUIData::ReferenceOIDNPrefilter,
                "Prefilter quality for guide passes (rtxpt.OidnPrefilter).")
        .def_rw("oidn_quality",            &SampleUIData::ReferenceOIDNQuality,
                "Beauty filter quality (rtxpt.OidnQuality).")
        .def_rw("oidn_changed",            &SampleUIData::ReferenceOIDNDenoiserChanged,
                "Set to True after editing any OIDN parameter to force a redenoise.\n"
                "Cleared automatically by the renderer.")
        .def("oidn_apply", [](SampleUIData& s) { s.ReferenceOIDNDenoiserChanged = true; },
             "Mark OIDN parameters dirty so the next accumulation completion runs the filter again.")

        .def_rw("environment_map",               &SampleUIData::EnvironmentMapParams,
                nb::rv_policy::reference_internal,
                "EnvironmentMapParams structure (intensity, tint, rotation, enabled).")
        ;

    // --- Sample (top-level renderer access) -------------------------------
    nb::class_<Sample>(m, "Sample",
        "RTXPT renderer instance. In embed mode use rtxpt.app(); in extension\n"
        "mode use Renderer.app to retrieve the underlying instance.")
        .def_prop_ro("settings", [](Sample& self) -> SampleUIData* {
                // SampleUIData is a global g_sampleUIData. Same identity in
                // embed and extension modes since the linked SampleUI binary
                // declares the exact same translation-unit-local global.
                return &g_sampleUIData;
            }, nb::rv_policy::reference,
            "Live `Settings` mirror of the current UI state.")

        .def_prop_ro("scene_name",  [](Sample& self) { return self.GetCurrentSceneName(); })
        .def_prop_ro("available_scenes", [](Sample& self) { return self.GetAvailableScenes(); })

        .def("set_scene", [](Sample& self, const std::string& name, bool forceReload)
            {
                self.SetCurrentScene(name, forceReload);
            },
            nb::arg("scene_name"), nb::arg("force_reload") = false,
            "Switch to a different scene file from rtxpt.Sample.available_scenes.")

        .def("load_gaussian_splats", [](Sample& self, const std::string& fileName, bool convertRdfToDonut)
            {
                return self.LoadGaussianSplatFile(fileName, convertRdfToDonut);
            },
            nb::arg("file_name"), nb::arg("convert_rdf_to_donut") = true,
            "Load a 3DGS .ply file and rasterize it over the current scene.")

        .def_prop_ro("gaussian_splat_count", [](Sample& self) { return self.GetGaussianSplatCount(); })
        .def_prop_ro("gaussian_splat_file_name", [](Sample& self) { return self.GetGaussianSplatFileName(); })

        .def("get_materials", [](Sample& self) {
                std::vector<std::shared_ptr<PTMaterial>> result;
                if (!self.GetScene())
                    return result;
                for (const auto& mat : self.GetScene()->GetSceneGraph()->GetMaterials())
                {
                    if (auto pt = PTMaterial::SafeCast(mat))
                        result.push_back(pt);
                }
                return result;
            }, "Return every PTMaterial in the current scene.")

        .def("find_material", [](Sample& self, const std::string& name) -> std::shared_ptr<PTMaterial> {
                if (!self.GetScene())
                    return nullptr;
                for (const auto& mat : self.GetScene()->GetSceneGraph()->GetMaterials())
                {
                    auto pt = PTMaterial::SafeCast(mat);
                    if (pt && (pt->Name == name || pt->UniqueName == name))
                        return pt;
                }
                return nullptr;
            }, nb::arg("name"), "Look up a material by Name or UniqueName.")

        .def("find_material_by_id", [](Sample& self, int materialId) -> std::shared_ptr<PTMaterial> {
                return PTMaterial::SafeCast(self.FindMaterial(materialId));
            }, nb::arg("material_id"))

        .def("get_lights", [](Sample& self) {
                std::vector<std::shared_ptr<Light>> out;
                if (!self.GetScene())
                    return out;
                for (const auto& l : self.GetScene()->GetSceneGraph()->GetLights())
                    out.push_back(l);
                return out;
            }, "Return every Light in the current scene.")

        .def("find_light", [](Sample& self, const std::string& name) -> std::shared_ptr<Light> {
                if (!self.GetScene())
                    return nullptr;
                for (const auto& l : self.GetScene()->GetSceneGraph()->GetLights())
                {
                    if (l->GetNode() && l->GetNode()->GetName() == name)
                        return l;
                }
                return nullptr;
            }, nb::arg("name"))

        .def("set_environment_map", [](Sample& self, const std::string& path) {
                self.SetEnvMapOverrideSource(path);
            }, nb::arg("path"))

        .def("get_camera_pos_dir_up", [](Sample& self) {
                return self.GetCurrentCameraPosDirUp();
            }, "Returns a comma-separated string of pos.xyz, dir.xyz, up.xyz.")

        .def("set_camera_pos_dir_up", [](Sample& self, const std::string& v) {
                return self.SetCurrentCameraPosDirUp(v);
            }, nb::arg("pos_dir_up"))

        .def("set_camera_fov", [](Sample& self, float fov) { self.SetCameraVerticalFOV(donut::math::radians(fov)); },
            nb::arg("vertical_fov_degrees"))

        .def("get_camera_fov", [](Sample& self) { return self.GetCameraVerticalFOV(); })

        .def("save_current_camera",  [](Sample& self) { self.SaveCurrentCamera(); })
        .def("load_current_camera",  [](Sample& self) { self.LoadCurrentCamera(); })

        .def("request_shader_reload",  [](Sample& self) { g_sampleUIData.ShaderReloadRequested = true; })
        .def("request_accel_rebuild",  [](Sample& self) { g_sampleUIData.AccelerationStructRebuildRequested = true; })
        .def("reset_accumulation",     [](Sample& self) { g_sampleUIData.ResetAccumulation = true; })

        .def("set_realtime_mode", [](Sample&, bool standaloneDenoiser, int realtimeAA)
            {
                if (!g_sampleUIData.RealtimeMode)
                    g_sampleUIData.ResetAccumulation = true;
                g_sampleUIData.RealtimeMode      = true;
                g_sampleUIData.StandaloneDenoiser = standaloneDenoiser;
                g_sampleUIData.RealtimeAA         = realtimeAA;
            },
            nb::arg("standalone_denoiser") = true,
            nb::arg("realtime_aa") = 2 /*DLSS*/,
            "Switch to realtime path tracing.\n"
            "Args:\n"
            "    standalone_denoiser: enable NRD (no effect with DLSS-RR)\n"
            "    realtime_aa        : 0=Off, 1=TAA, 2=DLSS, 3=DLSS-RR")

        .def("set_reference_mode", [](Sample&, int spp, bool oidn, int oidnQuality, int oidnPasses, int oidnPrefilter)
            {
                if (g_sampleUIData.RealtimeMode)
                    g_sampleUIData.ResetAccumulation = true;
                g_sampleUIData.RealtimeMode             = false;
                if (spp > 0)
                    g_sampleUIData.AccumulationTarget   = spp;
                g_sampleUIData.ReferenceOIDNDenoiser    = oidn;
                g_sampleUIData.ReferenceOIDNQuality     = oidnQuality;
                g_sampleUIData.ReferenceOIDNPasses      = oidnPasses;
                g_sampleUIData.ReferenceOIDNPrefilter   = oidnPrefilter;
                g_sampleUIData.ReferenceOIDNDenoiserChanged = true;
            },
            nb::arg("spp") = 0,
            nb::arg("oidn") = false,
            nb::arg("oidn_quality")   = 1 /*Balanced*/,
            nb::arg("oidn_passes")    = 1 /*Albedo*/,
            nb::arg("oidn_prefilter") = 1 /*Fast*/,
            "Switch to reference / accumulation rendering.\n"
            "Args:\n"
            "    spp           : reference SPP target (0 to keep current).\n"
            "    oidn          : run OIDN once accumulation hits the SPP target.\n"
            "    oidn_quality  : rtxpt.OidnQuality (0=Fast, 1=Balanced, 2=High)\n"
            "    oidn_passes   : rtxpt.OidnPasses (0=ColorOnly, 1=Albedo, 2=AlbedoNormal)\n"
            "    oidn_prefilter: rtxpt.OidnPrefilter (0=None, 1=Fast, 2=Accurate)")

        .def_prop_ro("accumulation_completed",
            [](Sample& self) { return self.AccumulationCompleted(); })
        .def_prop_ro("accumulation_sample_index",
            [](Sample& self) { return self.GetAccumulationSampleIndex(); })
        ;
}

} // namespace rtxpt_py

#endif // RTXPT_WITH_PYTHON
