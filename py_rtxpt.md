# RTXPT Python API Reference

本文档记录当前 `rtxpt` Python 绑定的使用方式。API 来源主要是：

- `Rtxpt/Python/PythonBindingsCore.cpp`
- `Rtxpt/Python/PythonBindings_Extension.cpp`
- `Rtxpt/Python/PythonBindings_Embed.cpp`
- `Rtxpt/Python/RenderSession.*`

## Two Usage Modes

`rtxpt` 模块有两种运行模式，共享大部分类型：

| Mode | How to use | Typical use |
| --- | --- | --- |
| `extension` | 在独立 Python 进程里 `import rtxpt`，创建 `rtxpt.Renderer(...)` | 离线渲染、批处理、截图、自动化测试、3DGS 快速验证 |
| `embed` | 从正在运行的 `Rtxpt.exe` 内部脚本系统 `import rtxpt` | 实时调参、调试、场景/材质/灯光热修改 |

运行时可通过：

```python
import rtxpt
print(rtxpt.MODE)  # "extension" or "embed"
```

在 extension mode 里，`rtxpt.Renderer` 会创建自己的窗口/设备/scene。  
在 embed mode 里，没有 `Renderer` 类；使用 `rtxpt.app()` 获取当前 `Rtxpt.exe` 内的 renderer。

## Import Setup

构建后，Python extension 输出在 `bin/`：

```text
bin/rtxpt.cp311-win_amd64.pyd
```

独立 Python 脚本需要把 `bin/` 放进 `sys.path` 或 `PYTHONPATH`：

```python
import sys
sys.path.insert(0, r"D:\ProgramCode\C++\RTXPT\bin")

import rtxpt
```

也可以参考 `Rtxpt/Python/Examples/test_splat_interactive.py` 中的 `configure_import_path()`。

## Quick Examples

### Headless Reference Render

```python
import rtxpt

with rtxpt.Renderer(
    width=1280,
    height=720,
    headless=True,
    scene="bistro-programmer-art.scene.json",
    realtime=False,
    accumulation_target=64,
) as r:
    r.settings.enable_tone_mapping = True
    frames = r.step_until_accumulated()
    print("frames:", frames)
    r.save_screenshot("frame.png")
```

### Windowed Interactive Loop

```python
import time
import rtxpt

r = rtxpt.Renderer(
    width=1280,
    height=720,
    headless=False,
    scene="bistro-programmer-art.scene.json",
    realtime=True,
    accumulation_target=1,
)

try:
    while r.step(-1.0):  # returns False if the window is closed
        time.sleep(0.001)
finally:
    r.close()
```

### Load 3D Gaussian Splats

```python
import rtxpt

r = rtxpt.Renderer(
    width=1280,
    height=720,
    headless=False,
    realtime=True,
    scene="bistro-programmer-art.scene.json",
    gaussian_splat_file=r"D:\ScanVideo\chuan\splats.ply",
    gaussian_splat_convert_rdf_to_donut=True,
)

s = r.settings
s.enable_gaussian_splats = True
s.gaussian_splat_sorting_mode = int(rtxpt.GaussianSplatSortMode.GpuSort)
s.gaussian_splat_sh_format = int(rtxpt.GaussianSplatStorageFormat.Uint8)
s.gaussian_splat_rgba_format = int(rtxpt.GaussianSplatStorageFormat.Uint8)
s.gaussian_splat_scale = 1.0
s.gaussian_splat_alpha_scale = 1.0
s.gaussian_splat_brightness = 1.0

while r.step(-1.0):
    pass
```

### 3DGS Reference / Realtime Batch Test

`3dgs_example.py` renders the same PLY twice:

- Reference mode accumulates 32 spp, then applies OIDN and writes `reference_oidn.png`.
- Realtime mode steps 32 frames and uses DLSS-RR when supported, falling back to DLSS/TAA/off, then writes `realtime_<aa>.png`.
- The default 3DGS sorting mode is GPU sort. Pass `--sorting stochastic` to compare with stochastic splats.

```powershell
python .\Rtxpt\Python\Examples\3dgs_example.py ^
    --ply D:/ScanVideo/chuan/splats.ply ^
    --out-dir 3dgs_chuan_gpu_sort_out
```

