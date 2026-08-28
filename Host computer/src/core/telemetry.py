"""Parsed telemetry messages sent by the ESP32 firmware over Bluetooth SPP.

Wire format (see VSCode/esp32_code/src/bt_telemetry/bt_telemetry.cpp): one JSON
object per line, newline-delimited. Two message types:
  {"t":"live", ...}   pushed ~5Hz, every field that can change moment to moment
  {"t":"curve", "n":N, "pts":[[rpm,v,p,r_opt], ...]}  pushed ~every 3s once done

This module only *parses*; it does not know about serial ports or threads.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from datetime import datetime
from typing import Any, Optional

# 對應 settings.h 的 ui_app_state
UI_WAIT_START = 0
UI_RUNNING = 1
UI_ESTOP = 2
UI_LINK_LOST = 3

# 對應 settings.h 的 measure_phase
MEAS_IDLE = 0
MEAS_MIN_SPEED_HOLD = 1
MEAS_SAFE_CURRENT = 2
MEAS_RESISTANCE = 3
MEAS_CURVE_CALC = 4
MEAS_DONE = 5
MEAS_LINK_LOST = 6

MEAS_PHASE_LABELS_ZH = {
    MEAS_IDLE: "尚未開始",
    MEAS_MIN_SPEED_HOLD: "定速至最低可用轉速",
    MEAS_SAFE_CURRENT: "安全電流測試",
    MEAS_RESISTANCE: "內阻測試",
    MEAS_CURVE_CALC: "曲線計算",
    MEAS_DONE: "測試完成",
    MEAS_LINK_LOST: "發電機斷線暫停",
}


@dataclass
class LiveSnapshot:
    received_at: datetime
    ts_ms: int = 0

    ui_state: int = 0
    ui_state_name: str = "UNKNOWN"

    fault: bool = False
    fault_code: int = 0
    fault_name: str = "NONE"

    phase: int = 0
    phase_name: str = "UNKNOWN"

    now_speed: float = 0.0
    keep_rpm: float = 0.0
    speed_valid: bool = False
    speed_stable: bool = False
    const_speed_ready: bool = False

    ina_online: bool = False
    ina_valid: bool = False
    bus_v: float = 0.0
    current_a: float = 0.0
    power_w: float = 0.0

    meas_phase: int = 0
    meas_phase_name: str = "UNKNOWN"
    resume_phase: int = 0
    link_lost: bool = False
    load_connected: bool = False
    session_active: bool = False

    safe_phase: int = 0
    safe_target_rpm: float = 0.0
    safe_oc_v: float = 0.0
    safe_electrical_a: float = 0.0
    safe_hot_a: float = 0.0
    safe_droop_ratio: float = 0.0
    safe_phase_elapsed_ms: int = 0
    safe_done: bool = False
    safe_pass_any: bool = False
    safe_i_cont_a: float = 0.0
    safe_i_cont_rpm: float = 0.0

    res_phase: int = 0
    res_point_index: int = 0
    res_target_rpm: float = 0.0
    res_oc_v: float = 0.0
    res_load_v: float = 0.0
    res_load_a: float = 0.0
    res_phase_elapsed_ms: int = 0
    res_done: bool = False
    res_valid_points: int = 0
    res_rth_ohm: float = 0.0
    res_ke_v_per_rpm: float = 0.0

    curve_done: bool = False
    curve_n_rl: float = 0.0
    curve_n_knee: float = 0.0
    curve_n_voc: float = 0.0
    curve_n_lim: float = 0.0
    curve_limit_reason: int = 0
    curve_point_count: int = 0

    @staticmethod
    def from_dict(d: dict[str, Any]) -> "LiveSnapshot":
        def g(key: str, default: Any) -> Any:
            v = d.get(key, default)
            return default if v is None else v

        return LiveSnapshot(
            received_at=datetime.now(),
            ts_ms=int(g("ts", 0)),
            ui_state=int(g("ui_state", 0)),
            ui_state_name=str(g("ui_state_name", "UNKNOWN")),
            fault=bool(g("fault", False)),
            fault_code=int(g("fault_code", 0)),
            fault_name=str(g("fault_name", "NONE")),
            phase=int(g("phase", 0)),
            phase_name=str(g("phase_name", "UNKNOWN")),
            now_speed=float(g("now_speed", 0.0)),
            keep_rpm=float(g("keep_rpm", 0.0)),
            speed_valid=bool(g("speed_valid", False)),
            speed_stable=bool(g("speed_stable", False)),
            const_speed_ready=bool(g("const_speed_ready", False)),
            ina_online=bool(g("ina_online", False)),
            ina_valid=bool(g("ina_valid", False)),
            bus_v=float(g("bus_V", 0.0)),
            current_a=float(g("current_A", 0.0)),
            power_w=float(g("power_W", 0.0)),
            meas_phase=int(g("meas_phase", 0)),
            meas_phase_name=str(g("meas_phase_name", "UNKNOWN")),
            resume_phase=int(g("resume_phase", 0)),
            link_lost=bool(g("link_lost", False)),
            load_connected=bool(g("load_connected", False)),
            session_active=bool(g("session_active", False)),
            safe_phase=int(g("safe_phase", 0)),
            safe_target_rpm=float(g("safe_target_rpm", 0.0)),
            safe_oc_v=float(g("safe_oc_V", 0.0)),
            safe_electrical_a=float(g("safe_electrical_A", 0.0)),
            safe_hot_a=float(g("safe_hot_A", 0.0)),
            safe_droop_ratio=float(g("safe_droop_ratio", 0.0)),
            safe_phase_elapsed_ms=int(g("safe_phase_elapsed_ms", 0)),
            safe_done=bool(g("safe_done", False)),
            safe_pass_any=bool(g("safe_pass_any", False)),
            safe_i_cont_a=float(g("safe_i_cont_A", 0.0)),
            safe_i_cont_rpm=float(g("safe_i_cont_rpm", 0.0)),
            res_phase=int(g("res_phase", 0)),
            res_point_index=int(g("res_point_index", 0)),
            res_target_rpm=float(g("res_target_rpm", 0.0)),
            res_oc_v=float(g("res_oc_V", 0.0)),
            res_load_v=float(g("res_load_V", 0.0)),
            res_load_a=float(g("res_load_A", 0.0)),
            res_phase_elapsed_ms=int(g("res_phase_elapsed_ms", 0)),
            res_done=bool(g("res_done", False)),
            res_valid_points=int(g("res_valid_points", 0)),
            res_rth_ohm=float(g("res_rth_ohm", 0.0)),
            res_ke_v_per_rpm=float(g("res_ke_v_per_rpm", 0.0)),
            curve_done=bool(g("curve_done", False)),
            curve_n_rl=float(g("curve_n_rl", 0.0)),
            curve_n_knee=float(g("curve_n_knee", 0.0)),
            curve_n_voc=float(g("curve_n_voc", 0.0)),
            curve_n_lim=float(g("curve_n_lim", 0.0)),
            curve_limit_reason=int(g("curve_limit_reason", 0)),
            curve_point_count=int(g("curve_point_count", 0)),
        )

    @property
    def has_fault_like_condition(self) -> bool:
        """Anything that should pop the non-dismissable warning dialog."""
        return self.fault or self.link_lost


@dataclass
class CurvePoint:
    rpm: float
    v_at_max_p: float
    p_max: float
    r_opt: float


@dataclass
class CurveTable:
    received_at: datetime
    points: list[CurvePoint] = field(default_factory=list)

    @staticmethod
    def from_dict(d: dict[str, Any]) -> Optional["CurveTable"]:
        pts_raw = d.get("pts")
        if not isinstance(pts_raw, list):
            return None
        points: list[CurvePoint] = []
        for p in pts_raw:
            if not isinstance(p, list) or len(p) < 4:
                continue
            try:
                points.append(CurvePoint(rpm=float(p[0]), v_at_max_p=float(p[1]),
                                          p_max=float(p[2]), r_opt=float(p[3])))
            except (TypeError, ValueError):
                continue
        return CurveTable(received_at=datetime.now(), points=points)


def parse_telemetry_line(line: str) -> tuple[Optional[LiveSnapshot], Optional[CurveTable]]:
    """Parse one line of telemetry. Returns (live, curve); at most one is not None.
    Malformed/partial lines (common right after a BT link comes up mid-line) are
    silently ignored -- returning (None, None) -- rather than raising, since the
    caller reads a continuous stream and a single bad line must not kill it."""
    line = line.strip()
    if not line:
        return None, None
    try:
        d = json.loads(line)
    except json.JSONDecodeError:
        return None, None
    if not isinstance(d, dict):
        return None, None

    kind = d.get("t")
    if kind == "live":
        try:
            return LiveSnapshot.from_dict(d), None
        except (TypeError, ValueError):
            return None, None
    if kind == "curve":
        return None, CurveTable.from_dict(d)
    return None, None
