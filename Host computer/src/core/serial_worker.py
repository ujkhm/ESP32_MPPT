"""Background serial reader for ESP32 MPPT telemetry."""

from __future__ import annotations

import threading
from dataclasses import dataclass
from datetime import datetime
from typing import Callable

import serial
from serial.tools import list_ports


@dataclass
class SpeedSample:
    timestamp: datetime
    rpm: float


class SerialWorker:
    def __init__(
        self,
        on_sample: Callable[[SpeedSample], None],
        on_status: Callable[[str], None],
        on_error: Callable[[str], None],
    ) -> None:
        self._on_sample = on_sample
        self._on_status = on_status
        self._on_error = on_error
        self._stop_event = threading.Event()
        self._thread: threading.Thread | None = None
        self._serial: serial.Serial | None = None

    @staticmethod
    def list_ports() -> list[str]:
        return [port.device for port in list_ports.comports()]

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
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._thread = None
        if self._serial and self._serial.is_open:
            self._serial.close()
        self._serial = None

    def _run(self, port: str, baudrate: int, timeout: float) -> None:
        try:
            self._serial = serial.Serial(port, baudrate, timeout=timeout)
            self._on_status(f"已連線 {port} @ {baudrate}")
        except serial.SerialException as exc:
            self._on_error(f"無法開啟序列埠: {exc}")
            return

        while not self._stop_event.is_set():
            try:
                assert self._serial is not None
                raw = self._serial.readline()
            except serial.SerialException as exc:
                self._on_error(f"序列埠讀取錯誤: {exc}")
                break

            if not raw:
                continue

            line = raw.decode("utf-8", errors="ignore").strip()
            if not line:
                continue

            try:
                rpm = float(line)
            except ValueError:
                continue

            self._on_sample(SpeedSample(timestamp=datetime.now(), rpm=rpm))

        if self._serial and self._serial.is_open:
            self._serial.close()
        self._serial = None
        self._on_status("已中斷連線")