Useful camera overrides:

```powershell
python .\Rtxpt\Python\Examples\3dgs_example.py ^
    --ply D:/ScanVideo/chuan/splats.ply ^
    --out-dir 3dgs_chuan_out ^
    --distance-scale 4.0 ^
    --side front
```

### Edit Materials

```python
import rtxpt

r = rtxpt.Renderer(scene="bistro-programmer-art.scene.json", headless=True)
mat = r.app.find_material("SomeMaterialName")
if mat:
    mat.base_color = (1.0, 0.2, 0.1)
    mat.roughness = 0.35
    mat.metalness = 0.0
    mat.mark_dirty()

r.step_n(4)
r.save_screenshot("material_edit.png")
r.close()
```

### Edit Lights

```python
import rtxpt

r = rtxpt.Renderer(scene="bistro-programmer-art.scene.json", headless=True)
for light in r.app.get_lights():
    print(light.name, light.light_type)
    light.color = (1.0, 0.9, 0.75)

sun = r.app.find_light("Sun")
if sun:
    sun.direction = (0.0, -1.0, 0.2)

r.step_n(8)
r.save_screenshot("lights.png")
r.close()
```

## Module-Level API

These functions exist in both embed and extension mode unless noted.

| API | Return | Notes |
| --- | --- | --- |
| `rtxpt.MODE` | `str` | `"embed"` or `"extension"`. |
| `rtxpt.app()` | `Sample` | Current renderer. In extension mode, returns the most recently created `Renderer`'s `Sample`. |
| `rtxpt.settings()` | `Settings` | Shortcut to global live UI/settings state. Same object as `rtxpt.app().settings`. |
| `rtxpt.log_info(message)` | `None` | Writes to RTXPT log at info level. |
| `rtxpt.log_warning(message)` | `None` | Writes to RTXPT log at warning level. |
| `rtxpt.log_error(message)` | `None` | Writes to RTXPT log at error level. |
| `rtxpt.Renderer(...)` | `Renderer` | Extension mode only. Creates a standalone renderer/device/window or headless backbuffer. |

## Enums

All enums are arithmetic, so `int(enum_value)` works and enum values can be assigned to int-backed settings fields.

### `PathTracerMode`

| Value | Int | Meaning |
| --- | ---: | --- |
| `rtxpt.PathTracerMode.Realtime` | `0` | Realtime path tracing mode. |
| `rtxpt.PathTracerMode.Reference` | `1` | Reference accumulation mode. |

### `RealtimeAA`

| Value | Int | Meaning |
| --- | ---: | --- |
| `rtxpt.RealtimeAA.Off` | `0` | No realtime AA/upscaler. |
| `rtxpt.RealtimeAA.TAA` | `1` | Temporal AA. |
| `rtxpt.RealtimeAA.DLSS` | `2` | DLSS Super Resolution. |
| `rtxpt.RealtimeAA.DLSS_RR` | `3` | DLSS Ray Reconstruction. |

### `DLSSMode`

| Value | Int |
| --- | ---: |
| `Off` | `0` |
| `MaxPerformance` | `1` |
| `Balanced` | `2` |
| `MaxQuality` | `3` |
| `UltraPerformance` | `4` |
| `UltraQuality` | `5` |
| `DLAA` | `6` |

### `DLSSFGMode`

| Value | Int |
| --- | ---: |
| `Off` | `0` |
| `On` | `1` |
| `Auto` | `2` |

### `DLSSRRPreset`

| Value | Int |
| --- | ---: |
| `Default` | `0` |
| `PresetA` ... `PresetH` | `1` ... `8` |

### `ReflexMode`

| Value | Int |
| --- | ---: |
| `Off` | `0` |
| `LowLatency` | `1` |
| `LowLatencyWithBoost` | `2` |

### OIDN Enums

| Enum | Values |
| --- | --- |
| `OidnPasses` | `ColorOnly=0`, `Albedo=1`, `AlbedoNormal=2` |
| `OidnPrefilter` | `None_=0`, `Fast=1`, `Accurate=2` |
| `OidnQuality` | `Fast=0`, `Balanced=1`, `High=2` |

### 3DGS Enums

