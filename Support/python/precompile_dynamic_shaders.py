from __future__ import annotations

import argparse
import ast
import os
import sys
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
DEFAULT_SCENES = ["builtin:plane_cube"]
DEFAULT_MODES = ["reference", "realtime"]

# Empty dict = leave SampleUIData defaults alone (one baseline warmup pass).
DEFAULT_GLOBAL_VARIANTS: list[dict[str, Any]] = [{}]

# Warm common PTPipelineBaker global macros that change with Settings toggles.
COVERAGE_GLOBAL_VARIANTS: list[dict[str, Any]] = [
    {},
    {"use_nee": False},
    {"use_nee": True, "nee_candidate_samples": 8, "nee_full_samples": 1},
    {"use_nee": True, "nee_candidate_samples": 16, "nee_full_samples": 2},
    {"enable_russian_roulette": False},
    {"enable_russian_roulette": True},
    {"reference_firefly_filter_enabled": False, "realtime_firefly_filter_enabled": False},
    {"reference_firefly_filter_enabled": True, "realtime_firefly_filter_enabled": True},
    {"use_restir_di": True, "use_restir_gi": False, "use_restir_pt": False},
    {"use_restir_di": False, "use_restir_gi": True, "use_restir_pt": False},
    {"use_restir_di": False, "use_restir_gi": False, "use_restir_pt": True},
    {"use_restir_di": True, "use_restir_gi": True, "use_restir_pt": False},
]


def split_csv(value: str, default: list[str]) -> list[str]:
    items = [item.strip() for item in value.replace(";", ",").split(",") if item.strip()]
    return items or list(default)


def parse_boolish(value: str) -> Any:
    lowered = value.strip().lower()
    if lowered in {"1", "true", "yes", "on"}:
        return True
    if lowered in {"0", "false", "no", "off"}:
        return False
    try:
        return ast.literal_eval(value)
    except (ValueError, SyntaxError):
        return value


def parse_variant_spec(spec: str) -> dict[str, Any]:
    overrides: dict[str, Any] = {}
    for item in spec.replace(";", ",").split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise ValueError(f"Invalid global variant override {item!r}; expected key=value")
        key, raw = item.split("=", 1)
        key = key.strip()
        if not key:
            raise ValueError(f"Invalid global variant override {item!r}; empty key")
        overrides[key] = parse_boolish(raw.strip())
    return overrides


def resolve_global_variants(preset: str, extra_specs: list[str] | None) -> list[dict[str, Any]]:
    if preset == "default":
        variants = [dict(item) for item in DEFAULT_GLOBAL_VARIANTS]
    elif preset == "coverage":
        variants = [dict(item) for item in COVERAGE_GLOBAL_VARIANTS]
    else:
        raise ValueError(f"Unknown global variant preset: {preset}")

    for spec in extra_specs or []:
        variants.append(parse_variant_spec(spec))
    return variants


def configure_import_path() -> None:
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(str(BIN_DIR))
    sys.path.insert(0, str(BIN_DIR))


def shader_bin_dir(shader_api: str) -> Path:
    if shader_api == "d3d12":
        return BIN_DIR / "ShaderDynamic" / "Bin" / "dxil"
    if shader_api == "vulkan":
        return BIN_DIR / "ShaderDynamic" / "Bin" / "spirv"
    raise ValueError(f"Unsupported shader API: {shader_api}")


def count_shader_bins(shader_api: str) -> int:
    path = shader_bin_dir(shader_api)
    if not path.exists():
        return 0
    return sum(1 for item in path.rglob("*.bin") if item.is_file())


def format_variant(overrides: dict[str, Any]) -> str:
    if not overrides:
        return "defaults"
    return ",".join(f"{key}={value}" for key, value in overrides.items())


def coerce_setting_value(current: Any, value: Any) -> Any:
    if isinstance(current, bool):
        if isinstance(value, bool):
            return value
        if isinstance(value, (int, float)) and value in (0, 1):
            return bool(value)
        if isinstance(value, str):
            return bool(parse_boolish(value))
        raise TypeError(f"Cannot coerce {value!r} to bool")
    if isinstance(current, int) and not isinstance(current, bool):
        return int(value)
    if isinstance(current, float):
        return float(value)
    return value


