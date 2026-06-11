"""Python package loader for the RTXPT native extension."""

from __future__ import annotations

import ctypes
import importlib
import os
import sys
from pathlib import Path

_PACKAGE_DIR = Path(__file__).resolve().parent
_DLL_DIRECTORY_COOKIE = None
_PRELOADED_SHARED_LIBRARIES: list[ctypes.CDLL] = []

if hasattr(os, "add_dll_directory"):
    _DLL_DIRECTORY_COOKIE = os.add_dll_directory(str(_PACKAGE_DIR))


def _is_native_extension(path: Path) -> bool:
    name = path.name
    return name.startswith("rtxpt") and (
        path.suffix in {".pyd", ".so"} or ".so." in name
    )


def _iter_package_shared_libraries() -> list[Path]:
    return sorted(
        path
        for path in _PACKAGE_DIR.iterdir()
        if path.is_file() and (path.suffix == ".so" or ".so." in path.name)
    )


def _preload_posix_shared_libraries() -> None:
    if os.name == "nt":
        return

    pending = [
        path
        for path in _iter_package_shared_libraries()
        if not _is_native_extension(path)
    ]
    mode = getattr(os, "RTLD_NOW", 0) | getattr(os, "RTLD_GLOBAL", 0)
    loaded_any = True

    # Some sibling libraries depend on each other. Retry in passes so that
    # absolute-path loads that become satisfiable later still get pulled in.
    while pending and loaded_any:
        loaded_any = False
        remaining: list[Path] = []
        for library_path in pending:
            try:
                _PRELOADED_SHARED_LIBRARIES.append(
                    ctypes.CDLL(str(library_path), mode=mode)
                )
                loaded_any = True
            except OSError:
                remaining.append(library_path)
        pending = remaining


_preload_posix_shared_libraries()

_saved_dlopen_flags = None
if os.name != "nt" and hasattr(sys, "getdlopenflags") and hasattr(sys, "setdlopenflags"):
    _saved_dlopen_flags = sys.getdlopenflags()
    sys.setdlopenflags(_saved_dlopen_flags | getattr(os, "RTLD_GLOBAL", 0))

try:
    _native_module = importlib.import_module(".rtxpt", __name__)
except ImportError as exc:
    raise ImportError(
        "Failed to import the RTXPT native extension. "
        "Make sure the wheel was built with the RTXPT .pyd/.so and runtime DLLs."
    ) from exc
finally:
    if _saved_dlopen_flags is not None:
        sys.setdlopenflags(_saved_dlopen_flags)

MODE = getattr(_native_module, "MODE", "extension")
PACKAGE_DIR = str(_PACKAGE_DIR)

for _name in dir(_native_module):
    if _name.startswith("_"):
        continue
    globals()[_name] = getattr(_native_module, _name)

