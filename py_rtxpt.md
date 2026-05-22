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

构建 `rtxpt_py` target 后，Python extension 输出在 `bin/`：

```text
bin/rtxpt.cp311-win_amd64.pyd
```

推荐安装方式是直接在仓库根目录运行：

```powershell
python -m pip install .
python -c "import rtxpt; print(rtxpt.MODE)"
```

这会从当前 `bin/` 里的 native extension、运行时 DLL/so、shader 和必要 Assets
组装本地 binary wheel，并安装到当前 Python 环境。也可以先显式构建 wheel，再安装：

```powershell
python Support/python/build_wheel.py
python -m pip install dist/rtxpt-*.whl
```

打包参数可以用环境变量控制：

| Variable | Default | Values |
| --- | --- | --- |
| `RTXPT_WHEEL_VERSION` | `0.2.0` | 任意 PEP 440 version |
| `RTXPT_WHEEL_ASSETS` | `minimal` | `minimal`, `full`, `none` |
| `RTXPT_WHEEL_DYNAMIC_SHADERS` | `bin` | `bin`, `full`, `none` |
| `RTXPT_WHEEL_SHADER_API` | Windows 为 `d3d12`，其他平台为 `vulkan` | `d3d12`, `vulkan`, `both` |

开发时如果不想安装，也仍然可以把 `bin/` 放进 `sys.path` 或 `PYTHONPATH`：

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

3DGS objects are scene graph objects. Prefer declaring them in the scene JSON:

```python
import rtxpt

scene = r'''
{
  "models": ["builtin:plane"],
  "graph": [
    { "name": "Ground", "model": 0 },
    {
      "name": "Scan",
      "type": "GaussianSplat",
      "path": "D:/ScanVideo/chuan/splats.ply",
      "convertRdfToDonut": true,
      "translation": [0, 0, 0],
      "scaling": [1, 1, 1]
    }
  ]
}
'''

r = rtxpt.Renderer(width=1280, height=720, headless=False, realtime=True, scene=scene)

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

For script-driven workflows, `load_gaussian_splats(path, convert_rdf_to_donut=True)` appends a `GaussianSplat` node to the current scene root. Calling `load_scene(...)` replaces the current scene graph and destroys previously appended splat nodes, so load them again after switching scenes or declare them in the target scene JSON.

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

### COLMAP Camera 3DGS Alignment Test

`render_gs_colmap_views.py` renders a 3DGS PLY from COLMAP `cameras.bin/images.bin` views. It is useful for comparing RTXPT output against gsplat output from the same camera poses.

By default, it reads:

```text
D:/ProgramCode/Python/demo_gsplat&blender/GS/gaussians.ply
D:/ProgramCode/Python/demo_gsplat&blender/GS/sparse
```

Example:

```powershell
python .\Rtxpt\Python\Examples\render_gs_colmap_views.py ^
    --max-views 8 ^
    --frames-per-view 8 ^
    --warmup-frames 4 ^
    --mip-antialiasing ^
    --out-dir "D:\ProgramCode\Python\demo_gsplat&blender\GS\rtxpt_rendered_intrinsics_mipaa"
```

The script passes full COLMAP pinhole intrinsics (`fx`, `fy`, `cx`, `cy`) through `Renderer.set_camera_intrinsics(...)`. This keeps off-center principal points aligned with gsplat. Use `--symmetric-fov` only when intentionally testing the older vertical-FOV-only path.

When `--convert-rdf-to-donut` is enabled, which is the default, both the PLY loader and the COLMAP camera pose are converted from RDF/COLMAP coordinates into RTXPT/Donut coordinates. `--mip-antialiasing` is enabled by default and can be disabled with `--no-mip-antialiasing`.

### Edit Materials

```python
import rtxpt

r = rtxpt.Renderer(scene="bistro-programmer-art.scene.json", headless=True)
scene = r.app.scene
mat = scene.find_material("SomeMaterialName")
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
scene = r.app.scene
for light in scene.get_lights():
    print(light.name, light.light_type)
    light.color = (1.0, 0.9, 0.75)

sun = scene.find_light("Sun")
if sun:
    sun.direction = (0.0, -1.0, 0.2)