| Enum | Values |
| --- | --- |
| `GaussianSplatSortMode` | `GpuSort=0`, `StochasticSplats=1` |
| `GaussianSplatStorageFormat` | `Float32=0`, `Float16=1`, `Uint8=2` |
| `GaussianSplatFrustumCulling` | `Disabled=0`, `AtDistanceStage=1`, `AtRasterStage=2` |
| `GaussianSplatShadowMode` | `Disabled=0`, `Hard=1`, `Soft=2` |

## `Renderer` Class

Extension mode only.

### Constructor

```python
rtxpt.Renderer(
    width=1920,
    height=1080,
    headless=True,
    vulkan=False,
    adapter_index=-1,
    debug=False,
    scene="",
    realtime=False,
    accumulation_target=64,
    gaussian_splat_file="",
    gaussian_splat_convert_rdf_to_donut=True,
    gaussian_splat_depth_test=True,
    gaussian_splat_scale=1.0,
    gaussian_splat_alpha_scale=1.0,
    gaussian_splat_brightness=1.0,
    gaussian_splat_alpha_cull_threshold=1.0 / 255.0,
)
```

| Argument | Meaning |
| --- | --- |
| `width`, `height` | Initial backbuffer/window size. |
| `headless` | `True`: offscreen backbuffers, no OS window. `False`: create a window and swap chain. |
| `vulkan` | `False` uses DX12. `True` requests Vulkan when available. |
| `adapter_index` | GPU index, `-1` means default adapter. |
| `debug` | Enable graphics debug settings. |
| `scene` | Scene file path/name. Relative paths are resolved from `Assets/`. |
| `realtime` | Start in realtime mode if `True`, reference mode if `False`. |
| `accumulation_target` | Reference SPP target. |
| `gaussian_splat_*` | Optional 3DGS PLY overlay and rasterization settings. |

### Methods / Properties

| API | Return | Notes |
| --- | --- | --- |
| `close()` | `None` | Tears down renderer/device. Also called by destructor/context manager. |
| `load_scene(scene_name, wait_until_ready=True)` | `bool` | Switch scene. |
| `load_gaussian_splats(file_name, convert_rdf_to_donut=True)` | `bool` | Load a `.ply` 3DGS overlay. |
| `step(dt=-1.0)` | `bool` | Render one frame. Returns `False` on failure or when window close is requested. |
| `step_n(frames)` | `bool` | Render exactly N frames unless `step()` fails. |
| `step_until_accumulated(max_frames=0)` | `int` | Reset accumulation and step until accumulation completes, or until `max_frames` if positive. |
| `save_screenshot(output_path)` | `bool` | Save current backbuffer to PNG/JPG/BMP/TGA. |
| `set_camera(position, direction, up=(0, 1, 0))` | `bool` | Triples can be lists/tuples of 3 floats. |
| `set_camera_fov(vertical_fov_degrees)` | `None` | Set vertical FOV in degrees. |
| `app` | `Sample` | Underlying renderer instance. |
| `settings` | `Settings` | Live UI/settings state. |

`Renderer` supports context manager syntax:

```python
with rtxpt.Renderer(headless=True) as r:
    r.step_n(8)
```

## `Sample` Class

Top-level renderer instance. In extension mode, access it through `renderer.app`; in embed mode, use `rtxpt.app()`.

### Read-Only Properties

| Property | Type | Notes |
| --- | --- | --- |
| `settings` | `Settings` | Live settings object. |
| `scene_name` | `str` | Current scene name. |
| `available_scenes` | `list[str]` | Scene files discovered by the app. |
| `gaussian_splat_count` | `int` | Loaded 3DGS splat count. |
| `gaussian_splat_file_name` | `str` | Loaded 3DGS file path. |
| `accumulation_completed` | `bool` | Whether reference accumulation is complete. |
| `accumulation_sample_index` | `int` | Current accumulation sample index. |

### Scene / Assets

| API | Return | Notes |
| --- | --- | --- |
| `set_scene(scene_name, force_reload=False)` | `None` | Switch scene. |
| `load_gaussian_splats(file_name, convert_rdf_to_donut=True)` | `bool` | Load 3DGS `.ply`. |
| `set_environment_map(path)` | `None` | Override scene environment map source. |

