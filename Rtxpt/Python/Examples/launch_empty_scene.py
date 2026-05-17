#!/usr/bin/env python
"""Launch RTXPT with an empty scene for drag-and-drop testing.

Usage:
    cd <repo>/bin
    python ../Rtxpt/Python/Examples/launch_empty_scene.py

Drag .ply / .gltf / .glb / .obj files into the window to load them.
Left-click to pick an instance (Inspector panel).
Right-click to pick a material (Material Editor panel).
"""

from __future__ import annotations

import glob
import json
import os
import sys
import tempfile
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


def create_empty_scene() -> Path:
    model_path = REPO_ROOT / "Assets" / "Models" / "ConvergenceTest" / "ConvergenceTest.gltf"
    if not model_path.exists():
        raise FileNotFoundError(
            f"Dummy model not found: {model_path}\n"
            "The engine requires at least one mesh to initialize the acceleration structure."
        )

    scene_path = Path(tempfile.gettempdir()) / "rtxpt_empty.scene.json"
    scene = {
        "models": [str(model_path).replace("\\", "/")],
        "graph": [
            {
                "name": "HiddenDummyMesh",
                "model": 0,
                "translation": [100000.0, 100000.0, 100000.0],
                "scaling": 0.001,
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": [0.0, 1.0, -5.0],
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                        "verticalFov": 0.785398,
                        "zNear": 0.001,
                        "exposureCompensation": 0.0,
                        "enableAutoExposure": False,
                    }
                ],
            },
        ],
    }
    scene_path.write_text(json.dumps(scene, indent=2), encoding="utf-8")
    print(f"[rtxpt] Generated empty scene: {scene_path}")
    return scene_path


def main() -> int:
    configure_import_path()
    import rtxpt

    scene = str(create_empty_scene())

    print(f"[rtxpt] Launching empty scene (windowed) ...")
    renderer = rtxpt.Renderer(
        width=1280,
        height=720,
        headless=False,
        scene=scene,
        realtime=True,
        accumulation_target=1,
    )

    try:
        s = renderer.settings
        s.realtime_mode = True
        s.bounce_count = 8
        s.enable_tone_mapping = True
        s.realtime_aa = rtxpt.RealtimeAA.Off

        print("[rtxpt] Ready. Drag files into the window to load models.")
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
