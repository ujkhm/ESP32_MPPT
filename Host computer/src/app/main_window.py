"""Main window for the micro generator host app.

A 4-step wizard (配對裝置 → 選擇儲存位置 → 放置並啟動 → 執行中) driven mostly by
telemetry rather than button clicks: once Bluetooth is connected, which sub-view
is shown is derived from the firmware's own reported state, so re-opening the
app after the device already started testing lands you straight on the correct
screen instead of replaying the wizard from scratch.
"""

from __future__ import annotations

import threading
import tkinter as tk
from collections import deque
from datetime import datetime
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Optional

from core.bt_pairing import BtDeviceInfo, BtPairingWorker
from core.config import load_config, save_config
from core.report_pdf import ReportData, generate_report_pdf
from core.serial_worker import SerialWorker
from core.session_history import HistoryEntry, SessionHistory
from core.telemetry import (
    MEAS_DONE,
    MEAS_PHASE_LABELS_ZH,
    MEAS_RESISTANCE,
    MEAS_SAFE_CURRENT,
    UI_WAIT_START,
    CurveTable,
    LiveSnapshot,
)

from .progress_bar import ProgressBar

_WHITE = "#ffffff"
_TEXT = "#1f2937"
_MUTED = "#6b7280"
_ACCENT = "#06b6d4"

_STEP_PAIRING = "pairing"
_STEP_FOLDER = "folder"
_STEP_CONNECTED = "connected"  # 內部再依 ui_state 分「放置並啟動」/「執行中」兩種畫面

_PROGRESS_LABELS = ["配對裝置", "選擇儲存位置", "放置並啟動", "執行中"]

_BAUDRATE = 115200
_SERIAL_TIMEOUT = 1.0
# 自動重連：下位機重開機(尤其是硬故障後需要重開機才能恢復的情況)到韌體重新初始化、
# 藍牙重新可以連線，往往需要幾秒到十幾秒；用「單次逾時就放棄、退回手動配對」等於
# 使用者每次重開機都要手動點一次「更改配對機器」，因此改成持續重試一段夠長的總時間
# (每次嘗試 _AUTO_RECONNECT_RETRY_MS，中間留 _AUTO_RECONNECT_RETRY_GAP_MS 讓 COM 埠
# 乾淨釋放，總共嘗試 _AUTO_RECONNECT_MAX_ATTEMPTS 次)，真正做到「重開機後自動接上」。
_AUTO_RECONNECT_RETRY_MS = 5000
_AUTO_RECONNECT_RETRY_GAP_MS = 1200
_AUTO_RECONNECT_MAX_ATTEMPTS = 24  # 24 * (5+1.2)秒 ≈ 149 秒，涵蓋一次完整重開機的合理時間
_PORT_BUSY_BACKOFF_MS = 2500  # COM 埠被佔用時多等一會再試，避免 PermissionError 連環

_FAULT_CODE_ZH = {
    0: "無",
    1: "轉速飛車",
    2: "馬達通電看門狗逾時",
    3: "開環已達上限仍無法達到可讀轉速",
    4: "PID 自動調參失敗",
    5: "緊急停止(ESTOP)",
    6: "量測安全鎖定",
}