def apply_settings_overrides(settings: Any, overrides: dict[str, Any]) -> None:
    for key, value in overrides.items():
        if not hasattr(settings, key):
            raise AttributeError(f"Settings has no attribute {key!r}")
        current = getattr(settings, key)
        setattr(settings, key, coerce_setting_value(current, value))


def precompile_one(
    rtxpt,
    *,
    scene: str,
    shader_api: str,
    mode: str,
    frames: int,
    overrides: dict[str, Any],
) -> None:
    realtime = mode == "realtime"
    renderer = rtxpt.Renderer(
        width=64,
        height=64,
        headless=True,
        vulkan=shader_api == "vulkan",
        scene=scene,
        realtime=realtime,
        accumulation_target=1,
    )

    try:
        settings = renderer.settings
        settings.realtime_mode = realtime
        settings.accumulation_target = 1
        settings.reset_accumulation = True
        if hasattr(settings, "realtime_aa"):
            settings.realtime_aa = int(rtxpt.RealtimeAA.Off)
        if hasattr(settings, "accumulation_prewarm_realtime_caches"):
            settings.accumulation_prewarm_realtime_caches = False
        apply_settings_overrides(settings, overrides)
        renderer.step_n(max(1, frames))
    finally:
        renderer.close()


def precompile(
    shader_api: str,
    scenes: list[str],
    modes: list[str],
    frames: int,
    variants: list[dict[str, Any]],
) -> None:
    configure_import_path()
    import rtxpt  # type: ignore

    before = count_shader_bins(shader_api)
    print(
        f"[rtxpt] Precompiling dynamic shaders: api={shader_api}, "
        f"scenes={len(scenes)}, modes={','.join(modes)}, variants={len(variants)}"
    )
    for scene in scenes:
        for mode in modes:
            for overrides in variants:
                label = format_variant(overrides)
                print(f"[rtxpt]   scene={scene!r}, mode={mode}, variant={label}")
                precompile_one(
                    rtxpt,
                    scene=scene,
                    shader_api=shader_api,
                    mode=mode,
                    frames=frames,
                    overrides=overrides,
                )

    after = count_shader_bins(shader_api)
    print(f"[rtxpt] Dynamic shader bins: {before} -> {after}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Warm RTXPT dynamic shader bins by rendering selected scenes on the build machine."
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan"],
        default="d3d12" if os.name == "nt" else "vulkan",
    )
    parser.add_argument(
        "--scene",
        action="append",
        dest="scenes",
        help="Scene to load. Repeat for multiple scenes. Defaults to builtin:plane_cube.",
    )
    parser.add_argument(
        "--modes",
        default=",".join(DEFAULT_MODES),
        help="Comma/semicolon separated modes: reference,realtime.",
    )
    parser.add_argument("--frames", type=int, default=1)
    parser.add_argument(
        "--global-variant-preset",
        choices=["default", "coverage"],
        default="default",
        help=(
            "Preset of Settings overrides used to warm PT global shader macros. "
            "'coverage' exercises common NEE / ReSTIR / firefly / roulette toggles."
        ),
    )
    parser.add_argument(
        "--global-variant",
        action="append",
        dest="global_variants",
        help=(
            "Additional Settings overrides for one shader warmup pass, for example "
            "'use_nee=0,nee_candidate_samples=8'. Repeat for multiple passes."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    modes = split_csv(args.modes, DEFAULT_MODES)
    invalid_modes = sorted(set(modes) - {"reference", "realtime"})
    if invalid_modes:
        raise ValueError(f"Unknown mode(s): {', '.join(invalid_modes)}")

    variants = resolve_global_variants(args.global_variant_preset, args.global_variants)
    precompile(
        args.shader_api,
        args.scenes or list(DEFAULT_SCENES),
        modes,
        args.frames,
        variants,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