r.step_n(8)
r.save_screenshot("lights.png")
r.close()
```

### Deform Mesh Vertices

Mesh deformation works on object-space vertex positions. After `set_mesh_vertices(...)`
or `deform_mesh(...)`, RTXPT refreshes the mesh GPU buffer and can rebuild ray tracing
acceleration structures so the edited geometry is used by subsequent frames.

```python
import math
import rtxpt

r = rtxpt.Renderer(scene="builtin:cube", headless=True, accumulation_target=8)
app = r.app

mesh = app.find_mesh("cube") or app.get_meshes()[0]
vertices = list(app.get_mesh_vertices(mesh))

# Simple soft bulge: move upper vertices upward based on x/z radius.
deformed = []
for x, y, z in vertices:
    radius = math.sqrt(x * x + z * z)
    lift = 0.15 * max(0.0, 1.0 - radius)
    deformed.append((x, y + lift, z))

app.set_mesh_vertices(mesh, deformed, recompute_normals=True)
app.step_until_accumulated()
r.save_screenshot("deformed_mesh.png")
r.close()
```

For callback-style edits, return `None` to keep a vertex unchanged:

```python
def wave(index, p):
    x, y, z = p
    if y < 0:
        return None
    return (x, y + 0.05 * math.sin(index * 0.37), z)

app.deform_mesh(mesh, wave, recompute_normals=True)
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
)
```

| Argument | Meaning |
| --- | --- |
| `width`, `height` | Initial backbuffer/window size. |
| `headless` | `True`: offscreen backbuffers, no OS window. `False`: create a window and swap chain. |
| `vulkan` | `False` uses DX12. `True` requests Vulkan when available. |
| `adapter_index` | GPU index, `-1` means default adapter. |
| `debug` | Enable graphics debug settings. |
| `scene` | Scene file path/name, `builtin:*` primitive reference, or inline scene JSON string. Relative file paths are resolved from `Assets/`. |
| `realtime` | Start in realtime mode if `True`, reference mode if `False`. |
| `accumulation_target` | Reference SPP target. |

### Methods / Properties

| API | Return | Notes |
| --- | --- | --- |
| `close()` | `None` | Tears down renderer/device. Also called by destructor/context manager. |
| `load_scene(scene_name, wait_until_ready=True)` | `bool` | Switch scene. |
| `load_gaussian_splats(file_name, convert_rdf_to_donut=True)` | `bool` | Append a `.ply` 3DGS scene object under the current scene root. |
| `load_mesh_file(file_name)` | `bool` | Append a `.gltf`, `.glb`, or `.obj` mesh under the current scene root. |
| `get_scene_bounds()` | `tuple | None` | Active scene world-space `((min.xyz), (max.xyz))` AABB from C++ `Scene::GetSceneBounds()`. |
| `scene_bounds` | `tuple | None` | Property alias for `get_scene_bounds()`. |
| `scene_bounds_center` | `tuple | None` | Center of `scene_bounds`. |
| `scene_bounds_size` | `tuple | None` | Extent `(max - min)` of `scene_bounds`. |
| `step(dt=-1.0)` | `bool` | Render one frame. Returns `False` on failure or when window close is requested. |
| `step_n(frames)` | `bool` | Render exactly N frames unless `step()` fails. |
| `step_until_accumulated(max_frames=0)` | `int` | Reset accumulation and step until accumulation completes, or until `max_frames` if positive. |
| `save_screenshot(output_path)` | `bool` | Save current backbuffer to PNG/JPG/BMP/TGA. |
| `set_camera(position, direction, up=(0, 1, 0))` | `bool` | Triples can be lists/tuples of 3 floats. |
| `set_camera_fov(vertical_fov_degrees)` | `None` | Set vertical FOV in degrees. |
| `set_camera_intrinsics(fx, fy, cx, cy, width, height)` | `None` | Set an off-center pinhole projection from pixel-space intrinsics. This overrides the symmetric FOV projection until `set_camera_fov(...)` is called. |
| `app` | `Sample` | Underlying renderer instance. |
| `settings` | `Settings` | Live UI/settings state. |

`Renderer` supports context manager syntax:

```python
with rtxpt.Renderer(headless=True) as r:
    r.step_n(8)