### Materials

| API | Return | Notes |
| --- | --- | --- |
| `get_materials()` | `list[Material]` | All `PTMaterial` materials in the current scene. |
| `find_material(name)` | `Material | None` | Match by `Name` or `UniqueName`. |
| `find_material_by_id(material_id)` | `Material | None` | Lookup by material ID. |

### Lights

| API | Return | Notes |
| --- | --- | --- |
| `get_lights()` | `list[Light]` | All lights in current scene. |
| `find_light(name)` | `Light | None` | Match by scene node name. |

### Camera

| API | Return | Notes |
| --- | --- | --- |
| `get_camera_pos_dir_up()` | `str` | Comma-separated `pos.xyz,dir.xyz,up.xyz`. |
| `set_camera_pos_dir_up(pos_dir_up)` | `bool` | Input format matches `get_camera_pos_dir_up()`. |
| `set_camera_fov(vertical_fov_degrees)` | `None` | Takes degrees. |
| `get_camera_fov()` | `float` | Returns current internal value in radians. |
| `save_current_camera()` | `None` | Save camera through app's camera persistence path. |
| `load_current_camera()` | `None` | Restore saved camera. |

Use `Renderer.set_camera()` when working in extension mode; it is simpler than building the comma-separated string manually.

### Runtime Requests

| API | Effect |
| --- | --- |
| `request_shader_reload()` | Requests shader reload. |
| `request_accel_rebuild()` | Requests acceleration structure rebuild. |
| `reset_accumulation()` | Resets reference accumulation. |

### Mode Helpers

```python
app.set_realtime_mode(
    standalone_denoiser=True,
    realtime_aa=int(rtxpt.RealtimeAA.DLSS),
)

app.set_reference_mode(
    spp=128,
    oidn=True,
    oidn_quality=int(rtxpt.OidnQuality.Balanced),
    oidn_passes=int(rtxpt.OidnPasses.Albedo),
    oidn_prefilter=int(rtxpt.OidnPrefilter.Fast),
)
```

| API | Notes |
| --- | --- |
| `set_realtime_mode(standalone_denoiser=True, realtime_aa=2)` | Sets realtime mode. `realtime_aa`: `0=Off`, `1=TAA`, `2=DLSS`, `3=DLSS_RR`. |
| `set_reference_mode(spp=0, oidn=False, oidn_quality=1, oidn_passes=1, oidn_prefilter=1)` | Sets reference mode. `spp=0` keeps current target. |

## `Settings` Class

`Settings` mirrors the live ImGui UI state. Most fields are writable and take effect on subsequent frames.

### General

| Property | Type | Notes |
| --- | --- | --- |
| `show_ui` | `bool` | Show/hide UI. |
| `enable_animations` | `bool` | Scene animation toggle. |
| `enable_vsync` | `bool` | VSync toggle. |
| `fps_limiter` | `float/int` | FPS limiter value. |

### Path Tracing Mode / Accumulation

| Property | Type | Notes |
| --- | --- | --- |
| `realtime_mode` | `bool` | `True` realtime, `False` reference. |
| `path_tracer_mode` | `int/PathTracerMode` | `Realtime=0`, `Reference=1`; changing it resets accumulation. |
| `realtime_samples_per_pixel` | `int` | SPP in realtime mode. |
| `accumulation_target` | `int` | Reference SPP target. |
| `reset_accumulation` | `bool` | Set `True` to reset accumulation. |
| `accumulation_aa` | `bool/int` | Accumulation AA toggle/setting. |
| `accumulation_prewarm_realtime_caches` | `bool` | Prewarm realtime caches before accumulation. |

### Path Tracer Knobs

| Property | Type |
| --- | --- |
| `bounce_count` | `int` |
| `diffuse_bounce_count` | `int` |
| `enable_russian_roulette` | `bool` |
| `texture_lod_bias` | `float` |

### NEE / ReSTIR

| Property | Type | Notes |
| --- | --- | --- |
| `use_nee` | `bool` | Next event estimation. |
| `nee_type` | `int` | `0=uniform`, `1=power-based`, `2=NEE-AT`. |
| `nee_candidate_samples` | `int` | Candidate sample count. |
| `nee_full_samples` | `int` | Full sample count. |
| `nee_mis_type` | `int` | MIS mode. |
| `use_restir_di` | `bool` | ReSTIR direct illumination. |
| `use_restir_gi` | `bool` | ReSTIR global illumination. |

