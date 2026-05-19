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

After building the `rtxpt_py` target, install the extension package from the
repository root:

```
python -m pip install .
python -c "import rtxpt; print(rtxpt.MODE)"
```

This creates a local binary wheel from the current `bin/` runtime payload and
installs it into the active Python environment. Then you can drive a brand-new
device + scene from a standalone Python interpreter:

```
python Rtxpt\Python\Examples\offline_render.py ^
       --scene bistro-programmer-art.scene.json ^
       --width 1280 --height 720 --spp 256 --out out.png
```

For quick local development without installing, adding `bin/` to `PYTHONPATH`
still works.

Or interactively:

```python
import rtxpt
r = rtxpt.Renderer(width=1280, height=720, headless=True,
                   scene="builtin:plane_cube")
r.settings.accumulation_target = 64
r.step_until_accumulated()
r.save_screenshot("frame.png")
r.close()
```

`headless=True` creates a DX12/Vulkan device with offscreen back buffers,
without creating an OS window or swap chain.

For package smoke tests, `scene="builtin:plane_cube"` does not require any
mesh file from `Assets`. You can also pass inline scene JSON directly; model
entries may use `builtin:plane`, `builtin:cube`, `builtin:sphere`, or
`builtin:plane_cube`.

## Bindings overview

| Object                          | Purpose                                       |
|---------------------------------|-----------------------------------------------|
| `rtxpt.MODE`                    | `"embed"` or `"extension"`                    |
| `rtxpt.Renderer(...)`           | (extension only) creates a new headless device|
| `rtxpt.builtin_scene_json(...)` | Inline JSON for a builtin primitive scene     |
| `rtxpt.app()`                   | Returns the current `Sample` renderer         |
| `rtxpt.settings()`              | Shortcut for `rtxpt.app().settings`           |
| `Sample.scene`                  | Current loaded `Scene`                        |
| `Sample.set_realtime_mode(...)` | Switch to realtime mode + AA + denoiser       |
| `Sample.set_reference_mode(...)`| Switch to reference accumulation + OIDN       |
| `Scene.get_materials()`         | List of `PTMaterial` in the current scene     |
| `Scene.find_material(name)`     | Lookup by `Name` or `UniqueName`              |
| `Scene.get_lights()`            | List of `Light` (Directional/Spot/Point/Env)  |
| `Sample.set_environment_map`    | Override the scene's HDRI                     |
| `Sample.set_camera_fov`         | Override vertical FOV (degrees)               |
| `Settings.path_tracer_mode`     | rtxpt.PathTracerMode (Realtime / Reference)   |
| `Settings.realtime_aa`          | rtxpt.RealtimeAA (Off / TAA / DLSS / DLSS-RR) |
| `Settings.dlss_mode` etc.       | DLSS / DLSS-RR / DLSS-G / Reflex parameters   |
| `Settings.oidn_*`               | OIDN denoiser parameters (reference mode)     |
| `Settings.gaussian_splat_*`     | 3DGS raster, storage, culling, shadow controls|
| `Settings.environment_map`      | Tint/intensity/rotation/visibility of env map |
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