```

### Inline / Builtin Scenes

For package smoke tests that should not depend on external mesh assets, the extension accepts builtin primitive scenes:

```python
with rtxpt.Renderer(headless=True, scene="builtin:plane_cube", accumulation_target=4) as r:
    r.step_until_accumulated()
    r.save_screenshot("smoke.png")
```

Supported builtin models are `builtin:plane`, `builtin:cube`, `builtin:sphere`, and `builtin:plane_cube`.

You can also pass an inline scene JSON string. Model entries may reference builtin primitives:

```python
scene = rtxpt.builtin_scene_json("plane_cube")
r = rtxpt.Renderer(headless=True, scene=scene)
```

Scene JSON may also declare one or more 3DGS nodes directly:

```python
scene = r"""
{
  "graph": [
    {
      "name": "ScanA",
      "type": "GaussianSplat",
      "path": "D:/ScanVideo/chuan/splats_a.ply",
      "translation": [0.0, 0.0, 0.0],
      "scaling": 1.0,
      "convertRdfToDonut": true,
      "enabled": true
    },
    {
      "name": "ScanB",
      "type": "GaussianSplat",
      "path": "D:/ScanVideo/chuan/splats_b.ply",
      "translation": [2.0, 0.0, 0.0],
      "scaling": 0.75
    }
  ]
}
"""
r = rtxpt.Renderer(headless=False, realtime=True, scene=scene)
```

For scene files, relative 3DGS paths are resolved relative to the scene JSON file. `path`, `file`, and `fileName` are accepted aliases.

## `Sample` Class

Top-level renderer instance. In extension mode, access it through `renderer.app`; in embed mode, use `rtxpt.app()`.

### Read-Only Properties

| Property | Type | Notes |
| --- | --- | --- |
| `settings` | `Settings` | Live settings object. |
| `scene` | `Scene | None` | Current loaded scene, matching the C++ `GetScene()` entry point. |
| `scene_name` | `str` | Current scene name. |
| `available_scenes` | `list[str]` | Scene files discovered by the app. |
| `gaussian_splat_object_count` | `int` | Number of loaded 3DGS scene objects. |
| `gaussian_splat_count` | `int` | Total loaded splat count across current 3DGS scene objects. |
| `gaussian_splat_file_name` | `str` | Single loaded 3DGS path, or a summary when multiple 3DGS objects are present. |
| `scene_bounds` | `tuple | None` | Shortcut for `scene.get_scene_bounds()`. |
| `scene_bounds_center` | `tuple | None` | Center of `scene_bounds`. |
| `scene_bounds_size` | `tuple | None` | Extent `(max - min)` of `scene_bounds`. |
| `accumulation_completed` | `bool` | Whether reference accumulation is complete. |
| `accumulation_sample_index` | `int` | Current accumulation sample index. |

### Scene / Assets

| API | Return | Notes |
| --- | --- | --- |
| `set_scene(scene_name, force_reload=False)` | `None` | Switch scene. |
| `load_gaussian_splats(file_name, convert_rdf_to_donut=True)` | `bool` | Append a 3DGS `.ply` node to the current scene. |
| `load_mesh_file(file_name)` | `bool` | Append a `.gltf`, `.glb`, or `.obj` mesh node to the current scene. |
| `set_environment_map(path)` | `None` | Override scene environment map source. |
| `get_scene()` | `Scene | None` | Return the current loaded scene. |
| `get_scene_bounds()` | `tuple | None` | Shortcut for `scene.get_scene_bounds()`. |

### Scene Meshes / Deformation

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_meshes()` | `list[Mesh]` | All meshes in the current scene. |
| `scene.find_mesh(name)` | `Mesh | None` | Match by mesh name. |
| `scene.mesh_count` | `int` | Number of meshes in the current scene. |
| `sample.get_meshes()` | `list[Mesh]` | Compatibility alias for `scene.get_meshes()`. |
| `sample.find_mesh(name)` | `Mesh | None` | Compatibility alias for `scene.find_mesh(name)`. |
| `sample.get_mesh_vertices(mesh)` | `list[tuple]` | Returns object-space `(x, y, z)` vertex positions. |
| `sample.set_mesh_vertices(mesh, vertices, recompute_normals=True, rebuild_acceleration_structure=True)` | `None` | Replaces all positions. `vertices` must contain exactly `mesh.vertex_count` triples. |
| `sample.deform_mesh(mesh, callback, recompute_normals=True, rebuild_acceleration_structure=True)` | `int` | Calls `callback(index, (x, y, z))` for each vertex. Return a new triple or `None`; returns edited vertex count. |

