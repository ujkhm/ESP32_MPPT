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
    # 上次成功配對/連上的藍牙裝置，用於下次開啟時自動重連(跳過第一/二步)
    "bluetooth": {
        "last_device_name": "",
        "last_device_id": "",  # WinRT DeviceInformation.id，配對用
        "last_device_mac": "",  # 12 碼十六進位 MAC，重連時用來重新找 COM 埠
        "last_com_port": "",  # 實際用來開序列埠的 COM 埠
    },
    # 使用者上次選擇的報表儲存資料夾，重新開啟時預設帶入
    "report": {
        "default_save_dir": "",
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
