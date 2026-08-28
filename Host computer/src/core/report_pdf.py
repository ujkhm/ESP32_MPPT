"""Generate the 3-page PDF test report.

Page 1: test parameters + result extremes + recommended load resistance.
Page 2: voltage at max power vs RPM (line chart, 100 RPM steps).
Page 3: power vs RPM (line chart, 100 RPM steps).
"""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Optional

import matplotlib
from matplotlib.backends.backend_pdf import PdfPages
from matplotlib.figure import Figure

from core.telemetry import CurveTable, LiveSnapshot

# ★matplotlib 預設字型(DejaVu Sans)不含中文字形，不設定的話整份 PDF 的中文字
# 會變成空白方塊(tofu)。「微軟正黑體」是 Windows 內建的繁體中文字型，這裡直接
# 指定成 sans-serif 字型清單的第一順位；找不到就退回其他清單項目，不會整個報錯。
matplotlib.rcParams["font.sans-serif"] = ["Microsoft JhengHei", "Microsoft YaHei", "SimHei", "DejaVu Sans"]
matplotlib.rcParams["font.family"] = "sans-serif"
matplotlib.rcParams["axes.unicode_minus"] = False

CURVE_LIMIT_REASON_TEXT = {
    0: "無(資料不足)",
    1: "固定測試電阻電流封頂",
    2: "最大功率點合法上限",
    3: "開路耐壓上限",
    4: "人為天花板",
}

_PAGE_SIZE = (8.27, 11.69)  # A4 直式(英吋)


@dataclass
class ReportData:
    """Everything the report needs, snapshotted at the moment the user hits Save."""

    generated_at: datetime
    last_live: Optional[LiveSnapshot]
    curve: Optional[CurveTable]


def _summary_lines(data: ReportData) -> list[str]:
    live = data.last_live
    curve = data.curve
    lines: list[str] = []

    lines.append(f"報表產生時間：{data.generated_at.strftime('%Y-%m-%d %H:%M:%S')}")
    lines.append("")
    lines.append("── 測試參數 ──")
    if live is not None:
        lines.append(f"安全連續電流 I_cont：{live.safe_i_cont_a:.3f} A"
                      f"　(量測轉速 {live.safe_i_cont_rpm:.0f} RPM)")
        lines.append(f"發電機內阻 R_th：{live.res_rth_ohm:.4f} Ω")
        lines.append(f"感應電勢常數 k_e：{live.res_ke_v_per_rpm:.6f} V/RPM")
        reason = CURVE_LIMIT_REASON_TEXT.get(live.curve_limit_reason, "未知")
        lines.append(f"曲線／極限轉速上限 n_lim：{live.curve_n_lim:.0f} RPM　(限制原因：{reason})")
        lines.append(f"　├ 固定測試電阻功率封頂 n_rl：{live.curve_n_rl:.0f} RPM")
        lines.append(f"　├ 最大功率點合法上限 n_knee：{live.curve_n_knee:.0f} RPM")
        lines.append(f"　└ 開路耐壓上限 n_voc：{live.curve_n_voc:.0f} RPM")
    else:
        lines.append("(尚未收到量測結果)")

    lines.append("")
    lines.append("── 結果極值與建議 ──")
    if curve is not None and curve.points:
        best = max(curve.points, key=lambda p: p.p_max)
        lines.append(f"曲線涵蓋轉速範圍：{curve.points[0].rpm:.0f} ～ {curve.points[-1].rpm:.0f} RPM"
                      f"　(共 {len(curve.points)} 點，每 100 RPM 一點)")
        lines.append(f"涵蓋範圍內最大功率：{best.p_max:.3f} W　於 {best.rpm:.0f} RPM"
                      f"　(此時電壓 {best.v_at_max_p:.2f} V，負載電阻 {best.r_opt:.3f} Ω)")
        lines.append("")
        if live is not None:
            lines.append(f"建議負載電阻：約 {live.res_rth_ohm:.2f} Ω")
        lines.append("(在 n_knee 以下，最大功率點負載電阻等於發電機內阻 R_th；")
        lines.append(" 超過 n_knee 之後，可用負載電阻會隨轉速升高而變小，見第 2、3 頁曲線)")
    else:
        lines.append("(尚未收到曲線資料)")

    return lines


def _build_summary_page(fig: Figure, data: ReportData) -> None:
    fig.clf()
    ax = fig.add_subplot(111)
    ax.axis("off")
    fig.suptitle("微型發電機測試報告", fontsize=18, fontweight="bold", y=0.97)
    text = "\n".join(_summary_lines(data))
    ax.text(0.05, 0.90, text, fontsize=11, va="top", ha="left",
            transform=ax.transAxes, wrap=True, linespacing=1.6)


def _build_line_chart_page(fig: Figure, curve: Optional[CurveTable], *, y_attr: str,
                            title: str, ylabel: str) -> None:
    fig.clf()
    ax = fig.add_subplot(111)
    ax.set_title(title, fontsize=14)
    ax.set_xlabel("轉速 (RPM)")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.3)

    if curve is None or not curve.points:
        ax.text(0.5, 0.5, "(尚未收到曲線資料)", ha="center", va="center", transform=ax.transAxes)
        return

    xs = [p.rpm for p in curve.points]
    ys = [getattr(p, y_attr) for p in curve.points]
    ax.plot(xs, ys, color="#1f77b4", marker="o", markersize=3, linewidth=1.5)


def generate_report_pdf(path: Path, data: ReportData) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with PdfPages(str(path)) as pdf:
        fig = Figure(figsize=_PAGE_SIZE)
        _build_summary_page(fig, data)
        pdf.savefig(fig)

        fig2 = Figure(figsize=_PAGE_SIZE)
        _build_line_chart_page(fig2, data.curve, y_attr="v_at_max_p",
                                title="各轉速下輸出最大功率時的電壓", ylabel="電壓 (V)")
        pdf.savefig(fig2)

        fig3 = Figure(figsize=_PAGE_SIZE)
        _build_line_chart_page(fig3, data.curve, y_attr="p_max",
                                title="各轉速下對應的最大功率", ylabel="功率 (W)")
        pdf.savefig(fig3)
