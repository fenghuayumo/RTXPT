from __future__ import annotations

import os
import shutil
import sys
from pathlib import Path

from setuptools import Distribution, setup
from setuptools.command.build_py import build_py as _build_py


ROOT = Path(__file__).resolve().parent
SUPPORT_PYTHON = ROOT / "Support" / "python"
sys.path.insert(0, str(SUPPORT_PYTHON))

from build_wheel import (  # noqa: E402
    BIN_DIR,
    PYTHON_PACKAGE_DIR,
    copy_runtime_files,
    directory_size,
)


def env_choice(name: str, default: str, choices: set[str]) -> str:
    value = os.environ.get(name, default).lower()
    if value not in choices:
        allowed = ", ".join(sorted(choices))
        raise RuntimeError(f"{name} must be one of: {allowed}")
    return value


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:
        return True


class BuildPyWithRuntime(_build_py):
    def run(self) -> None:
        if not BIN_DIR.exists():
            raise FileNotFoundError(f"{BIN_DIR} does not exist. Build RTXPT first.")
        if not PYTHON_PACKAGE_DIR.exists():
            raise FileNotFoundError(f"{PYTHON_PACKAGE_DIR} does not exist.")

        package_dir = Path(self.build_lib) / "rtxpt"
        if package_dir.exists():
            shutil.rmtree(package_dir)

        super().run()

        assets = env_choice("RTXPT_WHEEL_ASSETS", "minimal", {"minimal", "full", "none"})
        dynamic_shaders = env_choice(
            "RTXPT_WHEEL_DYNAMIC_SHADERS",
            "bin",
            {"full", "bin", "none"},
        )
        shader_api = env_choice(
            "RTXPT_WHEEL_SHADER_API",
            "d3d12" if os.name == "nt" else "vulkan",
            {"d3d12", "vulkan", "both"},
        )
        if os.name != "nt" and shader_api == "d3d12":
            raise RuntimeError(
                "RTXPT_WHEEL_SHADER_API=d3d12 is only valid on Windows. "
                "Use RTXPT_WHEEL_SHADER_API=vulkan on Linux."
            )

        copy_runtime_files(
            package_dir,
            dynamic_shaders=dynamic_shaders,
            shader_api=shader_api,
            assets=assets,
        )
        size_mib = directory_size(package_dir) / (1024 * 1024)
        print(f"Staged rtxpt package size: {size_mib:.1f} MiB")

    def get_outputs(self, include_bytecode: int = 1) -> list[str]:
        outputs = super().get_outputs(include_bytecode)
        package_dir = Path(self.build_lib) / "rtxpt"
        if package_dir.exists():
            outputs.extend(str(path) for path in package_dir.rglob("*") if path.is_file())
        return outputs


setup(
    name="rtxpt",
    version=os.environ.get("RTXPT_WHEEL_VERSION", "0.2.0"),
    description="Python bindings for RTXPT",
    long_description=(ROOT / "py_rtxpt.md").read_text(encoding="utf-8"),
    long_description_content_type="text/markdown",
    packages=["rtxpt"],
    package_dir={"rtxpt": "python/rtxpt"},
    include_package_data=True,
    license_files=["LICENSE.txt"],
    python_requires=">=3.8",
    distclass=BinaryDistribution,
    cmdclass={"build_py": BuildPyWithRuntime},
    zip_safe=False,
)
