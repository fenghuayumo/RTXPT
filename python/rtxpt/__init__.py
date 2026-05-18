"""Python package loader for the RTXPT native extension."""

from __future__ import annotations

import os
from pathlib import Path

_PACKAGE_DIR = Path(__file__).resolve().parent
_DLL_DIRECTORY_COOKIE = None

if hasattr(os, "add_dll_directory"):
    _DLL_DIRECTORY_COOKIE = os.add_dll_directory(str(_PACKAGE_DIR))

try:
    from .rtxpt import *  # noqa: F401,F403
except ImportError as exc:
    raise ImportError(
        "Failed to import the RTXPT native extension. "
        "Make sure the wheel was built with the RTXPT .pyd/.so and runtime DLLs."
    ) from exc

