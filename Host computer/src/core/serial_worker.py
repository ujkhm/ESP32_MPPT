"""Background reader for the ESP32's Bluetooth SPP telemetry stream.

Once paired, Windows exposes the ESP32's BluetoothSerial link as an ordinary
virtual COM port, so this reuses plain pyserial exactly like the original
USB-serial version did -- only the payload format changed (JSON lines, see
core.telemetry), not the transport.
"""

from __future__ import annotations

import threading
import time
from typing import Callable, Optional

import serial
from serial.tools import list_ports

from core.telemetry import CurveTable, LiveSnapshot, parse_telemetry_line

# Windows 藍牙 SPP 虛擬序列埠在斷線/重開機後，作業系統常需要一段時間才會釋放 handle；
# 若立刻重開同一個 COM 埠，pyserial 會丟 PermissionError(13, '存取被拒。')。
_PORT_RELEASE_POLL_S = 0.25
_PORT_RELEASE_TIMEOUT_S = 6.0

# 藍牙 SPP 虛擬序列埠斷線(尤其是對方裝置重新開機)時，Windows 不一定會讓 pyserial 的
# readline() 直接丟例外——很多情況下埠本身仍然「看起來開著」，只是永遠讀不到新資料。
# 因為這裡的通訊完全是單向(只收、不送)，光靠例外偵測斷線並不夠，必須額外用「太久沒收到
# 任何一行資料」當作斷線的第二種判斷依據，才能讓上位機在下位機重開機後真正發現斷線、
# 進而觸發 core.main_window 既有的自動重連流程。
_SILENCE_TIMEOUT_S = 8.0


class SerialWorker:
    def __init__(
        self,
        on_live: Callable[[LiveSnapshot], None],
        on_curve: Callable[[CurveTable], None],
        on_status: Callable[[str], None],
        on_error: Callable[[str], None],
    ) -> None:
        self._on_live = on_live
        self._on_curve = on_curve
        self._on_status = on_status
        self._on_error = on_error
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._serial: Optional[serial.Serial] = None

    @staticmethod
    def list_ports() -> list[str]:
        return [port.device for port in list_ports.comports()]

    @staticmethod
    def snapshot_ports() -> set[str]:
        """Capture the current COM port set, to diff against after triggering a
        fresh Bluetooth pairing (Windows creates the virtual COM port a few
        seconds *after* pairing succeeds, not instantly)."""
        return {p.device for p in list_ports.comports()}

    @staticmethod
    def find_new_port(before: set[str], timeout_s: float = 15.0, poll_s: float = 1.0) -> Optional[str]:
        """Poll until a COM port not present in `before` shows up, or timeout.
        Only useful for a *freshly* paired device -- see find_port_by_mac() for
        the (much more common) already-paired case, where the port already
        existed before pairing was even triggered so there is nothing "new"."""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            now = {p.device for p in list_ports.comports()}
            new_ports = sorted(now - before)
            if new_ports:
                return new_ports[0]
            time.sleep(poll_s)
        return None

    @staticmethod
    def is_permission_denied(exc: BaseException) -> bool:
        text = str(exc).lower()
        return "permissionerror" in text or "存取被拒" in text or "access is denied" in text

    @staticmethod
    def wait_for_port_release(port: str, timeout_s: float = _PORT_RELEASE_TIMEOUT_S) -> bool:
        """輪詢直到指定 COM 埠不再被本程序佔用，或逾時。"""
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            try:
                probe = serial.Serial(port, timeout=0)
                probe.close()
                return True
            except serial.SerialException:
                time.sleep(_PORT_RELEASE_POLL_S)
        return False

    @staticmethod
    def find_port_by_mac(mac12: str) -> Optional[str]:
        """在目前序列埠的 hwid 裡找含有這個藍牙位址(12 碼十六進位，不分大小寫/分隔符)的
        那一個。藍牙 SPP 虛擬序列埠的 hwid 常見格式類似
        'BTHENUM\\{...}\\B&xxxxxxx&0&<12碼位址>_Cxxxxxxxx'，實測直接比對這個位址
        對「已經配對過」的裝置非常可靠、且不需要等待(對照組：find_new_port() 只對
        「這台電腦從沒配對過」的全新裝置才有意義，因為已配對裝置的序列埠早就存在，
        不會有「配對前後多出來的埠」可以比對)。"""
        target = "".join(ch for ch in mac12.upper() if ch in "0123456789ABCDEF")
        if len(target) != 12:
            return None
        for p in list_ports.comports():
            hwid_compact = "".join(ch for ch in (p.hwid or "").upper() if ch in "0123456789ABCDEF")
            if target in hwid_compact:
                return p.device
        return None

    @property
    def is_running(self) -> bool:
        return self._thread is not None and self._thread.is_alive()

    def start(self, port: str, baudrate: int, timeout: float) -> None:
        if self.is_running:
            self.stop()

        self._stop_event.clear()
        self._thread = threading.Thread(
            target=self._run,
            args=(port, baudrate, timeout),
            daemon=True,
            name="SerialWorker",
        )
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        # 先關閉序列埠再 join，讓 Windows 盡快釋放 COM 埠，降低重連時「存取被拒」機率
        ser = self._serial
        if ser is not None:
            try:
                if ser.is_open:
                    ser.close()
            except serial.SerialException:
                pass
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=3.0)
        self._thread = None
        self._serial = None

    def _run(self, port: str, baudrate: int, timeout: float) -> None:
        opened = False
        try:
            self._serial = serial.Serial(port, baudrate, timeout=timeout)
            opened = True
            self._on_status(f"已連線 {port} @ {baudrate}")
        except serial.SerialException as exc:
            if self.is_permission_denied(exc):
                self._on_error(f"PORT_BUSY:{port}:{exc}")
            else:
                self._on_error(f"無法開啟序列埠: {exc}")
            return

        last_data_at = time.monotonic()
        try:
            while not self._stop_event.is_set():
                try:
                    assert self._serial is not None
                    raw = self._serial.readline()
                except serial.SerialException as exc:
                    self._on_error(f"序列埠讀取錯誤: {exc}")
                    break

                if not raw:
                    if (time.monotonic() - last_data_at) > _SILENCE_TIMEOUT_S:
                        self._on_error("DISCONNECTED:序列埠長時間沒有收到資料(可能是藍牙斷線或裝置重新開機)")
                        break
                    continue

                last_data_at = time.monotonic()
                line = raw.decode("utf-8", errors="ignore")
                live, curve = parse_telemetry_line(line)
                if live is not None:
                    self._on_live(live)
                elif curve is not None:
                    self._on_curve(curve)
        finally:
            if self._serial is not None and self._serial.is_open:
                try:
                    self._serial.close()
                except serial.SerialException:
                    pass
            self._serial = None
            if opened:
                self._on_status("已中斷連線")