### Camera

| Property | Type |
| --- | --- |
| `camera_aperture` | `float` |
| `camera_focal_distance` | `float` |
| `camera_move_speed` | `float` |

### Firefly Filters

| Property | Type |
| --- | --- |
| `realtime_firefly_filter_enabled` | `bool` |
| `realtime_firefly_filter_threshold` | `float` |
| `reference_firefly_filter_enabled` | `bool` |
| `reference_firefly_filter_threshold` | `float` |

### Tone Mapping / Bloom

| Property | Type |
| --- | --- |
| `enable_tone_mapping` | `bool` |
| `enable_bloom` | `bool` |
| `bloom_intensity` | `float` |
| `bloom_radius` | `float` |

### 3D Gaussian Splats

The table below lists the Python-facing 3DGS settings that are currently wired into the renderer. Some legacy UI/Python fields still exist in the bindings for compatibility, but are no longer consumed by the render path and are intentionally omitted here.

| Property | Type | Notes |
| --- | --- | --- |
| `enable_gaussian_splats` | `bool` | Enables splat overlay. |
| `gaussian_splat_depth_test` | `bool` | Test against scene depth. |
| `gaussian_splat_sorting_mode` | `int/GaussianSplatSortMode` | `GpuSort` or `StochasticSplats`. |
| `gaussian_splat_sh_format` | `int/GaussianSplatStorageFormat` | SH payload storage format. |
| `gaussian_splat_rgba_format` | `int/GaussianSplatStorageFormat` | RGBA payload storage format. |
| `gaussian_splat_use_aabbs` | `bool` | Use AABB-based splat shadow acceleration data. |
| `gaussian_splat_use_tlas_instances` | `bool` | Use TLAS instances for splat shadow acceleration. |
| `gaussian_splat_blas_compaction` | `bool` | Enable BLAS compaction for splat shadow acceleration data. |
| `gaussian_splat_mip_antialiasing` | `bool` | Enable splat mip antialiasing path. |
| `gaussian_splat_frustum_culling` | `int/GaussianSplatFrustumCulling` | Frustum culling stage. |
| `gaussian_splat_frustum_dilation` | `float` | Culling frustum dilation. |
| `gaussian_splat_screen_size_culling` | `bool` | Enable screen-size splat culling. |
| `gaussian_splat_min_pixel_coverage` | `float` | Minimum pixel coverage for screen-size culling. |
| `gaussian_splat_scale` | `float` | Projected footprint scale. |
| `gaussian_splat_alpha_scale` | `float` | Opacity multiplier. |
| `gaussian_splat_brightness` | `float` | Color multiplier. |
| `gaussian_splat_alpha_cull_threshold` | `float` | Cull low-alpha splats. |
| `gaussian_splat_translation` | `(x, y, z)` | World-space splat object translation. |
| `gaussian_splat_rotation_euler_deg` | `(x, y, z)` | Splat object Euler rotation in degrees. |
| `gaussian_splat_object_scale` | `(x, y, z)` | Splat object non-uniform scale. |
| `gaussian_splat_shadows` | `bool` | Enable splat shadow integration. |
| `gaussian_splat_hybrid_shadows` | `bool` | Alias for `gaussian_splat_shadows`. |
| `gaussian_splat_shadows_mode` | `int/GaussianSplatShadowMode` | Disabled, hard, or soft splat shadows. |
| `gaussian_splat_shadow_strength` | `float` | Shadow opacity/strength. |
| `gaussian_splat_shadow_soft_radius` | `float` | Soft shadow radius. |
| `gaussian_splat_shadow_soft_sample_count` | `int` | Soft shadow sample count. |
| `gaussian_splat_rtx_kernel_degree` | `int` | RTX splat kernel degree. |
| `gaussian_splat_rtx_adaptive_clamp` | `bool` | Enable adaptive RTX alpha clamp. |
| `gaussian_splat_rtx_particle_shadow_offset` | `float` | RTX particle shadow offset. |
| `gaussian_splat_count` | `int` | Read-only. |
| `gaussian_splat_file_name` | `str` | Read-only. |