`set_mesh_vertices(...)` updates object-space mesh bounds, optionally recomputes normals,
refreshes GPU vertex data, resets accumulation, and requests acceleration structure rebuild
by default. Keep `rebuild_acceleration_structure=True` for ray tracing-correct geometry.
Only set it to `False` when batching several edits and calling `request_accel_rebuild()`
after the final update.

### Scene Bounds

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_scene_bounds()` | `tuple | None` | World-space `((min.xyz), (max.xyz))` AABB from C++ `Scene::GetSceneBounds()`. |
| `scene.get_bounds()` | `tuple | None` | Alias for `scene.get_scene_bounds()`. |
| `scene.bounds` | `tuple | None` | Property alias for `scene.get_scene_bounds()`. |
| `scene.bounds_center` | `tuple | None` | Center of `scene.bounds`. |
| `scene.bounds_size` | `tuple | None` | Extent `(max - min)` of `scene.bounds`. |

### Scene Nodes

| API | Return | Notes |
| --- | --- | --- |
| `scene.find_node(path)` | `SceneNode | None` | Find a scene graph node by name or path. |
| `sample.find_node(path)` | `SceneNode | None` | Compatibility alias for `scene.find_node(path)`. |

### Scene Materials

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_materials()` | `list[Material]` | All `PTMaterial` materials in the current scene. |
| `scene.find_material(name)` | `Material | None` | Match by `Name` or `UniqueName`. |
| `scene.find_material_by_id(material_id)` | `Material | None` | Lookup by material ID. |
| `scene.material_count` | `int` | Number of PT materials in the current scene. |

`Sample.get_materials()`, `Sample.find_material()`, and `Sample.find_material_by_id()` remain available as compatibility aliases.

### Scene Lights

| API | Return | Notes |
| --- | --- | --- |
| `scene.get_lights()` | `list[Light]` | All lights in current scene. |
| `scene.find_light(name)` | `Light | None` | Match by scene node name. |
| `scene.light_count` | `int` | Number of lights in the current scene. |

`Sample.get_lights()` and `Sample.find_light()` remain available as compatibility aliases.

### Camera

| API | Return | Notes |
| --- | --- | --- |
| `get_camera_pos_dir_up()` | `str` | Comma-separated `pos.xyz,dir.xyz,up.xyz`. |
| `set_camera_pos_dir_up(pos_dir_up)` | `bool` | Input format matches `get_camera_pos_dir_up()`. |
| `set_camera_fov(vertical_fov_degrees)` | `None` | Takes degrees. |
| `set_camera_intrinsics(fx, fy, cx, cy, width, height)` | `None` | Uses pixel-space pinhole intrinsics for the active projection. Useful for COLMAP/OpenCV cameras with non-centered `cx/cy`. |
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

3DGS data is scene-owned. Scene JSON can contain any number of `GaussianSplat`, `GaussianSplats`, or `3DGaussianSplat` nodes. `load_gaussian_splats(...)` is a convenience method that appends another `GaussianSplat` node to the current scene root. Switching scenes clears the old scene graph, including its 3DGS objects.

Rasterization runs over all enabled 3DGS scene objects. Emissive proxy sampling also combines all enabled 3DGS objects into one world-space proxy list. The current RTX/path-tracing splat shadow binding still has one global resource slot, so splat shadows use the first enabled 3DGS object as the primary shadow source.

The table below lists the Python-facing 3DGS settings that are currently wired into the renderer. Rasterization, culling, emission, and shadow settings are shared render settings. Object placement belongs to the scene graph node transform. `gaussian_splat_translation`, `gaussian_splat_rotation_euler_deg`, and `gaussian_splat_object_scale` are only used as the initial transform when Python appends a new 3DGS node through `load_gaussian_splats(...)`.