class MainWindow(tk.Tk):
    APP_TITLE = "micro generator 上位機"

    def __init__(self) -> None:
        super().__init__()
        self.config_data = load_config()

        self.title(self.APP_TITLE)
        self.geometry("960x720")
        self.minsize(820, 600)
        self.configure(bg=_WHITE)
        self._apply_white_theme()

        # ---- 狀態 ----
        self._pending: deque[tuple[str, object]] = deque()
        self._latest_live: Optional[LiveSnapshot] = None
        self._latest_curve: Optional[CurveTable] = None
        self._last_meas_phase_seen: Optional[int] = None
        self._fault_popup: Optional[tk.Toplevel] = None
        self._fault_popup_reason: Optional[str] = None
        self._history = SessionHistory()
        self._pending_entry: Optional[HistoryEntry] = None
        self._save_dir = self.config_data["report"]["default_save_dir"] or str(Path.home() / "Documents")
        self._is_reconnect_flow = False
        self._auto_reconnect_after_id: Optional[str] = None
        self._auto_reconnect_attempt = 0
        self._reconnect_active = False
        self._awaiting_live = False
        self._bt_scan_devices: dict[str, BtDeviceInfo] = {}
        self._current_port: Optional[str] = None
        self._connected_subview: Optional[str] = None  # "wait" | "live"
        self._live_extra_mode: Optional[int] = None  # MEAS_SAFE_CURRENT / MEAS_RESISTANCE / other
        self._live_phase_var: Optional[tk.StringVar] = None
        self._live_rpm_var: Optional[tk.StringVar] = None
        self._live_elec_var: Optional[tk.StringVar] = None
        self._live_load_var: Optional[tk.StringVar] = None
        self._live_extra1_label_var: Optional[tk.StringVar] = None
        self._live_extra1_value_var: Optional[tk.StringVar] = None
        self._live_extra2_label_var: Optional[tk.StringVar] = None
        self._live_extra2_value_var: Optional[tk.StringVar] = None
        self._save_frame: Optional[ttk.Frame] = None
        self._save_done_shown = False

        self._bt = BtPairingWorker()
        self._bt.start()
        self._serial = SerialWorker(
            on_live=self._on_live_bg,
            on_curve=self._on_curve_bg,
            on_status=self._on_status_bg,
            on_error=self._on_error_bg,
        )

        self._current_step = _STEP_PAIRING
        self._build_shell()
        self._go_pairing_step(auto=True)

        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(200, self._poll)

    # ================================================================= 外殼 UI
    def _apply_white_theme(self) -> None:
        style = ttk.Style(self)
        try:
            style.theme_use("clam")
        except tk.TclError:
            pass
        style.configure(".", background=_WHITE, foreground=_TEXT, font=("Segoe UI", 10))
        style.configure("TFrame", background=_WHITE)
        style.configure("TLabel", background=_WHITE, foreground=_TEXT)
        style.configure("Muted.TLabel", background=_WHITE, foreground=_MUTED)
        style.configure("Title.TLabel", background=_WHITE, foreground=_TEXT, font=("Segoe UI", 15, "bold"))
        style.configure("TButton", padding=8)
        style.configure("Accent.TButton", padding=8)
        style.map("Accent.TButton", background=[("!disabled", _ACCENT)])

    def _build_shell(self) -> None:
        self.progress = ProgressBar(self, _PROGRESS_LABELS)
        self.progress.pack(fill=tk.X, padx=10, pady=(14, 4))

        self.content = ttk.Frame(self, padding=24)
        self.content.pack(fill=tk.BOTH, expand=True)

        bottom = ttk.Frame(self, padding=(16, 8))
        bottom.pack(fill=tk.X, side=tk.BOTTOM)
        ttk.Separator(self, orient=tk.HORIZONTAL).pack(fill=tk.X, side=tk.BOTTOM)
        self.status_var = tk.StringVar(value="")
        ttk.Label(bottom, textvariable=self.status_var, style="Muted.TLabel").pack(side=tk.LEFT)
        ttk.Button(bottom, text="更改配對機器", command=self._on_change_device).pack(side=tk.RIGHT)
        ttk.Button(bottom, text="歷史紀錄", command=self._show_history_window).pack(side=tk.RIGHT, padx=(0, 8))

    def _clear_content(self) -> None:
        for child in self.content.winfo_children():
            child.destroy()

    def _set_progress(self, index: int) -> None:
        self.progress.set_current(index)

    # ============================================================ Step 1：配對
    def _go_pairing_step(self, *, auto: bool) -> None:
        self._current_step = _STEP_PAIRING
        self._set_progress(0)
        self._clear_content()
        self._is_reconnect_flow = auto
        self._cancel_auto_reconnect_timer()
        self._reconnect_active = auto
        self._awaiting_live = auto

        if auto:
            self._auto_reconnect_attempt = 0
            self._render_pairing_auto()
            self._begin_auto_reconnect()
        else:
            self._reconnect_active = False
            self._awaiting_live = False
            self._render_pairing_manual()

    def _render_pairing_auto(self) -> None:
        last_port = self.config_data["bluetooth"]["last_com_port"] or "（未知）"
        ttk.Label(self.content, text="正在重新連接裝置", style="Title.TLabel").pack(anchor=tk.W)
        self._auto_reconnect_status_var = tk.StringVar(
            value=f"偵測到上次連線過的裝置（{last_port}），正在嘗試自動重新連接…"
        )
        ttk.Label(
            self.content, textvariable=self._auto_reconnect_status_var,
            style="Muted.TLabel", wraplength=760, justify=tk.LEFT,
        ).pack(anchor=tk.W, pady=(12, 20))
        ttk.Label(
            self.content,
            text="（例如下位機剛按過重開機／重置，重新開機加上藍牙重新連上通常需要一點時間，"
                 "這裡會持續自動重試，不需要手動操作）",
            style="Muted.TLabel", wraplength=760, justify=tk.LEFT,
        ).pack(anchor=tk.W, pady=(0, 20))
        ttk.Button(self.content, text="改為手動配對", command=lambda: self._go_pairing_step(auto=False)) \
            .pack(anchor=tk.W)

    def _cancel_auto_reconnect_timer(self) -> None:
        if self._auto_reconnect_after_id is not None:
            self.after_cancel(self._auto_reconnect_after_id)
            self._auto_reconnect_after_id = None

    def _resolve_reconnect_port(self) -> Optional[str]:
        mac = (self.config_data["bluetooth"].get("last_device_mac") or "").strip()
        if mac:
            port = SerialWorker.find_port_by_mac(mac)
            if port:
                return port
        cached = (self.config_data["bluetooth"].get("last_com_port") or "").strip()
        return cached or None

    def _begin_auto_reconnect(self) -> None:
        port = self._resolve_reconnect_port()
        if not port:
            self.status_var.set("找不到上次連線的序列埠，請手動配對")
            self._go_pairing_step(auto=False)
            return

        def _after_bt_check(ready: bool, message: str) -> None:
            self._pending.append(("bt_ready", (ready, message, port)))

        self._bt.check_bluetooth_ready(on_result=_after_bt_check)

    def _continue_auto_reconnect(self, port: str, *, extra_delay_ms: int = 0) -> None:
        if not self._reconnect_active or self._current_step != _STEP_PAIRING:
            return

        def _start() -> None:
            if not self._reconnect_active or self._current_step != _STEP_PAIRING:
                return
            if self._serial.is_running:
                self._serial.stop()
            SerialWorker.wait_for_port_release(port, timeout_s=2.0)
            self._current_port = port
            self._awaiting_live = True
            self._serial.start(port, _BAUDRATE, _SERIAL_TIMEOUT)
            self._auto_reconnect_after_id = self.after(_AUTO_RECONNECT_RETRY_MS, self._auto_reconnect_timeout)

        delay = max(0, extra_delay_ms)
        if delay:
            self._auto_reconnect_after_id = self.after(delay, _start)
        else:
            _start()

    def _schedule_auto_reconnect_retry(self, port: str, *, port_busy: bool = False) -> None:
        if not self._reconnect_active:
            return
        self._auto_reconnect_attempt += 1
        if self._auto_reconnect_attempt >= _AUTO_RECONNECT_MAX_ATTEMPTS:
            self.status_var.set("自動重新連接逾時，請手動配對")
            self._reconnect_active = False
            self._go_pairing_step(auto=False)
            return

        gap = _PORT_BUSY_BACKOFF_MS if port_busy else _AUTO_RECONNECT_RETRY_GAP_MS
        if hasattr(self, "_auto_reconnect_status_var"):
            reason = "序列埠仍被佔用" if port_busy else "尚未連上"
            self._auto_reconnect_status_var.set(
                f"{reason}，持續重試中…（第 {self._auto_reconnect_attempt} 次，"
                f"可能是下位機正在重新開機）"
            )
        self._auto_reconnect_after_id = self.after(
            gap,
            lambda p=port: self._continue_auto_reconnect(p),
        )

    def _try_auto_reconnect(self, port: str) -> None:
        self._continue_auto_reconnect(port)

    def _auto_reconnect_timeout(self) -> None:
        self._auto_reconnect_after_id = None
        if self._latest_live is not None:
            return  # 已經連上、_poll() 那邊已經處理過了

        if self._serial.is_running:
            self._serial.stop()
        port = self._resolve_reconnect_port()
        if port:
            self._schedule_auto_reconnect_retry(port)
        else:
            self.status_var.set("找不到上次連線的序列埠，請手動配對")
            self._reconnect_active = False
            self._go_pairing_step(auto=False)

    def _render_pairing_manual(self) -> None:
        ttk.Label(self.content, text="配對藍牙裝置", style="Title.TLabel").pack(anchor=tk.W)
        ttk.Label(
            self.content,
            text="請先確認 micro generator 下位機已經開機。已經配對過的裝置會自動列在下方；"
                 "第一次使用請點「掃描裝置」找到 micro generator 後點「配對並連線」。",
            style="Muted.TLabel", wraplength=760, justify=tk.LEFT,
        ).pack(anchor=tk.W, pady=(12, 16))

        # 手動選 COM 埠的區塊固定只有這一個 frame，之後不管重試幾次配對都只清空重畫，
        # 不會像先前那樣每次找不到序列埠就多疊一份下拉選單出來。
        self._fallback_frame = ttk.Frame(self.content)

        if not self._bt.available:
            ttk.Label(
                self.content,
                text="（本機缺少藍牙配對所需的元件，改用下方序列埠清單手動選擇）",
                style="Muted.TLabel", wraplength=760,
            ).pack(anchor=tk.W, pady=(0, 12))
            self._fallback_frame.pack(fill=tk.X, pady=(0, 0))
            self._render_manual_com_fallback()
            return

        btn_row = ttk.Frame(self.content)
        btn_row.pack(fill=tk.X, pady=(0, 10))
        ttk.Button(btn_row, text="掃描裝置", command=self._start_scan).pack(side=tk.LEFT)

        self._device_list_frame = ttk.Frame(self.content)
        self._device_list_frame.pack(fill=tk.BOTH, expand=False)

        self._pairing_status_var = tk.StringVar(value="正在查詢已配對過的裝置…")
        ttk.Label(self.content, textvariable=self._pairing_status_var, style="Muted.TLabel") \
            .pack(anchor=tk.W, pady=(10, 0))

        self._fallback_frame.pack(fill=tk.X, pady=(10, 0))

        self._bt_scan_devices.clear()
        self._bt.list_paired_devices(
            on_result=lambda devs: self._pending.append(("bt_paired_list", devs)),
            on_error=lambda msg: self._pending.append(("bt_scan_error", msg)),
        )

    def _render_manual_com_fallback(self) -> None:
        # ★冪等：先清空這個 frame 底下所有東西再重畫，不管被呼叫幾次畫面上永遠只有一份，
        # 修正先前「重複點按配對、找不到序列埠時清單越疊越多份」的問題。
        for child in self._fallback_frame.winfo_children():
            child.destroy()
        ttk.Label(self._fallback_frame, text="找不到對應的序列埠，請改用下方清單手動選擇：",
                  style="Muted.TLabel").pack(anchor=tk.W)
        row = ttk.Frame(self._fallback_frame)
        row.pack(fill=tk.X, pady=(4, 0))
        ttk.Label(row, text="序列埠：").pack(side=tk.LEFT)
        self._com_var = tk.StringVar()
        combo = ttk.Combobox(row, textvariable=self._com_var, width=18, state="readonly")
        combo["values"] = SerialWorker.list_ports()
        combo.pack(side=tk.LEFT, padx=(6, 6))
        ttk.Button(row, text="重新整理", command=lambda: combo.configure(values=SerialWorker.list_ports())) \
            .pack(side=tk.LEFT)
        ttk.Button(row, text="連線", style="Accent.TButton",
                   command=lambda: self._connect_to_port(self._com_var.get().strip())).pack(side=tk.LEFT, padx=(10, 0))

    def _clear_fallback(self) -> None:
        if hasattr(self, "_fallback_frame"):
            for child in self._fallback_frame.winfo_children():
                child.destroy()

    def _start_scan(self) -> None:
        self._pairing_status_var.set("掃描中…（找到裝置會即時出現在下方清單；"
                                      "若本機藍牙較不穩定，找不到裝置可以多試幾次）")
        self._bt.start_scan(
            on_device_found=lambda dev: self._pending.append(("bt_device", dev)),
            on_error=lambda msg: self._pending.append(("bt_scan_error", msg)),
        )

    def _apply_device_list(self, devices: list[BtDeviceInfo]) -> None:
        for dev in devices:
            self._add_device_row(dev)
        if not devices:
            self._pairing_status_var.set("目前沒有已配對過的裝置，請點「掃描裝置」尋找 micro generator。")
        else:
            self._pairing_status_var.set(f"已列出 {len(devices)} 個已配對裝置，可繼續掃描尋找新裝置。")

    def _add_device_row(self, dev: BtDeviceInfo) -> None:
        if dev.id in self._bt_scan_devices:
            return
        self._bt_scan_devices[dev.id] = dev
        row = ttk.Frame(self._device_list_frame)
        row.pack(fill=tk.X, pady=3)
        label = dev.name + ("　(已配對)" if dev.is_paired else "")
        ttk.Label(row, text=label).pack(side=tk.LEFT)
        ttk.Button(row, text="配對並連線", command=lambda d=dev: self._pair_device(d)) \
            .pack(side=tk.RIGHT)

    def _pair_device(self, dev: BtDeviceInfo) -> None:
        self._pairing_status_var.set(f"正在與「{dev.name}」配對…")
        self._clear_fallback()
        self._bt.stop_scan()

        def _after_bt_check(ready: bool, message: str) -> None:
            if not ready:
                self._pairing_status_var.set(message)
                return
            before_ports = SerialWorker.snapshot_ports()
            self._bt.pair(
                dev.id,
                on_done=lambda ok, msg, mac: self._pending.append(
                    ("bt_paired", (dev, ok, msg, mac or dev.mac, before_ports))
                ),
            )

        self._bt.check_bluetooth_ready(on_result=_after_bt_check)

    def _handle_bt_paired(self, dev: BtDeviceInfo, ok: bool, msg: str,
                           mac: Optional[str], before_ports: set[str]) -> None:
        if not ok:
            self._pairing_status_var.set(f"配對失敗：{msg}")
            return
        self.config_data["bluetooth"]["last_device_name"] = dev.name
        self.config_data["bluetooth"]["last_device_id"] = dev.id
        if mac:
            self.config_data["bluetooth"]["last_device_mac"] = mac
        save_config(self.config_data)

        # ★已配對裝置的序列埠通常早就存在(不是配對當下才建立)，優先直接用藍牙位址
        # 去比對現有序列埠，這一步是同步、瞬間完成的，不需要等待。
        if mac:
            port = SerialWorker.find_port_by_mac(mac)
            if port:
                self._pairing_status_var.set(f"找到序列埠 {port}，連線中…")
                self._connect_to_port(port)
                return

        # 找不到(通常代表這是「這台電腦第一次配對」的全新裝置，Windows 還在建立序列埠)：
        # 退回「比對配對前後多出來的埠」，這個方法對全新裝置才有意義，最多等 15 秒。
        self._pairing_status_var.set("配對成功，正在等待系統建立序列埠…")

        def _wait_port() -> None:
            port = SerialWorker.find_new_port(before_ports)
            self._pending.append(("bt_port_found", port))

        threading.Thread(target=_wait_port, daemon=True).start()

    def _handle_bt_port_found(self, port: Optional[str]) -> None:
        if not port:
            self._pairing_status_var.set("配對成功，但找不到對應的序列埠，請改用下方序列埠清單手動選擇。")
            self._render_manual_com_fallback()
            return
        self._pairing_status_var.set(f"找到序列埠 {port}，連線中…")
        self._connect_to_port(port)

    def _connect_to_port(self, port: str) -> None:
        if not port:
            messagebox.showwarning(self.APP_TITLE, "請先選擇序列埠")
            return
        self._is_reconnect_flow = False
        self._reconnect_active = False
        self._awaiting_live = True
        self._current_port = port
        self._serial.start(port, _BAUDRATE, _SERIAL_TIMEOUT)

    # ====================================================== Step 2：儲存位置
    def _go_folder_step(self) -> None:
        self._current_step = _STEP_FOLDER
        self._set_progress(1)
        self._clear_content()

        ttk.Label(self.content, text="選擇測試結果儲存位置", style="Title.TLabel").pack(anchor=tk.W)
        ttk.Label(
            self.content,
            text="測試完成後，PDF 報表預設會存到這個資料夾（之後仍可在儲存時更改）。",
            style="Muted.TLabel", wraplength=760, justify=tk.LEFT,
        ).pack(anchor=tk.W, pady=(12, 16))

        row = ttk.Frame(self.content)
        row.pack(fill=tk.X, pady=(0, 20))
        self._folder_var = tk.StringVar(value=self._save_dir)
        ttk.Entry(row, textvariable=self._folder_var, width=60, state="readonly").pack(side=tk.LEFT, padx=(0, 8))
        ttk.Button(row, text="瀏覽…", command=self._pick_folder).pack(side=tk.LEFT)

        ttk.Button(self.content, text="下一步", style="Accent.TButton", command=self._confirm_folder) \
            .pack(anchor=tk.W)

    def _pick_folder(self) -> None:
        chosen = filedialog.askdirectory(initialdir=self._save_dir, title="選擇儲存位置")
        if chosen:
            self._folder_var.set(chosen)

    def _confirm_folder(self) -> None:
        self._save_dir = self._folder_var.get().strip() or self._save_dir
        self.config_data["report"]["default_save_dir"] = self._save_dir
        save_config(self.config_data)
        self._go_connected_step()

    # ============================================== Step 3/4：已連線(依狀態顯示)
    def _go_connected_step(self) -> None:
        self._current_step = _STEP_CONNECTED
        self._render_connected_view()

    def _render_connected_view(self) -> None:
        live = self._latest_live
        want_subview = "wait" if (live is None or live.ui_state == UI_WAIT_START) else "live"

        if want_subview == "live" and self._connected_subview == "live" and self._live_phase_var is not None:
            if live is not None:
                self._update_live_status(live)
            return

        self._connected_subview = want_subview
        self._live_extra_mode = None
        self._save_done_shown = False
        self._clear_content()

        if want_subview == "wait":
            self._set_progress(2)
            self._render_place_and_start(live)
        else:
            self._set_progress(3)
            if live is not None:
                self._build_live_status_panel(live)

    def _build_live_status_panel(self, live: LiveSnapshot) -> None:
        """建立執行中畫面（只建一次，後續用 _update_live_status 更新）。"""
        ttk.Label(self.content, text="測試進行中", style="Title.TLabel").pack(anchor=tk.W)

        info = ttk.Frame(self.content)
        info.pack(fill=tk.X, pady=(16, 10))

        self._live_phase_var = tk.StringVar()
        self._live_rpm_var = tk.StringVar()
        self._live_elec_var = tk.StringVar()
        self._live_load_var = tk.StringVar()
        self._live_extra1_label_var = tk.StringVar()
        self._live_extra1_value_var = tk.StringVar()
        self._live_extra2_label_var = tk.StringVar()
        self._live_extra2_value_var = tk.StringVar()

        def row(label_var: tk.StringVar, value_var: tk.StringVar, r: int) -> None:
            ttk.Label(info, textvariable=label_var, style="Muted.TLabel").grid(
                row=r, column=0, sticky=tk.W, pady=2,
            )
            ttk.Label(info, textvariable=value_var, font=("Segoe UI", 11, "bold")).grid(
                row=r, column=1, sticky=tk.W, padx=(10, 0),
            )

        row(tk.StringVar(value="目前動作："), self._live_phase_var, 0)
        row(tk.StringVar(value="轉速："), self._live_rpm_var, 1)
        row(tk.StringVar(value="電壓／電流／功率："), self._live_elec_var, 2)
        row(tk.StringVar(value="負載開關："), self._live_load_var, 3)
        row(self._live_extra1_label_var, self._live_extra1_value_var, 4)
        row(self._live_extra2_label_var, self._live_extra2_value_var, 5)

        self._save_frame = ttk.Frame(self.content)
        self._save_frame.pack(fill=tk.X, pady=(20, 0))
        self._update_live_status(live)

    def _update_live_status(self, live: LiveSnapshot) -> None:
        """就地更新執行中畫面的數值，避免每次遙測都重建 widget 造成閃爍。"""
        if self._live_phase_var is None:
            return

        self._live_phase_var.set(MEAS_PHASE_LABELS_ZH.get(live.meas_phase, "未知"))
        self._live_rpm_var.set(f"{live.now_speed:.0f} / {live.keep_rpm:.0f} RPM")
        self._live_elec_var.set(
            f"{live.bus_v:.2f} V ／ {live.current_a:.3f} A ／ {live.power_w:.2f} W",
        )
        self._live_load_var.set("已接通" if live.load_connected else "斷開")

        extra_mode = live.meas_phase if live.meas_phase in (MEAS_SAFE_CURRENT, MEAS_RESISTANCE) else -1
        if extra_mode != self._live_extra_mode:
            self._live_extra_mode = extra_mode
            if live.meas_phase == MEAS_SAFE_CURRENT:
                self._live_extra1_label_var.set("安全電流本檔目標：")
                self._live_extra2_label_var.set("本檔熱穩下垂：")
            elif live.meas_phase == MEAS_RESISTANCE:
                self._live_extra1_label_var.set("內阻本點：")
                self._live_extra2_label_var.set("")
            else:
                self._live_extra1_label_var.set("")
                self._live_extra2_label_var.set("")

        if live.meas_phase == MEAS_SAFE_CURRENT:
            self._live_extra1_value_var.set(f"{live.safe_target_rpm:.0f} RPM")
            self._live_extra2_value_var.set(f"{live.safe_droop_ratio * 100:.1f}%")
        elif live.meas_phase == MEAS_RESISTANCE:
            self._live_extra1_value_var.set(
                f"第 {live.res_point_index + 1} / 3 點，目標 {live.res_target_rpm:.0f} RPM",
            )
            self._live_extra2_value_var.set("")
        else:
            self._live_extra1_value_var.set("")
            self._live_extra2_value_var.set("")

        if self._save_frame is None:
            return
        if live.meas_phase == MEAS_DONE and self._latest_curve is not None:
            if not self._save_done_shown:
                self._save_done_shown = True
                self._render_save_controls()
        elif self._save_done_shown:
            self._save_done_shown = False
            for child in self._save_frame.winfo_children():
                child.destroy()
            ttk.Label(
                self._save_frame,
                text="測試完成後，這裡會出現儲存報表的按鈕。",
                style="Muted.TLabel",
            ).pack(anchor=tk.W)

    def _render_place_and_start(self, live: Optional[LiveSnapshot]) -> None:
        ttk.Label(self.content, text="放置發電機並按下 START", style="Title.TLabel").pack(anchor=tk.W)
        ttk.Label(
            self.content,
            text="請把待測發電機安裝好、接線確認無誤後，按下機台上的 START 鈕開始測試。"
                 "按下後這裡會自動切換到即時狀態畫面。",
            style="Muted.TLabel", wraplength=760, justify=tk.LEFT,
        ).pack(anchor=tk.W, pady=(12, 20))
        state = "已連線，等待按下 START" if live is not None else "連線中…"
        ttk.Label(self.content, text=f"目前狀態：{state}").pack(anchor=tk.W)

    def _render_save_controls(self) -> None:
        for child in self._save_frame.winfo_children():
            child.destroy()
        if self._pending_entry is None:
            self._pending_entry = self._history.add(ReportData(
                generated_at=datetime.now(), last_live=self._latest_live, curve=self._latest_curve,
            ))

        ttk.Label(self._save_frame, text="測試完成，可以取下發電機。", font=("Segoe UI", 11, "bold")) \
            .pack(anchor=tk.W, pady=(0, 10))
        row = ttk.Frame(self._save_frame)
        row.pack(fill=tk.X)
        ttk.Button(row, text="儲存報表 (PDF)", style="Accent.TButton", command=self._save_report) \
            .pack(side=tk.LEFT)
        ttk.Button(row, text="更改儲存位置", command=self._change_save_location_inline).pack(side=tk.LEFT, padx=(10, 0))
        self._save_result_var = tk.StringVar(value=f"目前儲存位置：{self._save_dir}")
        ttk.Label(self._save_frame, textvariable=self._save_result_var, style="Muted.TLabel") \
            .pack(anchor=tk.W, pady=(10, 0))

    def _change_save_location_inline(self) -> None:
        chosen = filedialog.askdirectory(initialdir=self._save_dir, title="選擇儲存位置")
        if chosen:
            self._save_dir = chosen
            self.config_data["report"]["default_save_dir"] = chosen
            save_config(self.config_data)
            self._save_result_var.set(f"目前儲存位置：{self._save_dir}")

    def _save_report(self) -> None:
        if self._pending_entry is None:
            return
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        default_name = f"micro_generator_report_{stamp}.pdf"
        path_str = filedialog.asksaveasfilename(
            initialdir=self._save_dir, initialfile=default_name,
            defaultextension=".pdf", filetypes=[("PDF", "*.pdf")],
            title="儲存測試報表",
        )
        if not path_str:
            return
        try:
            generate_report_pdf(Path(path_str), self._pending_entry.data)
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(self.APP_TITLE, f"儲存 PDF 失敗：{exc}")
            return
        self._history.mark_saved(self._pending_entry, path_str)
        self._save_result_var.set(f"已儲存：{path_str}")

    # ============================================================== 歷史紀錄
    # ★刻意不寫入磁碟：只保留「這次開啟程式期間」跑過的測試，關閉程式就消失。
    # 已經另外按過「儲存報表」的項目，這裡只是記著它存在哪裡，檔案本身才是真正的紀錄。
    def _show_history_window(self) -> None:
        win = tk.Toplevel(self)
        win.title("歷史紀錄(本次執行期間)")
        win.configure(bg=_WHITE)
        win.geometry("520x360")

        entries = self._history.entries
        if not entries:
            ttk.Label(win, text="本次開啟程式後還沒有完成任何測試。", padding=20).pack()
            return

        container = ttk.Frame(win, padding=12)
        container.pack(fill=tk.BOTH, expand=True)
        for i, entry in enumerate(reversed(entries), start=1):
            row = ttk.Frame(container, padding=(0, 6))
            row.pack(fill=tk.X)
            stamp = entry.completed_at.strftime("%H:%M:%S")
            status = entry.saved_path if entry.saved_path else "尚未儲存"
            ttk.Label(row, text=f"{i}. {stamp}　{status}", wraplength=460, justify=tk.LEFT).pack(side=tk.LEFT)
            if not entry.saved_path:
                ttk.Button(row, text="儲存", command=lambda e=entry: self._save_history_entry(e, win)) \
                    .pack(side=tk.RIGHT)

    def _save_history_entry(self, entry: HistoryEntry, host_window: tk.Toplevel) -> None:
        stamp = entry.completed_at.strftime("%Y%m%d_%H%M%S")
        path_str = filedialog.asksaveasfilename(
            initialdir=self._save_dir, initialfile=f"micro_generator_report_{stamp}.pdf",
            defaultextension=".pdf", filetypes=[("PDF", "*.pdf")],
            title="儲存測試報表",
        )
        if not path_str:
            return
        try:
            generate_report_pdf(Path(path_str), entry.data)
        except Exception as exc:  # noqa: BLE001
            messagebox.showerror(self.APP_TITLE, f"儲存 PDF 失敗：{exc}")
            return
        self._history.mark_saved(entry, path_str)
        host_window.destroy()
        self._show_history_window()

    # ==================================================== 更改配對機器(任何步驟)
    def _on_change_device(self) -> None:
        self._cancel_auto_reconnect_timer()
        self._reconnect_active = False
        self._awaiting_live = False
        self._bt.stop_scan()
        if self._serial.is_running:
            self._serial.stop()
        self._latest_live = None
        self._latest_curve = None
        self._pending_entry = None
        self._close_fault_popup()
        self._go_pairing_step(auto=False)

    # ============================================================ 異常彈窗
    def _update_fault_popup(self, live: LiveSnapshot) -> None:
        if live.fault:
            reason = f"設備發生故障：{_FAULT_CODE_ZH.get(live.fault_code, live.fault_name)}\n" \
                     "需要重新開機才能恢復。"
        elif live.link_lost:
            reason = "偵測到發電機斷線（可能是量測線鬆脫）。\n" \
                     "請確認接線後，在機台上重新按下 START 即可繼續目前的測試。"
        else:
            reason = None

        if reason is None:
            self._close_fault_popup()
            return

        if self._fault_popup is None:
            self._open_fault_popup(reason)
        elif reason != self._fault_popup_reason:
            self._fault_popup_reason = reason
            self._fault_popup_label.configure(text=reason)

    def _open_fault_popup(self, reason: str) -> None:
        self._fault_popup_reason = reason
        popup = tk.Toplevel(self)
        popup.title("異常通知")
        popup.configure(bg=_WHITE)
        popup.resizable(False, False)
        popup.protocol("WM_DELETE_WINDOW", lambda: None)  # 沒有關閉按鈕，只能等狀況排除自動消失
        popup.attributes("-topmost", True)
        frame = ttk.Frame(popup, padding=20)
        frame.pack()
        self._fault_popup_label = ttk.Label(frame, text=reason, wraplength=360, justify=tk.LEFT)
        self._fault_popup_label.pack()
        ttk.Label(frame, text="（狀況排除後這個視窗會自動消失）", style="Muted.TLabel") \
            .pack(pady=(12, 0))
        self._fault_popup = popup

    def _close_fault_popup(self) -> None:
        if self._fault_popup is not None:
            self._fault_popup.destroy()
            self._fault_popup = None
            self._fault_popup_reason = None

    # ===================================================== 背景執行緒 callback
    # 這幾個函式在 SerialWorker/BtPairingWorker 的背景執行緒被呼叫，Tkinter 不是
    # 執行緒安全的，所以一律只把資料丟進 deque，實際處理都在 _poll()(主執行緒)進行。
    def _on_live_bg(self, live: LiveSnapshot) -> None:
        self._pending.append(("live", live))

    def _on_curve_bg(self, curve: CurveTable) -> None:
        self._pending.append(("curve", curve))

    def _on_status_bg(self, msg: str) -> None:
        self._pending.append(("status", msg))

    def _on_error_bg(self, msg: str) -> None:
        self._pending.append(("error", msg))

    # ==================================================================== poll
    def _poll(self) -> None:
        while self._pending:
            kind, payload = self._pending.popleft()
            if kind == "live":
                self._apply_live(payload)  # type: ignore[arg-type]
            elif kind == "curve":
                self._latest_curve = payload  # type: ignore[assignment]
            elif kind == "status":
                self.status_var.set(str(payload))
            elif kind == "error":
                self._handle_serial_error(str(payload))
            elif kind == "bt_ready":
                ready, message, port = payload  # type: ignore[misc]
                if ready:
                    port_now = self._resolve_reconnect_port() or port
                    self._continue_auto_reconnect(port_now)
                else:
                    self.status_var.set(message)
                    if hasattr(self, "_auto_reconnect_status_var"):
                        self._auto_reconnect_status_var.set(message)
                    self._auto_reconnect_after_id = self.after(
                        3000,
                        lambda p=port: self._begin_auto_reconnect(),
                    )
            elif kind == "bt_device":
                if self._current_step == _STEP_PAIRING and hasattr(self, "_device_list_frame"):
                    self._add_device_row(payload)  # type: ignore[arg-type]
            elif kind == "bt_paired_list":
                if self._current_step == _STEP_PAIRING and hasattr(self, "_device_list_frame"):
                    self._apply_device_list(payload)  # type: ignore[arg-type]
            elif kind == "bt_scan_error":
                if hasattr(self, "_pairing_status_var"):
                    self._pairing_status_var.set(str(payload))
            elif kind == "bt_paired":
                dev, ok, msg, mac, before_ports = payload  # type: ignore[misc]
                self._handle_bt_paired(dev, ok, msg, mac, before_ports)
            elif kind == "bt_port_found":
                self._handle_bt_port_found(payload)  # type: ignore[arg-type]

        self.after(200, self._poll)

    def _handle_serial_error(self, message: str) -> None:
        if message.startswith("PORT_BUSY:"):
            # 重連期間 COM 埠仍被 Windows/其他程式佔用：不要整個退回第一步再立刻重開，
            # 否則會形成 PermissionError 連環；只在自動重連流程內退避重試。
            parts = message.split(":", 2)
            port = parts[1] if len(parts) > 1 else (self._resolve_reconnect_port() or "")
            self.status_var.set(f"序列埠 {port or '?'} 暫時無法開啟（可能被系統佔用），稍後重試…")
            if self._reconnect_active and port:
                if self._serial.is_running:
                    self._serial.stop()
                self._awaiting_live = True
                self._schedule_auto_reconnect_retry(port, port_busy=True)
            elif self._current_step == _STEP_PAIRING and hasattr(self, "_pairing_status_var"):
                self._pairing_status_var.set(
                    f"無法開啟 {port or '序列埠'}（存取被拒）。請確認沒有其他程式佔用此 COM 埠，"
                    f"或改用下方清單手動選擇。"
                )
                self._render_manual_com_fallback()
            else:
                self.status_var.set(message)
            return

        if message.startswith("DISCONNECTED:"):
            detail = message.split(":", 1)[1]
            self.status_var.set(detail)
            self._handle_disconnect()
            return

        self.status_var.set(message)
        if self._reconnect_active:
            port = self._resolve_reconnect_port()
            if port:
                self._schedule_auto_reconnect_retry(port)
            return
        if self._awaiting_live and self._current_step == _STEP_PAIRING:
            if hasattr(self, "_pairing_status_var"):
                self._pairing_status_var.set(message)
            self._render_manual_com_fallback()
            return
        self._handle_disconnect()

    def _apply_live(self, live: LiveSnapshot) -> None:
        first_live_this_connection = self._latest_live is None
        self._latest_live = live

        if first_live_this_connection:
            self._cancel_auto_reconnect_timer()
            self._reconnect_active = False
            self._awaiting_live = False
            self._on_connected(is_reconnect=self._is_reconnect_flow)
            return  # _on_connected 已經 render 過一次

        self._update_fault_popup(live)

        if live.meas_phase != self._last_meas_phase_seen:
            self._last_meas_phase_seen = live.meas_phase
            if live.meas_phase != MEAS_DONE:
                self._pending_entry = None  # 開始新一輪量測(例如斷線續測後)，清掉舊的待存結果

        if self._current_step == _STEP_CONNECTED:
            self._render_connected_view()

    def _on_connected(self, *, is_reconnect: bool) -> None:
        if self._current_port:
            self.config_data["bluetooth"]["last_com_port"] = self._current_port
            save_config(self.config_data)
        self.status_var.set("已連線")

        # self._latest_live 在呼叫本函式之前就已經被 _apply_live() 設成剛收到的第一筆資料，
        # 這裡先用它更新彈窗/階段追蹤，下面的 _go_connected_step()/_go_folder_step()
        # 才會用到最新狀態渲染，避免多渲染一次。
        if self._latest_live is not None:
            self._update_fault_popup(self._latest_live)
            self._last_meas_phase_seen = self._latest_live.meas_phase

        if is_reconnect:
            self._go_connected_step()
        else:
            self._go_folder_step()

    def _handle_disconnect(self) -> None:
        """BT 斷線(例如機台按了 reset)：彈窗消失，回到第一步嘗試自動重連上次的裝置。"""
        if self._serial.is_running:
            self._serial.stop()
        self._close_fault_popup()
        self._latest_live = None
        self._latest_curve = None
        self._connected_subview = None
        self._live_phase_var = None
        self._go_pairing_step(auto=True)

    # ==================================================================== 關閉
    def _on_close(self) -> None:
        self._bt.stop()
        if self._serial.is_running:
            self._serial.stop()
        self.destroy()


def run_app() -> None:
    app = MainWindow()
    app.mainloop()
