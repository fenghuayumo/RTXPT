# RTXPT Python Scripting Examples

RTXPT exposes a `rtxpt` Python module in **two complementary modes**:

| Mode | Binary | When | Use cases |
|---|---|---|---|
| **Embed** | `Rtxpt.exe` | Python is hosted inside the running renderer | Live tweaking, debug overlays, capture scripts, gameplay scripting |
| **Extension** | `rtxpt.pyd` | Python launches the renderer (`python script.py`) | Offline rendering, batch / data generation, headless CI |

Both modes share the same `rtxpt.Material`, `rtxpt.Light*`, `rtxpt.Settings`,
`rtxpt.Sample` types so a script can be moved between them with minimal
changes.  Inspect `rtxpt.MODE` (`"embed"` vs `"extension"`) when you need
to branch.

---

## Embed mode

Already shipping inside `Rtxpt.exe`.

* Make sure RTXPT was built with `RTXPT_WITH_PYTHON=ON` (default).
* Run the renderer with a startup script:

```
Rtxpt.exe --pythonScript Rtxpt/Python/Examples/example_basic.py
```

* Or run a one-off expression:

```
Rtxpt.exe --pythonExpr "import rtxpt; print(rtxpt.app().scene_name)"
```

* From within the running app, open `System -> Python scripting`,
  paste an expression in the multi-line editor and hit `Run inline`.

In embed mode the "current renderer" is the singleton inside `Rtxpt.exe`,
so you reach it via `rtxpt.app()`.

## Extension mode (offline / headless rendering)

After a build, `rtxpt.pyd` is produced next to `Rtxpt.exe` (e.g.
`bin/Release/rtxpt.pyd`).  Add that folder to `PYTHONPATH` and you can
drive a brand-new device + scene from a standalone Python interpreter:

```
set PYTHONPATH=<repo>\bin\Release;%PYTHONPATH%
python Rtxpt\Python\Examples\offline_render.py ^
       --scene bistro-programmer-art.scene.json ^
       --width 1280 --height 720 --spp 256 --out out.png
```

Or interactively:

```python
import rtxpt
r = rtxpt.Renderer(width=1280, height=720, headless=True,
                   scene="bistro-programmer-art.scene.json")
r.settings.accumulation_target = 64
r.step_until_accumulated()
r.save_screenshot("frame.png")
r.close()
```

`headless=True` creates a DX12/Vulkan device with offscreen back buffers,
without creating an OS window or swap chain.

## Bindings overview

| Object                          | Purpose                                       |
|---------------------------------|-----------------------------------------------|
| `rtxpt.MODE`                    | `"embed"` or `"extension"`                    |
| `rtxpt.Renderer(...)`           | (extension only) creates a new headless device|
| `rtxpt.app()`                   | Returns the current `Sample` renderer         |
| `rtxpt.settings()`              | Shortcut for `rtxpt.app().settings`           |
| `Sample.set_realtime_mode(...)` | Switch to realtime mode + AA + denoiser       |
| `Sample.set_reference_mode(...)`| Switch to reference accumulation + OIDN       |
| `Sample.get_materials()`        | List of `PTMaterial` in the current scene     |
| `Sample.find_material(name)`    | Lookup by `Name` or `UniqueName`              |
| `Sample.get_lights()`           | List of `Light` (Directional/Spot/Point/Env)  |
| `Sample.set_environment_map`    | Override the scene's HDRI                     |
| `Sample.set_camera_fov`         | Override vertical FOV (degrees)               |
| `Settings.path_tracer_mode`     | rtxpt.PathTracerMode (Realtime / Reference)   |
| `Settings.realtime_aa`          | rtxpt.RealtimeAA (Off / TAA / DLSS / DLSS-RR) |
| `Settings.dlss_mode` etc.       | DLSS / DLSS-RR / DLSS-G / Reflex parameters   |
| `Settings.oidn_*`               | OIDN denoiser parameters (reference mode)     |
| `Settings.gaussian_splat_*`     | 3DGS raster, storage, culling, shadow controls|
| `Settings.environment_map`      | Tint/intensity/rotation of the env map        |
| `Settings.bounce_count` etc.    | Path tracer / NEE / RTXDI knobs               |
| `Renderer.step()/step_n(n)`     | (extension) drive the loop one frame at a time|
| `Renderer.step_until_accumulated()` | (extension) render to SPP target          |
| `Renderer.save_screenshot(path)`| (extension) write back buffer to PNG/JPG/BMP  |

### Enums

* `rtxpt.PathTracerMode` - `Realtime`, `Reference`
* `rtxpt.RealtimeAA` - `Off`, `TAA`, `DLSS`, `DLSS_RR`
* `rtxpt.DLSSMode` - `Off`, `MaxPerformance`, `Balanced`, `MaxQuality`, `UltraPerformance`, `UltraQuality`, `DLAA`
* `rtxpt.DLSSFGMode` - `Off`, `On`, `Auto`
* `rtxpt.DLSSRRPreset` - `Default`, `PresetA`..`PresetH`
* `rtxpt.ReflexMode` - `Off`, `LowLatency`, `LowLatencyWithBoost`
* `rtxpt.OidnPasses` - `ColorOnly`, `Albedo`, `AlbedoNormal`
* `rtxpt.OidnPrefilter` - `None_`, `Fast`, `Accurate`
* `rtxpt.OidnQuality` - `Fast`, `Balanced`, `High`
* `rtxpt.GaussianSplatSortMode` - `GpuSort`, `StochasticSplats`
* `rtxpt.GaussianSplatStorageFormat` - `Float32`, `Float16`, `Uint8`
* `rtxpt.GaussianSplatFrustumCulling` - `Disabled`, `AtDistanceStage`, `AtRasterStage`
* `rtxpt.GaussianSplatShadowMode` - `Disabled`, `Hard`, `Soft`
* `rtxpt.GaussianSplatFTBSyncMode` - `Disabled`, `Interlock`

All enums support `int(rtxpt.<EnumName>.<value>)` so they can be assigned
directly to settings fields that store ints.

Inspect the full surface with:

```python
import rtxpt
help(rtxpt)
```
