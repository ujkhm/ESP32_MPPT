"""Minimal step indicator: circles connected by lines.

Style: filled cyan + checkmark for a completed step, cyan ring with a small
filled dot for the current step, plain gray ring for a pending step. Matches
the reference screenshot supplied for this app.
"""

from __future__ import annotations

import tkinter as tk
from typing import Sequence

_DONE_COLOR = "#06b6d4"
_PENDING_COLOR = "#d1d5db"
_LABEL_COLOR = "#374151"
_BG = "#ffffff"
_RADIUS = 13


class ProgressBar(tk.Canvas):
    def __init__(self, master: tk.Misc, steps: Sequence[str], **kwargs) -> None:
        super().__init__(master, bg=_BG, highlightthickness=0, height=64, **kwargs)
        self._steps = list(steps)
        self._current_index = 0
        self.bind("<Configure>", lambda _e: self._redraw())

    def set_current(self, index: int) -> None:
        self._current_index = max(0, min(index, len(self._steps) - 1))
        self._redraw()

    def _redraw(self) -> None:
        self.delete("all")
        width = max(self.winfo_width(), 1)
        n = len(self._steps)
        if n == 0:
            return
        margin = 70
        usable = max(width - 2 * margin, 1)
        xs = [margin + usable * i / max(n - 1, 1) for i in range(n)]
        y = 24

        for i in range(n - 1):
            color = _DONE_COLOR if i < self._current_index else _PENDING_COLOR
            self.create_line(xs[i] + _RADIUS, y, xs[i + 1] - _RADIUS, y, fill=color, width=3)

        for i, (x, label) in enumerate(zip(xs, self._steps)):
            if i < self._current_index:
                self.create_oval(x - _RADIUS, y - _RADIUS, x + _RADIUS, y + _RADIUS,
                                  fill=_DONE_COLOR, outline=_DONE_COLOR)
                self.create_text(x, y, text="\u2713", fill="white", font=("Segoe UI", 12, "bold"))
            elif i == self._current_index:
                self.create_oval(x - _RADIUS, y - _RADIUS, x + _RADIUS, y + _RADIUS,
                                  fill=_BG, outline=_DONE_COLOR, width=3)
                self.create_oval(x - 4, y - 4, x + 4, y + 4, fill=_DONE_COLOR, outline=_DONE_COLOR)
            else:
                self.create_oval(x - _RADIUS, y - _RADIUS, x + _RADIUS, y + _RADIUS,
                                  fill=_BG, outline=_PENDING_COLOR, width=2)
            self.create_text(x, y + _RADIUS + 15, text=label, fill=_LABEL_COLOR, font=("Segoe UI", 9))
