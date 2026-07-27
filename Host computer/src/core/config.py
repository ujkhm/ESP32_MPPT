"""Load and save host application settings."""

from __future__ import annotations

import json
from copy import deepcopy
from typing import Any

from core.paths import get_config_path

DEFAULT_CONFIG: dict[str, Any] = {
    "serial": {
        "port": "",
        "baudrate": 115200,
        "timeout": 1.0,
    },
    "logging": {
        "auto_save_csv": True,
        "csv_prefix": "speed_log",
    },
    "ui": {
        "refresh_ms": 200,
        "chart_max_points": 300,
    },
}


def load_config() -> dict[str, Any]:
    path = get_config_path()
    if not path.exists():
        save_config(DEFAULT_CONFIG)
        return deepcopy(DEFAULT_CONFIG)

    with path.open(encoding="utf-8") as f:
        data = json.load(f)

    merged = deepcopy(DEFAULT_CONFIG)
    for section, values in data.items():
        if section in merged and isinstance(values, dict):
            merged[section].update(values)
        else:
            merged[section] = values
    return merged


def save_config(config: dict[str, Any]) -> None:
    path = get_config_path()
    with path.open("w", encoding="utf-8") as f:
        json.dump(config, f, indent=2, ensure_ascii=False)