| Property | Type | Notes |
| --- | --- | --- |
| `enable_gaussian_splats` | `bool` | Enables rendering for 3DGS scene objects. |
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
| `gaussian_splat_tint_color` | `(r, g, b)` | Multiplies the SH0/base color before brightness. |
| `gaussian_splat_as_emitter` | `bool` | Inject 3DGS emissive proxies into light sampling. |
| `gaussian_splat_emission_intensity` | `float` | Emissive proxy intensity multiplier. |
| `gaussian_splat_emission_max_proxy_count` | `int` | Emissive proxy budget. |
| `gaussian_splat_alpha_cull_threshold` | `float` | Cull low-alpha splats. |
| `gaussian_splat_translation` | `(x, y, z)` | Initial translation for newly attached Python 3DGS nodes. |
| `gaussian_splat_rotation_euler_deg` | `(x, y, z)` | Initial Euler rotation in degrees for newly attached Python 3DGS nodes. |
| `gaussian_splat_object_scale` | `(x, y, z)` | Initial non-uniform scale for newly attached Python 3DGS nodes. |
| `gaussian_splat_shadows` | `bool` | Enable splat shadow integration. |
| `gaussian_splat_hybrid_shadows` | `bool` | Alias for `gaussian_splat_shadows`. |
| `gaussian_splat_shadows_mode` | `int/GaussianSplatShadowMode` | Disabled, hard, or soft splat shadows. |
| `gaussian_splat_shadow_strength` | `float` | Shadow opacity/strength. |
| `gaussian_splat_shadow_soft_radius` | `float` | Soft shadow radius. |
| `gaussian_splat_shadow_soft_sample_count` | `int` | Soft shadow sample count. |
| `gaussian_splat_rtx_kernel_degree` | `int` | RTX splat kernel degree. |
| `gaussian_splat_rtx_adaptive_clamp` | `bool` | Enable adaptive RTX alpha clamp. |
| `gaussian_splat_rtx_particle_shadow_offset` | `float` | RTX particle shadow offset. |
| `gaussian_splat_object_count` | `int` | Read-only 3DGS scene object count. |
| `gaussian_splat_count` | `int` | Read-only total splat count across current 3DGS scene objects. |
| `gaussian_splat_file_name` | `str` | Read-only single path or multi-object summary. |

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
| `visible_to_camera` | `bool` |
| `hide_source` | `bool` inverse of `visible_to_camera` |

## `Material` Class

Returned by `Scene.get_materials()`, `Scene.find_material()`, and `Scene.find_material_by_id()`.

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

## `SceneNode` Class

Returned by `Scene.find_node()` and `Sample.find_node()`.

| Property | Type |
| --- | --- |
| `name` | `str` |
| `path` | `str` |
| `mesh` | `Mesh | None` |
| `is_mesh` | `bool` |
| `translation` | `(x, y, z)` |
| `rotation` | `(x, y, z, w)` quaternion |
| `euler` | `(x, y, z)` radians |
| `scaling` | `(x, y, z)` |
| `bounds` | `((min.xyz), (max.xyz)) \| None` |

`rotation` and `euler` both write the node's local Transform rotation. Assigning
`euler` converts XYZ radians to the stored quaternion; assigning `rotation` expects
an XYZW quaternion, matching scene JSON.

## `Mesh` Class

Returned by `Scene.get_meshes()`, `Scene.find_mesh()`, `Sample.get_meshes()`,
`Sample.find_mesh()`, and `SceneNode.mesh`.

Read-only properties:

| Property | Type | Notes |
| --- | --- | --- |
| `name` | `str` | Mesh name from the source model or builtin primitive. |
| `global_mesh_index` | `int` | Internal scene mesh index. |
| `vertex_count` | `int` | Number of object-space positions expected by `set_mesh_vertices(...)`. |
| `index_count` | `int` | Total index count. |
| `geometry_count` | `int` | Number of mesh geometry groups/submeshes. |
| `bounds` | `((min.xyz), (max.xyz)) \| None` | Object-space mesh AABB. |

Vertex data is intentionally edited through `Sample`, not through writable `Mesh`
properties, because changing vertices also needs renderer-side GPU buffer refresh and
acceleration-structure invalidation.

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
| `Rtxpt/Python/Examples/render_gs_colmap_views.py` | Render 3DGS from COLMAP camera poses with full pinhole intrinsics and optional Mip antialiasing. |
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
