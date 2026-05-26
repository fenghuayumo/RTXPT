#!/usr/bin/env python
"""Render ScanVideo example OBJ meshes with RTX path tracing.

Built-in presets:
  - plate : m-plate-pbr_final/textured.obj
  - link  : link/link7_0.obj

Usage:
    cd <repo>
    python Rtxpt/Python/Examples/render_m_plate.py --model plate
    python Rtxpt/Python/Examples/render_m_plate.py --model link --out link7_0.png

    # Render all built-in presets:
    python Rtxpt/Python/Examples/render_m_plate.py --all

    # Custom OBJ path:
    python Rtxpt/Python/Examples/render_m_plate.py --obj-path D:/path/to/model.obj
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
SCANVIDEO_MESH_ROOT = Path(r"D:\ScanVideo\models\example_mesh")
MODEL_PRESETS: dict[str, Path] = {
    "plate": SCANVIDEO_MESH_ROOT / "background" / "output.obj",
    "link": SCANVIDEO_MESH_ROOT / "link" / "link7_0.obj",
}
DEFAULT_MODEL = "plate"
DEFAULT_HDR = REPO_ROOT / "Assets" / "EnvironmentMaps" / "20060807_wells6_hd.hdr"


def configure_import_path() -> None:
    try:
        import rtxpt  # noqa: F401

        return
    except ImportError:
        pass

    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build-linux" / "bin",
        REPO_ROOT / "build" / "Rtxpt" / "Release",
        Path(__file__).resolve().parent,
    ]
    for candidate in candidates:
        if glob.glob(str(candidate / "rtxpt*.pyd")) or glob.glob(str(candidate / "rtxpt*.so")):
            sys.path.insert(0, str(candidate))
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            os.chdir(candidate)
            return

    try:
        import rtxpt  # noqa: F401

        return
    except ImportError:
        pass

    searched = "\n".join(f"  {p}" for p in candidates)
    raise RuntimeError(f"Could not find rtxpt Python module. Searched:\n{searched}")


def build_scene_description(rtxpt, hdr_path: Path | None, ground_scale: float) -> str:
    """Minimal plane + lights + camera scene for a single imported mesh."""
    sky_light = None
    if hdr_path is not None and hdr_path.is_file():
        sky_light = {
            "name": "Sky",
            "type": "EnvironmentLight",
            "radianceScale": [1.0, 1.0, 1.0],
            "textureIndex": [0],
            "rotation": [0.0],
            "path": str(hdr_path.resolve()).replace("\\", "/"),
        }

    lights = [
        {
            "name": "Sun",
            "type": "DirectionalLight",
            "rotation": [-0.23053891, -0.15879166, -0.68904659, 0.66846975],
            "angularSize": 1.5,
            "color": [1.0, 0.96, 0.9],
            "irradiance": 0.5,
        },
        {
            "name": "Fill",
            "type": "PointLight",
            "translation": [1.5, 2.0, 2.5],
            "color": [1.0, 0.95, 0.85],
            "intensity": 10.0,
            "radius": 0.05,
            "range": 12.0,
        },
    ]
    if sky_light is not None:
        lights.append(sky_light)

    if hasattr(rtxpt, "builtin_scene_json"):
        # Start from plane-only builtin and replace graph below.
        base = json.loads(rtxpt.builtin_scene_json("plane"))
    else:
        base = {"models": ["builtin:plane"]}

    base["graph"] = [
        {
            "name": "GroundPlane",
            "model": 0,
            "translation": [0.0, 0.0, 0.0],
            "scaling": [ground_scale, 1.0, ground_scale],
        },
        {"name": "Lights", "children": lights},
        {
            "name": "Cameras",
            "children": [
                {
                    "name": "Default",
                    "type": "PerspectiveCameraEx",
                    "translation": [0.0, 0.5, 2.5],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                    "verticalFov": 0.65,
                    "zNear": 0.001,
                    "enableAutoExposure": False,
                    "exposureCompensation": 1.0,
                }
            ],
        },
        {
            "name": "SampleSettings",
            "type": "SampleSettings",
            "realtimeMode": True,
            "startingCamera": -1,
            "maxBounces": 8,
        },
    ]
    return json.dumps(base, indent=2)


def resolve_path(path: str) -> Path:
    resolved = Path(path).expanduser()
    if not resolved.is_absolute():
        resolved = Path.cwd() / resolved
    return resolved.resolve()


def read_obj_aabb(path: Path) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    """Return ((min_x, min_y, min_z), (max_x, max_y, max_z)) from OBJ vertex positions."""
    mins = [float("inf"), float("inf"), float("inf")]
    maxs = [float("-inf"), float("-inf"), float("-inf")]
    vertex_count = 0

    with path.open("r", encoding="utf-8", errors="ignore") as file:
        for line in file:
            if not line.startswith("v "):
                continue
            parts = line.split()
            if len(parts) < 4:
                continue
            try:
                xyz = [float(parts[1]), float(parts[2]), float(parts[3])]
            except ValueError:
                continue
            vertex_count += 1
            for axis, value in enumerate(xyz):
                mins[axis] = min(mins[axis], value)
                maxs[axis] = max(maxs[axis], value)

    if vertex_count == 0:
        raise RuntimeError(f"OBJ contains no readable vertex positions: {path}")

    return tuple(mins), tuple(maxs)


def aabb_center_extent(
    mins: tuple[float, float, float],
    maxs: tuple[float, float, float],
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    center = tuple((lo + hi) * 0.5 for lo, hi in zip(mins, maxs))
    extent = tuple(hi - lo for lo, hi in zip(mins, maxs))
    return center, extent


def frame_mesh(renderer, center: tuple[float, float, float], extent: tuple[float, float, float]) -> None:
    """Frame the camera around a mesh using its local AABB (not the full scene)."""
    horizontal = max(extent[0], extent[2]) * 0.5
    height = extent[1]
    focus = (center[0], center[1], center[2])
    # Pull back along +Z and slightly above for a 3/4 view of a flat plate.
    distance = max(horizontal, height) * 4.5
    camera_pos = (
        focus[0],
        focus[1] + horizontal * 0.8 + height * 2.0,
        focus[2] + distance,
    )
    camera_dir = (
        focus[0] - camera_pos[0],
        focus[1] - camera_pos[1],
        focus[2] - camera_pos[2],
    )
    renderer.set_camera(camera_pos, camera_dir, (0.0, 1.0, 0.0))
    renderer.set_camera_fov(35.0)


def place_mesh_on_ground(
    renderer,
    obj_path: Path,
) -> tuple[tuple[float, float, float], tuple[float, float, float]]:
    """Center mesh on XZ and place its bottom on y=0. Returns world center + extent."""
    scene = renderer.app.scene
    if scene is None:
        raise RuntimeError("No active scene after mesh import.")

    model_name = obj_path.stem
    node = scene.find_node(model_name)
    if node is None:
        raise RuntimeError(f"Scene node '{model_name}' not found after import.")

    mins, maxs = read_obj_aabb(obj_path)
    local_center, extent = aabb_center_extent(mins, maxs)
    node.translation = (
        -local_center[0],
        -mins[1],
        -local_center[2],
    )
    renderer.step_n(1)

    world_center = (0.0, extent[1] * 0.5, 0.0)
    print(
        f"[rtxpt] Placed '{model_name}' on ground: translation={node.translation}, "
        f"extent={tuple(round(v, 4) for v in extent)}"
    )
    return world_center, extent


def ground_scale_for_extent(extent: tuple[float, float, float]) -> float:
    horizontal = max(extent[0], extent[2])
    return max(horizontal * 1.5, 0.15)


def resolve_obj_paths(args: argparse.Namespace) -> list[Path]:
    if args.all:
        return [path.resolve() for path in MODEL_PRESETS.values()]

    if args.obj_path is not None:
        return [resolve_path(args.obj_path)]

    preset = args.model
    if preset not in MODEL_PRESETS:
        choices = ", ".join(sorted(MODEL_PRESETS))
        raise ValueError(f"Unknown model preset '{preset}'. Available: {choices}")
    return [MODEL_PRESETS[preset].resolve()]


def default_output_path(obj_path: Path) -> str:
    return f"{obj_path.stem}.png"


def print_materials(scene, model_name: str) -> None:
    count = 0
    for material in scene.get_materials():
        if material.model_name != model_name:
            continue
        count += 1
        print(
            f"[rtxpt]   {material.name}: base={material.base_color}, "
            f"rough={material.roughness:.2f}, metal={material.metalness:.2f}, "
            f"base_tex={material.enable_base_texture}"
        )
    if count == 0:
        print(f"[rtxpt] WARNING: no materials found for model '{model_name}'")


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Render ScanVideo example OBJ meshes.")
    model_group = parser.add_mutually_exclusive_group()
    model_group.add_argument(
        "--model",
        choices=sorted(MODEL_PRESETS),
        default=DEFAULT_MODEL,
        help=f"Built-in mesh preset (default: {DEFAULT_MODEL}).",
    )
    model_group.add_argument(
        "--obj-path",
        default=None,
        help="Custom OBJ path (overrides --model).",
    )
    model_group.add_argument(
        "--all",
        action="store_true",
        help="Render all built-in presets sequentially.",
    )
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--headless", action="store_true", default=True,
                            help="Render offscreen and save screenshot (default).")
    mode_group.add_argument("--window", "--no-headless", dest="headless", action="store_false",
                            help="Open interactive preview window.")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=32, help="Samples per pixel (headless).")
    parser.add_argument("--bounces", type=int, default=8)
    parser.add_argument("--out", default=None,
                        help="Output screenshot path (default: <mesh_stem>.png).")
    parser.add_argument("--vulkan", action="store_true", help="Use Vulkan backend.")
    parser.add_argument("--hdr", default=str(DEFAULT_HDR),
                        help="HDR environment map path (empty to disable).")
    parser.add_argument("--oidn", action="store_true",
                        help="Enable OIDN denoising after accumulation.")
    parser.add_argument("--oidn-gpu", dest="oidn_gpu",
                        action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--oidn-quality", type=int, default=2,
                        help="OIDN quality: 0=Fast, 1=Balanced, 2=High.")
    return parser


def render_mesh(
    obj_path: Path,
    out_path: Path,
    args: argparse.Namespace,
    rtxpt,
    launch_cwd: Path,
) -> None:
    if not obj_path.is_file():
        raise FileNotFoundError(f"OBJ mesh not found: {obj_path}")

    hdr_path = resolve_path(args.hdr) if args.hdr.strip() else None
    _, extent = aabb_center_extent(*read_obj_aabb(obj_path))
    scene = build_scene_description(rtxpt, hdr_path, ground_scale_for_extent(extent))

    mode = "headless" if args.headless else "windowed"
    print(f"[rtxpt] Mesh  : {obj_path}")
    print(f"[rtxpt] Mode  : {mode}")

    use_reference = args.headless or args.oidn
    renderer = rtxpt.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        vulkan=args.vulkan,
        scene=scene,
        realtime=not use_reference,
        accumulation_target=args.spp,
    )

    try:
        print(f"[rtxpt] Loading mesh: {obj_path}")
        if not renderer.load_mesh_file(str(obj_path)):
            raise RuntimeError(f"Failed to load mesh file: {obj_path}")

        world_center, extent = place_mesh_on_ground(renderer, obj_path)

        scene_obj = renderer.app.scene
        model_name = obj_path.stem
        print(f"[rtxpt] Materials for '{model_name}':")
        print_materials(scene_obj, model_name)

        print(
            f"[rtxpt] Mesh bounds (world): center={tuple(round(v, 4) for v in world_center)}, "
            f"extent={tuple(round(v, 4) for v in extent)}"
        )
        frame_mesh(renderer, world_center, extent)

        settings = renderer.settings
        settings.realtime_mode = not use_reference
        settings.accumulation_target = args.spp
        settings.accumulation_prewarm_realtime_caches = False
        settings.bounce_count = args.bounces
        settings.use_nee = True
        settings.enable_tone_mapping = True
        settings.realtime_aa = int(rtxpt.RealtimeAA.Off)

        if args.oidn:
            settings.oidn_enabled = True
            settings.oidn_use_gpu = args.oidn_gpu
            settings.oidn_quality = args.oidn_quality
            settings.oidn_apply()
            print("[rtxpt] OIDN enabled")

        if args.headless:
            label = f"{args.spp} spp"
            if args.oidn:
                label += " + OIDN"
            print(f"[rtxpt] Rendering {label} ...")
            t_start = time.time()
            frames = renderer.step_until_accumulated()
            elapsed = time.time() - t_start
            print(f"[rtxpt] Done in {elapsed:.2f}s ({frames} frames)")

            if not out_path.is_absolute():
                out_path = launch_cwd / out_path
            out_path = out_path.resolve()
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")
            print(f"[rtxpt] Saved: {out_path}")
        else:
            print("[rtxpt] Ready. Close window or Ctrl+C to exit.")
            while renderer.step(-1.0):
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\n[rtxpt] Interrupted.")
        raise
    finally:
        renderer.close()


def main() -> int:
    args = build_arg_parser().parse_args()
    if args.oidn and not args.headless:
        print("[rtxpt] --oidn uses reference accumulation; enabling --headless")
        args.headless = True
    if args.all and not args.headless:
        print("[rtxpt] --all uses headless rendering; enabling --headless")
        args.headless = True
    if args.all and args.out is not None:
        raise ValueError("--out cannot be used together with --all")

    obj_paths = resolve_obj_paths(args)
    launch_cwd = Path.cwd()
    configure_import_path()
    import rtxpt

    for index, obj_path in enumerate(obj_paths):
        if args.out is None:
            out_name = default_output_path(obj_path)
        else:
            out_name = args.out
        out_path = Path(out_name)

        if len(obj_paths) > 1:
            print(f"\n[rtxpt] === [{index + 1}/{len(obj_paths)}] {obj_path.name} ===")

        render_mesh(obj_path, out_path, args, rtxpt, launch_cwd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
