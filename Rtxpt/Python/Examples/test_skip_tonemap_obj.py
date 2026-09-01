"""Render an OBJ with the matching COLMAP camera for SkipToneMapping checks.

This is a standalone diagnostic helper. It never edits the OBJ/MTL/texture files.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np


def qvec2rotmat(qvec: np.ndarray) -> np.ndarray:
    q0, q1, q2, q3 = qvec
    return np.array(
        [
            [1 - 2 * q2 * q2 - 2 * q3 * q3, 2 * q1 * q2 - 2 * q0 * q3, 2 * q3 * q1 + 2 * q0 * q2],
            [2 * q1 * q2 + 2 * q0 * q3, 1 - 2 * q1 * q1 - 2 * q3 * q3, 2 * q2 * q3 - 2 * q0 * q1],
            [2 * q3 * q1 - 2 * q0 * q2, 2 * q2 * q3 + 2 * q0 * q1, 1 - 2 * q1 * q1 - 2 * q2 * q2],
        ],
        dtype=np.float64,
    )


def load_colmap_camera(cameras_txt: Path, images_txt: Path, image_name: str):
    cameras = {}
    for raw in cameras_txt.read_text(encoding="utf-8").splitlines():
        if not raw or raw.startswith("#"):
            continue
        p = raw.split()
        camera_id, model = int(p[0]), p[1]
        if model != "PINHOLE":
            raise ValueError(f"Expected PINHOLE camera, got {model}")
        cameras[camera_id] = (int(p[2]), int(p[3]), float(p[4]), float(p[5]), float(p[6]), float(p[7]))

    image_lines = images_txt.read_text(encoding="utf-8").splitlines()
    for raw in image_lines:
        if not raw or raw.startswith("#"):
            continue
        p = raw.split()
        if len(p) >= 10 and p[9] == image_name:
            image_id = int(p[0])
            qvec = np.asarray([float(x) for x in p[1:5]], dtype=np.float64)
            tvec = np.asarray([float(x) for x in p[5:8]], dtype=np.float64)
            camera_id = int(p[8])
            if camera_id not in cameras:
                raise KeyError(f"Camera {camera_id} not found")
            R = qvec2rotmat(qvec)
            w2c = np.eye(4, dtype=np.float64)
            w2c[:3, :3] = R
            w2c[:3, 3] = tvec
            return cameras[camera_id], np.linalg.inv(w2c), image_id
    raise FileNotFoundError(f"COLMAP image entry not found: {image_name}")


def make_scene(obj_path: Path) -> str:
    scene = {
        "models": [obj_path.as_posix()],
        "graph": [
            {"name": "Statue", "model": 0, "translation": [0, 0, 0], "rotation": [0, 0, 0, 1], "scaling": [1, 1, 1]},
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "Sun",
                        "type": "DirectionalLight",
                        "rotation": [0.0, 0.0, 0.0, 1.0],
                        "angularSize": 0.5,
                        "color": [1.0, 1.0, 1.0],
                        "irradiance": 1.0,
                    }
                ],
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": [0, 0, 0],
                        "rotation": [0, 0, 0, 1],
                        "zNear": 0.001,
                        "enableAutoExposure": False,
                        "exposureCompensation": 0.0,
                    }
                ],
            },
            # Do not add a SampleSettings/startup-camera node here. The Python
            # camera is installed after scene creation; a SampleSettings node
            # would re-select its default camera on the first realtime step and
            # overwrite the COLMAP pose used by this diagnostic.
        ],
    }
    return json.dumps(scene)


def apply_camera(renderer, c2w: np.ndarray, camera, rtxpt, convert_rdf_to_donut: bool, look_at: np.ndarray | None) -> None:
    _, _, fx, fy, cx, cy = camera
    pos = c2w[:3, 3].copy()
    # The RTXPT Python camera API follows the camera basis convention used by the
    # existing COLMAP render helpers in this repository.
    direction = c2w[:3, 2].copy()
    # COLMAP stores the camera Y axis as image-down. RTXPT's `up` argument is
    # image-up, so negate this basis vector to keep the scan upright.
    up = -c2w[:3, 1].copy()
    if look_at is not None:
        direction = look_at - pos
        direction /= max(float(np.linalg.norm(direction)), 1e-8)
        up = np.array([0.0, -1.0, 0.0], dtype=np.float64)
    if convert_rdf_to_donut:
        rdf_to_donut = np.array([1.0, -1.0, -1.0], dtype=np.float64)
        pos *= rdf_to_donut
        direction *= rdf_to_donut
        up *= rdf_to_donut
    renderer.set_camera(tuple(pos.tolist()), tuple(direction.tolist()), tuple(up.tolist()))
    if not hasattr(renderer, "set_camera_intrinsics"):
        raise RuntimeError("Current Python binding has no set_camera_intrinsics")
    renderer.set_camera_intrinsics(fx, fy, cx, cy, float(camera[0]), float(camera[1]))


def set_materials(renderer, skip: bool, shadow_strength: float, secondary_scale: float) -> int:
    changed = 0
    for mat in renderer.app.scene.get_materials():
        if not hasattr(mat, "unlit_receive_shadows"):
            continue
        mat.unlit_receive_shadows = True
        mat.unlit_shadow_strength = float(shadow_strength)
        if hasattr(mat, "skip_tone_mapping"):
            mat.skip_tone_mapping = bool(skip)
        if hasattr(mat, "camera_plate_secondary_scale"):
            mat.camera_plate_secondary_scale = max(0.0, float(secondary_scale))
        if hasattr(mat, "mark_dirty"):
            mat.mark_dirty()
        changed += 1
        print(
            f"[material] {mat.name}: unlit=True shadow={shadow_strength} "
            f"skip_tone_mapping={skip} secondary_scale={secondary_scale}"
        )
    return changed


def render_variant(args, skip: bool, out_path: Path, rtxpt, camera, c2w) -> None:
    renderer = rtxpt.Renderer(
        width=int(camera[0]),
        height=int(camera[1]),
        headless=True,
        vulkan=False,
        scene=make_scene(args.obj),
        realtime=True,
        accumulation_target=1,
    )
    try:
        print(f"[scene] bounds={getattr(renderer, 'scene_bounds', None)}")
        settings = renderer.settings
        settings.enable_tone_mapping = True
        settings.enable_bloom = False
        settings.use_nee = True
        settings.bounce_count = 1
        settings.diffuse_bounce_count = 1
        if hasattr(renderer.app, "set_realtime_mode"):
            renderer.app.set_realtime_mode(standalone_denoiser=False, realtime_aa=int(rtxpt.RealtimeAA.Off))
        else:
            settings.realtime_mode = True
            settings.realtime_aa = int(rtxpt.RealtimeAA.Off)
        apply_camera(renderer, c2w, camera, rtxpt, args.convert_rdf_to_donut, args.look_at)
        if set_materials(renderer, skip, args.shadow_strength, args.secondary_scale) == 0:
            raise RuntimeError("No material exposing unlit_receive_shadows was found")
        renderer.settings.reset_accumulation = True
        renderer.step_n(max(1, args.frames))
        if not renderer.save_screenshot(str(out_path)):
            raise RuntimeError(f"Failed to save {out_path}")
    finally:
        renderer.close()
    print(f"[saved] {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--obj", type=Path, required=True)
    parser.add_argument("--cameras", type=Path, required=True)
    parser.add_argument("--images", type=Path, required=True)
    parser.add_argument("--image-name", default="statue_alex01250.png")
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--frames", type=int, default=2)
    parser.add_argument(
        "--shadow-strength",
        type=float,
        default=0.5,
        help="Visibility-shadow strength received by unlit scan materials (0 disables shadows, 1 is full strength).",
    )
    parser.add_argument(
        "--secondary-scale",
        type=float,
        default=0.65,
        help="PBR albedo scale used when secondary rays hit a skip-tone-mapping plate.",
    )
    parser.add_argument("--convert-rdf-to-donut", action=argparse.BooleanOptionalAction, default=False)
    parser.add_argument("--look-at", type=float, nargs=3, default=None, metavar=("X", "Y", "Z"))
    args = parser.parse_args()
    args.obj = args.obj.resolve()
    args.out_dir = args.out_dir.resolve()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    import rtxpt

    camera, c2w, image_id = load_colmap_camera(args.cameras.resolve(), args.images.resolve(), args.image_name)
    print(f"[camera] image_id={image_id} size={camera[0]}x{camera[1]} fx={camera[2]:.6f} fy={camera[3]:.6f} cx={camera[4]:.6f} cy={camera[5]:.6f}")
    print(f"[camera] c2w=\n{c2w}")
    render_variant(args, False, args.out_dir / "normal_tonemap.png", rtxpt, camera, c2w)
    render_variant(args, True, args.out_dir / "skip_tonemap.png", rtxpt, camera, c2w)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
