"""Capture ESP32 serial output from COM4 with timestamps."""

from __future__ import annotations

import sys
import time
from datetime import datetime
from pathlib import Path

import serial

PORT = "COM4"
BAUD = 115200
OUT = Path(__file__).resolve().parents[1] / "com4_capture.log"


def main() -> int:
    out_path = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with serial.Serial(PORT, BAUD, timeout=1.0) as ser, out_path.open("a", encoding="utf-8") as f:
        f.write(f"\n===== MONITOR START {datetime.now().isoformat()} =====\n")
        f.flush()
        print(f"Monitoring {PORT} -> {out_path}", flush=True)
        while True:
            try:
                raw = ser.readline()
            except serial.SerialException as exc:
                line = f"[MONITOR ERROR] {exc}\n"
                f.write(line)
                f.flush()
                print(line, end="", flush=True)
                time.sleep(1.0)
                continue
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not text:
                continue
            stamp = datetime.now().strftime("%H:%M:%S")
            row = f"[{stamp}] {text}\n"
            f.write(row)
            f.flush()
            try:
                print(row, end="", flush=True)
            except UnicodeEncodeError:
                # Windows 主控台預設 cp950，略過無法顯示的字元但仍寫入 log
                enc = getattr(sys.stdout, "encoding", None) or "utf-8"
                print(row.encode(enc, errors="replace").decode(enc), end="", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
