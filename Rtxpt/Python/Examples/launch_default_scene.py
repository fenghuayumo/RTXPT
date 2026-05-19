#!/usr/bin/env python
"""Launch RTXPT with a native builtin default scene.

The rtxpt.Renderer(scene=...) binding accepts file names, builtin primitive
references, and inline scene JSON strings. This example uses inline JSON plus a
builtin primitive model, so it does not depend on any mesh file in Assets.

Usage:
    cd <repo>/bin
    python ../Rtxpt/Python/Examples/launch_default_scene.py

    # Non-interactive smoke test:
    python ../Rtxpt/Python/Examples/launch_default_scene.py --headless --out default_scene.png
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


def configure_import_path() -> None:
    candidates = [
        REPO_ROOT / "bin",
        REPO_ROOT / "build" / "Rtxpt" / "Release",
        Path(__file__).resolve().parent,
    ]
    for candidate in candidates:
        if glob.glob(str(candidate / "rtxpt*.pyd")) or glob.glob(str(candidate / "rtxpt*.so")):
            sys.path.insert(0, str(candidate))
            os.environ["PATH"] = str(candidate) + os.pathsep + os.environ.get("PATH", "")
            os.chdir(candidate)
            return

    searched = "\n".join(f"  {p}" for p in candidates)
    raise RuntimeError(f"Could not find rtxpt Python module. Searched:\n{searched}")


def build_default_scene_description(rtxpt) -> str:
    """Return an inline scene description string for a plane + cube."""
    if hasattr(rtxpt, "builtin_scene_json"):
        return rtxpt.builtin_scene_json("plane_cube")

    # Fallback for source readability; current C++ builds provide the helper.
    scene = {
        "models": ["builtin:plane_cube"],
        "graph": [
            {
                "name": "DefaultPlaneCube",
                "model": 0,
            },
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "Sun",
                        "type": "DirectionalLight",
                        "rotation": [
                            -0.2305389071743629,
                            -0.15879165885860183,
                            -0.6890465942713406,
                            0.6684697541989844,
                        ],
                        "angularSize": 1.5,
                        "color": [1.0, 0.96, 0.9],
                        "irradiance": 3.0,
                    },
                    {
                        "name": "Fill",
                        "type": "PointLight",
                        "translation": [0.0, 2.5, 3.0],
                        "color": [1.0, 0.95, 0.85],
                        "intensity": 30.0,
                        "radius": 0.05,
                        "range": 10.0,
                    }
                ],
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": [0.0, 1.15, 5.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                        "verticalFov": 0.7,
                        "zNear": 0.001,
                        "exposureCompensation": 1.0,
                        "enableAutoExposure": False,
                    }
                ],
            },
            {
                "name": "SampleSettings",
                "type": "SampleSettings",
                "realtimeMode": True,
                "startingCamera": -1,
            },
        ],
    }
    return json.dumps(scene, indent=2)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Launch a native builtin plane + cube scene.")
    parser.add_argument("--headless", action="store_true", help="Render offscreen and exit.")
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--spp", type=int, default=16, help="Reference samples for --headless.")
    parser.add_argument("--out", default="default_scene.png", help="Screenshot path for --headless.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    launch_cwd = Path.cwd()
    configure_import_path()
    import rtxpt

    scene = build_default_scene_description(rtxpt)

    mode = "headless" if args.headless else "windowed"
    print(f"[rtxpt] Launching default plane + cube scene ({mode}) ...")
    renderer = rtxpt.Renderer(
        width=args.width,
        height=args.height,
        headless=args.headless,
        scene=scene,
        realtime=not args.headless,
        accumulation_target=args.spp,
    )

    try:
        s = renderer.settings
        s.realtime_mode = not args.headless
        s.bounce_count = 8
        s.enable_tone_mapping = True
        s.realtime_aa = rtxpt.RealtimeAA.Off

        if args.headless:
            print(f"[rtxpt] Rendering {args.spp} spp ...")
            frames = renderer.step_until_accumulated()
            out_path = Path(args.out)
            if not out_path.is_absolute():
                out_path = launch_cwd / out_path
            out_path = out_path.resolve()
            if not renderer.save_screenshot(str(out_path)):
                raise RuntimeError(f"Failed to save screenshot: {out_path}")
            print(f"[rtxpt] Saved: {out_path} ({frames} frames)")
        else:
            print("[rtxpt] Ready. Default scene contains one plane with one cube on top.")
            print("[rtxpt]   Left-click  -> Inspector (Transform)")
            print("[rtxpt]   Right-click -> Material Editor")
            print("[rtxpt]   Close window or Ctrl+C to exit.")
            while renderer.step(-1.0):
                time.sleep(0.001)
    except KeyboardInterrupt:
        print("\n[rtxpt] Interrupted.")
    finally:
        renderer.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
