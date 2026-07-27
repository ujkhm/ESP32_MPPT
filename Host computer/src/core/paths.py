"""Portable path helpers — all runtime data lives beside the executable."""

from __future__ import annotations

import sys
from pathlib import Path


def get_app_root() -> Path:
    """Return the folder containing the app (exe when frozen, project root in dev)."""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    # src/core/paths.py -> Host computer/
    return Path(__file__).resolve().parents[2]


def get_data_dir() -> Path:
    data = get_app_root() / "data"
    data.mkdir(parents=True, exist_ok=True)
    return data


def get_logs_dir() -> Path:
    logs = get_data_dir() / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    return logs


def get_config_path() -> Path:
    return get_data_dir() / "config.json"


def get_assets_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(getattr(sys, "_MEIPASS", get_app_root())) / "assets"
    return get_app_root() / "assets"
