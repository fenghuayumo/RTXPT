from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import sysconfig
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BIN_DIR = ROOT / "bin"
PYTHON_PACKAGE_DIR = ROOT / "python" / "rtxpt"
BUILD_DIR = ROOT / "build" / "python-wheel"
STAGING_DIR = BUILD_DIR / "staging"
DIST_DIR = ROOT / "dist"


MINIMAL_ASSET_FILES = [
    "ArtLicenses.txt",
    "README.md",
    "default.json",
    "loading_splash.png",
]

def copy_file(src: Path, dst: Path) -> None:
    if not src.exists():
        raise FileNotFoundError(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def copy_optional_file(src: Path, dst: Path) -> None:
    if not src.exists():
        print(f"WARNING: optional runtime asset not found, skipping: {src}")
        return
    copy_file(src, dst)


def copy_tree(
    src: Path,
    dst: Path,
    *,
    suffixes: set[str] | None = None,
    path_filter: set[str] | None = None,
) -> None:
    if not src.exists():
        raise FileNotFoundError(src)

    for item in src.rglob("*"):
        if not item.is_file():
            continue
        if suffixes is not None and item.suffix.lower() not in suffixes:
            continue
        if path_filter is not None and not (set(item.relative_to(src).parts) & path_filter):
            continue
        copy_file(item, dst / item.relative_to(src))


def directory_size(path: Path) -> int:
    return sum(item.stat().st_size for item in path.rglob("*") if item.is_file())


def find_native_extension() -> Path:
    ext_suffix = sysconfig.get_config_var("EXT_SUFFIX")
    if ext_suffix:
        exact = BIN_DIR / f"rtxpt{ext_suffix}"
        if exact.exists():
            return exact

    candidates = sorted(BIN_DIR.glob("rtxpt*.pyd" if os.name == "nt" else "rtxpt*.so"))
    if candidates:
        return candidates[-1]

    raise FileNotFoundError(
        f"No RTXPT Python extension found in {BIN_DIR}. Build target 'rtxpt_py' first."
    )


def copy_runtime_files(package_dir: Path, *, dynamic_shaders: str, shader_api: str, assets: str) -> None:
    native_extension = find_native_extension()
    copy_file(native_extension, package_dir / native_extension.name)

    if os.name == "nt":
        for item in BIN_DIR.iterdir():
            if item.is_file() and item.suffix.lower() in {".dll", ".json"}:
                copy_file(item, package_dir / item.name)
        if (BIN_DIR / "D3D12").exists():
            copy_tree(BIN_DIR / "D3D12", package_dir / "D3D12")
    else:
        for item in BIN_DIR.iterdir():
            if item.is_file() and (item.suffix == ".so" or ".so." in item.name):
                copy_file(item, package_dir / item.name)

    shader_filter = None
    tool_filter = None
    if shader_api == "d3d12":
        shader_filter = {"dxil"}
        tool_filter = {"d3d12"}
    elif shader_api == "vulkan":
        shader_filter = {"spirv"}
        tool_filter = {"vk"}
    elif shader_api != "both":
        raise ValueError(f"Unknown shader API: {shader_api}")

    copy_tree(
        BIN_DIR / "ShaderPrecompiled",
        package_dir / "ShaderPrecompiled",
        path_filter=shader_filter,
    )

    if dynamic_shaders in {"bin", "full"} and (BIN_DIR / "ShaderDynamic" / "Bin").exists():
        copy_tree(
            BIN_DIR / "ShaderDynamic" / "Bin",
            package_dir / "ShaderDynamic" / "Bin",
            suffixes={".bin"},
            path_filter=shader_filter,
        )

    if dynamic_shaders == "full":
        if (BIN_DIR / "ShaderDynamic" / "Source").exists():
            copy_tree(BIN_DIR / "ShaderDynamic" / "Source", package_dir / "ShaderDynamic" / "Source")
        if (BIN_DIR / "ShaderDynamic" / "Tools").exists():
            copy_tree(
                BIN_DIR / "ShaderDynamic" / "Tools",
                package_dir / "ShaderDynamic" / "Tools",
                suffixes={".dll", ".exe", ".json", ".marker", ".so", ""},
                path_filter=tool_filter,
            )

    if assets == "minimal":
        for relative in MINIMAL_ASSET_FILES:
            copy_file(ROOT / "Assets" / relative, package_dir / "Assets" / relative)
        copy_tree(ROOT / "Assets" / "Fonts", package_dir / "Assets" / "Fonts")
    elif assets == "full":
        copy_tree(ROOT / "Assets", package_dir / "Assets")
    elif assets == "none":
        keep = package_dir / "Assets" / ".rtxpt-wheel-runtime"
        keep.parent.mkdir(parents=True, exist_ok=True)
        keep.write_text("Runtime asset root placeholder for RTXPT wheels.\n", encoding="utf-8")
    else:
        raise ValueError(f"Unknown assets mode: {assets}")


def write_build_project(version: str) -> None:
    (STAGING_DIR / "pyproject.toml").write_text(
        """[build-system]
requires = ["setuptools>=68", "wheel"]
build-backend = "setuptools.build_meta"
""",
        encoding="utf-8",
    )

    (STAGING_DIR / "MANIFEST.in").write_text(
        "recursive-include rtxpt *\n",
        encoding="utf-8",
    )

    (STAGING_DIR / "README.md").write_text(
        """# RTXPT Python Wheel

Local binary wheel assembled from the current RTXPT build output.
""",
        encoding="utf-8",
    )

    (STAGING_DIR / "setup.py").write_text(
        f"""from setuptools import Distribution, setup


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


setup(
    name="rtxpt",
    version={version!r},
    description="Python bindings for RTXPT",
    long_description=open("README.md", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    packages=["rtxpt"],
    include_package_data=True,
    license_files=["LICENSE.txt"],
    distclass=BinaryDistribution,
    python_requires=">=3.8",
)
""",
        encoding="utf-8",
    )

    copy_file(ROOT / "LICENSE.txt", STAGING_DIR / "LICENSE.txt")


def build_wheel() -> Path:
    DIST_DIR.mkdir(parents=True, exist_ok=True)
    before = set(DIST_DIR.glob("rtxpt-*.whl"))
    subprocess.run(
        [
            sys.executable,
            "-m",
            "pip",
            "wheel",
            "--no-deps",
            "--no-build-isolation",
            "--wheel-dir",
            str(DIST_DIR),
            str(STAGING_DIR),
        ],
        check=True,
        cwd=ROOT,
    )
    after = set(DIST_DIR.glob("rtxpt-*.whl"))
    created = sorted(after - before, key=lambda path: path.stat().st_mtime)
    if created:
        return created[-1]
    existing = sorted(after, key=lambda path: path.stat().st_mtime)
    if existing:
        return existing[-1]
    raise RuntimeError("pip did not produce an rtxpt wheel")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a local RTXPT Python wheel from bin/.")
    parser.add_argument("--version", default="0.1.0", help="Wheel package version.")
    parser.add_argument(
        "--assets",
        choices=["minimal", "full", "none"],
        default="minimal",
        help="Asset payload to include. 'full' is very large.",
    )
    parser.add_argument(
        "--dynamic-shaders",
        choices=["full", "bin", "none"],
        default="bin",
        help=(
            "ShaderDynamic payload. 'bin' includes compiled runtime variants only; "
            "'full' also includes Source and Tools for runtime compilation; "
            "'none' omits ShaderDynamic."
        ),
    )
    parser.add_argument(
        "--shader-api",
        choices=["d3d12", "vulkan", "both"],
        default="d3d12" if os.name == "nt" else "vulkan",
        help="Shader backend payload to include. Windows wheels default to D3D12 only.",
    )
    parser.add_argument(
        "--no-dynamic-shader-bin",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if os.name != "nt" and args.shader_api == "d3d12":
        raise ValueError("D3D12 shader payload is only valid for Windows wheels. Use --shader-api vulkan on Linux.")

    if not BIN_DIR.exists():
        raise FileNotFoundError(f"{BIN_DIR} does not exist. Build RTXPT first.")
    if not PYTHON_PACKAGE_DIR.exists():
        raise FileNotFoundError(f"{PYTHON_PACKAGE_DIR} does not exist.")

    if STAGING_DIR.exists():
        shutil.rmtree(STAGING_DIR)
    STAGING_DIR.mkdir(parents=True)

    package_dir = STAGING_DIR / "rtxpt"
    shutil.copytree(PYTHON_PACKAGE_DIR, package_dir)

    dynamic_shaders = "none" if args.no_dynamic_shader_bin else args.dynamic_shaders

    copy_runtime_files(
        package_dir,
        dynamic_shaders=dynamic_shaders,
        shader_api=args.shader_api,
        assets=args.assets,
    )
    write_build_project(args.version)

    print(f"Staged package size: {directory_size(package_dir) / (1024 * 1024):.1f} MiB")
    wheel = build_wheel()
    print(f"Built wheel: {wheel}")
    print(f"Wheel size: {wheel.stat().st_size / (1024 * 1024):.1f} MiB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