### Realtime AA / DLSS / Reflex

Availability depends on build options and hardware support.

| Property | Type | Notes |
| --- | --- | --- |
| `realtime_aa` | `int/RealtimeAA` | `0=Off`, `1=TAA`, `2=DLSS`, `3=DLSS_RR`. |
| `dlss_mode` | `int/DLSSMode` | DLSS quality preset. |
| `dlss_lod_bias_use_override` | `bool` | Override DLSS texture LOD bias. |
| `dlss_lod_bias_override` | `float` | LOD bias override. |
| `dlss_always_use_extents` | `bool` | Use DLSS extents mode. |
| `dlss_fg_mode` | `int/DLSSFGMode` | DLSS frame generation mode. |
| `dlss_fg_multiplier` | `int` | Frame generation multiplier. |
| `dlss_fg_num_frames_to_generate` | `int` | Current generated frame count. |
| `dlss_fg_max_num_frames_to_generate` | `int` | Max generated frame count. |
| `dlss_rr_preset` | `int/DLSSRRPreset` | DLSS Ray Reconstruction preset. |
| `dlss_rr_micro_jitter` | `bool/float` | DLSS-RR micro jitter setting. |
| `dlss_rr_brightness_clamp_k` | `float` | Brightness clamp factor. |
| `disable_restirs_with_dlss_rr` | `bool` | Disable ReSTIR features with DLSS-RR. |
| `reflex_mode` | `int/ReflexMode` | NVIDIA Reflex mode. |
| `reflex_capped_fps` | `float/int` | Reflex FPS cap. |

Read-only support flags:

| Property | Type |
| --- | --- |
| `is_dlss_supported` | `bool` |
| `is_dlss_fg_supported` | `bool` |
| `is_dlss_rr_supported` | `bool` |
| `is_reflex_supported` | `bool` |

### Denoisers

Realtime / NRD:

| Property | Type | Notes |
| --- | --- | --- |
| `standalone_denoiser` | `bool` | NRD denoiser in realtime mode; no effect with DLSS-RR. |
| `denoiser_radiance_clamp_k` | `float` | NRD radiance clamp. |

Reference / OIDN:

| Property | Type | Notes |
| --- | --- | --- |
| `oidn_enabled` | `bool` | Run OIDN when accumulation completes. |
| `oidn_use_gpu` | `bool` | Use OIDN GPU device when available. |
| `oidn_passes` | `int/OidnPasses` | Auxiliary guide passes. |
| `oidn_prefilter` | `int/OidnPrefilter` | Guide prefilter quality. |
| `oidn_quality` | `int/OidnQuality` | Beauty filter quality. |
| `oidn_changed` | `bool` | Set true after edits; renderer clears it. |
| `oidn_apply()` | method | Marks OIDN parameters dirty. |

### Environment Map Runtime Parameters

`settings.environment_map` is an `EnvironmentMapParams` object:

| Property | Type |
| --- | --- |
| `tint_color` | `(r, g, b)` |
| `intensity` | `float` |
| `rotation_xyz` | `(x, y, z)` |
| `enabled` | `bool` |

## `Material` Class

Returned by `Sample.get_materials()`, `Sample.find_material()`, and `Sample.find_material_by_id()`.

Read-only identifiers:

| Property | Type |
| --- | --- |
| `name` | `str` |
| `model_name` | `str` |
| `unique_name` | `str` |

Editable properties automatically mark GPU data dirty:

| Property | Type |
| --- | --- |
| `base_color` | `(r, g, b)` |
| `specular_color` | `(r, g, b)` |
| `emissive_color` | `(r, g, b)` |
| `emissive_intensity` | `float` |
| `metalness` | `float` |
| `roughness` | `float` |
| `opacity` | `float` |
| `transmission_factor` | `float` |
| `diffuse_transmission_factor` | `float` |
| `normal_texture_scale` | `float` |
| `ior` | `float` |
| `alpha_cutoff` | `float` |
| `volume_attenuation_distance` | `float` |
| `volume_attenuation_color` | `(r, g, b)` |
| `nested_priority` | `int` |
| `use_specular_gloss` | `bool` |
| `enable_alpha_testing` | `bool` |
| `enable_transmission` | `bool` |
| `thin_surface` | `bool` |
| `exclude_from_nee` | `bool` |
| `enable_as_analytic_light_proxy` | `bool` |
| `skip_render` | `bool` |
| `metalness_in_red_channel` | `bool` |
| `enable_base_texture` | `bool` |
| `enable_orm_texture` | `bool` |
| `enable_normal_texture` | `bool` |
| `enable_emissive_texture` | `bool` |
| `enable_transmission_texture` | `bool` |

