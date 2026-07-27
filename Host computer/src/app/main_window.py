"""Main window for ESP32 MPPT host application."""

from __future__ import annotations

import csv
import tkinter as tk
from collections import deque
from datetime import datetime
from tkinter import messagebox, ttk

from matplotlib.backends.backend_tkagg import FigureCanvasTkAgg
from matplotlib.figure import Figure

from core.config import load_config, save_config
from core.paths import get_logs_dir
from core.serial_worker import SerialWorker, SpeedSample


class MainWindow(tk.Tk):
    APP_TITLE = "ESP32 MPPT 上位機"

    def __init__(self) -> None:
        super().__init__()
        self.config_data = load_config()
        self.max_points = int(self.config_data["ui"]["chart_max_points"])

        self.title(self.APP_TITLE)
        self.geometry("960x640")
        self.minsize(820, 520)

        self._samples: deque[SpeedSample] = deque(maxlen=self.max_points)
        self._csv_file = None
        self._csv_writer = None
        self._pending_ui: deque[tuple[str, object]] = deque()

        self._worker = SerialWorker(
            on_sample=self._handle_sample,
            on_status=lambda msg: self._queue_ui("status", msg),
            on_error=lambda msg: self._queue_ui("error", msg),
        )

        self._build_ui()
        self._refresh_ports()
        self.after(int(self.config_data["ui"]["refresh_ms"]), self._poll_ui)

    def _build_ui(self) -> None:
        top = ttk.Frame(self, padding=10)
        top.pack(fill=tk.X)

        ttk.Label(top, text="序列埠:").grid(row=0, column=0, sticky=tk.W, padx=(0, 6))
        self.port_var = tk.StringVar(value=self.config_data["serial"]["port"])
        self.port_combo = ttk.Combobox(top, textvariable=self.port_var, width=18, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky=tk.W)

        ttk.Button(top, text="重新整理", command=self._refresh_ports).grid(row=0, column=2, padx=6)

        ttk.Label(top, text="鮑率:").grid(row=0, column=3, sticky=tk.W, padx=(12, 6))
        self.baud_var = tk.StringVar(value=str(self.config_data["serial"]["baudrate"]))
        ttk.Entry(top, textvariable=self.baud_var, width=10).grid(row=0, column=4, sticky=tk.W)

        self.connect_btn = ttk.Button(top, text="連線", command=self._toggle_connection)
        self.connect_btn.grid(row=0, column=5, padx=(12, 0))

        status_frame = ttk.Frame(self, padding=(10, 0, 10, 6))
        status_frame.pack(fill=tk.X)
        self.status_var = tk.StringVar(value="就緒")
        ttk.Label(status_frame, textvariable=self.status_var).pack(anchor=tk.W)

        metric_frame = ttk.LabelFrame(self, text="即時數據", padding=10)
        metric_frame.pack(fill=tk.X, padx=10, pady=(0, 8))

        self.rpm_var = tk.StringVar(value="--")
        ttk.Label(metric_frame, text="轉速 (RPM):", font=("Segoe UI", 12)).grid(row=0, column=0, sticky=tk.W)
        ttk.Label(metric_frame, textvariable=self.rpm_var, font=("Segoe UI", 20, "bold")).grid(
            row=0, column=1, sticky=tk.W, padx=(8, 0)
        )

        chart_frame = ttk.LabelFrame(self, text="轉速曲線", padding=6)
        chart_frame.pack(fill=tk.BOTH, expand=True, padx=10, pady=(0, 10))

        self.figure = Figure(figsize=(8, 4), dpi=100)
        self.ax = self.figure.add_subplot(111)
        self.ax.set_xlabel("樣本序號")
        self.ax.set_ylabel("RPM")
        self.ax.grid(True, alpha=0.3)
        (self._line,) = self.ax.plot([], [], color="#1f77b4")

        self.canvas = FigureCanvasTkAgg(self.figure, master=chart_frame)
        self.canvas.get_tk_widget().pack(fill=tk.BOTH, expand=True)

        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _refresh_ports(self) -> None:
        ports = SerialWorker.list_ports()
        self.port_combo["values"] = ports
        if ports:
            current = self.port_var.get()
            if current not in ports:
                self.port_var.set(ports[0])
        else:
            self.port_var.set("")

    def _toggle_connection(self) -> None:
        if self._worker.is_running:
            self._stop_connection()
            return

        port = self.port_var.get().strip()
        if not port:
            messagebox.showwarning(self.APP_TITLE, "請先選擇序列埠")
            return

        try:
            baudrate = int(self.baud_var.get().strip())
        except ValueError:
            messagebox.showerror(self.APP_TITLE, "鮑率必須為整數")
            return

        timeout = float(self.config_data["serial"]["timeout"])
        self.config_data["serial"]["port"] = port
        self.config_data["serial"]["baudrate"] = baudrate
        save_config(self.config_data)

        if self.config_data["logging"]["auto_save_csv"]:
            self._open_csv_log()

        self._worker.start(port, baudrate, timeout)
        self.connect_btn.configure(text="中斷")
        self.port_combo.configure(state="disabled")

    def _stop_connection(self) -> None:
        self._worker.stop()
        self.connect_btn.configure(text="連線")
        self.port_combo.configure(state="readonly")
        self._close_csv_log()

    def _open_csv_log(self) -> None:
        prefix = self.config_data["logging"]["csv_prefix"]
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = get_logs_dir() / f"{prefix}_{stamp}.csv"
        self._csv_file = path.open("w", newline="", encoding="utf-8-sig")
        self._csv_writer = csv.writer(self._csv_file)
        self._csv_writer.writerow(["timestamp", "rpm"])

    def _close_csv_log(self) -> None:
        if self._csv_file:
            self._csv_file.close()
        self._csv_file = None
        self._csv_writer = None

    def _handle_sample(self, sample: SpeedSample) -> None:
        self._queue_ui("sample", sample)

    def _queue_ui(self, kind: str, payload: object) -> None:
        self._pending_ui.append((kind, payload))

    def _poll_ui(self) -> None:
        while self._pending_ui:
            kind, payload = self._pending_ui.popleft()
            if kind == "sample":
                assert isinstance(payload, SpeedSample)
                self._apply_sample(payload)
            elif kind == "status":
                self.status_var.set(str(payload))
            elif kind == "error":
                self.status_var.set(str(payload))
                messagebox.showerror(self.APP_TITLE, str(payload))
                self._stop_connection()

        self.after(int(self.config_data["ui"]["refresh_ms"]), self._poll_ui)

    def _apply_sample(self, sample: SpeedSample) -> None:
        self._samples.append(sample)
        self.rpm_var.set(f"{sample.rpm:.2f}")

        if self._csv_writer:
            self._csv_writer.writerow([sample.timestamp.isoformat(timespec="milliseconds"), f"{sample.rpm:.4f}"])
            self._csv_file.flush()

        xs = list(range(len(self._samples)))
        ys = [item.rpm for item in self._samples]
        self._line.set_data(xs, ys)
        if ys:
            self.ax.set_xlim(max(0, len(ys) - self.max_points), max(len(ys), 1))
            ymin = min(ys)
            ymax = max(ys)
            if ymin == ymax:
                ymin -= 1
                ymax += 1
            self.ax.set_ylim(ymin, ymax)
        self.canvas.draw_idle()

    def _on_close(self) -> None:
        self._stop_connection()
        self.destroy()


def run_app() -> None:
    app = MainWindow()
    app.mainloop()