Methods:

| API | Notes |
| --- | --- |
| `mark_dirty()` | Force material GPU buffer refresh next frame. |

## `Light` Classes

Base class: `Light`.

| Property | Type | Notes |
| --- | --- | --- |
| `light_type` | implementation enum/int | Underlying Donut light type. |
| `name` | `str` | Scene node name. Read-only. |
| `color` | `(r, g, b)` | Writable. |
| `position` | `(x, y, z)` | Writable. |
| `direction` | `(x, y, z)` | Writable. |

Derived classes expose extra properties depending on actual light type.

### `DirectionalLight`

| Property | Type | Notes |
| --- | --- | --- |
| `irradiance` | `float` | Target illuminance, multiplied by `color`. |
| `angular_size` | `float` | Apparent angular size in degrees. |

### `SpotLight`

| Property | Type |
| --- | --- |
| `intensity` | `float` |
| `radius` | `float` |
| `range` | `float` |
| `inner_angle` | `float` |
| `outer_angle` | `float` |

### `PointLight`

| Property | Type |
| --- | --- |
| `intensity` | `float` |
| `radius` | `float` |
| `range` | `float` |

### `EnvironmentLight`

| Property | Type |
| --- | --- |
| `radiance_scale` | `(r, g, b)` |
| `rotation` | float / implementation-specific value |
| `path` | `str` |

For common environment tweaks, prefer `settings.environment_map` and `Sample.set_environment_map(path)`.

## Embedded Mode Notes

In embedded mode, scripts run inside `Rtxpt.exe`:

```powershell
Rtxpt.exe --pythonScript Rtxpt/Python/Examples/example_basic.py
Rtxpt.exe --pythonExpr "import rtxpt; print(rtxpt.app().scene_name)"
```

Inside the app, the Python panel can run inline code. Typical script shape:

```python
import rtxpt

app = rtxpt.app()
s = rtxpt.settings()

s.realtime_mode = True
s.realtime_aa = int(rtxpt.RealtimeAA.TAA)
app.reset_accumulation()
```

Do not create `rtxpt.Renderer` in embed mode; the running app already owns the renderer.

## Extension Mode Notes

In extension mode, every `Renderer` owns a GPU device and scene. Use `close()` or a context manager so GPU resources are released promptly.

```python
with rtxpt.Renderer(headless=True, scene="...") as r:
    ...
```

For windowed extension usage:

- `headless=False` opens a GLFW window.
- `Renderer.step()` must be called repeatedly to pump events and render frames.
- Clicking the window close button makes `step()` return `False`.
- Resize/maximize/minimize are handled by the underlying Donut `DeviceManager` during `step()`.

## Existing Examples

| File | Purpose |
| --- | --- |
| `Rtxpt/Python/Examples/offline_render.py` | Headless reference render and screenshot. |
| `Rtxpt/Python/Examples/test_splat_interactive.py` | Windowed or headless 3DGS rasterization test. |
| `Rtxpt/Python/Examples/3dgs_example.py` | Batch 3DGS Reference/OIDN and Realtime/DLSS-RR render test. |
| `Rtxpt/Python/Examples/example_basic.py` | Basic embedded scripting. |
| `Rtxpt/Python/Examples/example_modes_dlss_oidn.py` | Realtime/reference mode, DLSS, OIDN settings. |
| `Rtxpt/Python/Examples/example_animate_lights.py` | Per-frame light edits. |

## Introspection

The binding also exposes docstrings through nanobind:

```python
import rtxpt
help(rtxpt)
help(rtxpt.Renderer)
help(rtxpt.Sample)
help(rtxpt.Settings)
```
