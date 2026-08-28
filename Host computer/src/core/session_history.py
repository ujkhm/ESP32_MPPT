"""In-memory (session-only) history of completed test runs.

By design nothing here is persisted to disk: once the app closes, this history
is gone. Only explicitly-saved PDF reports survive, and only at the location the
user saved them to -- there is no "recover last run" feature."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime
from typing import Optional

from core.report_pdf import ReportData


@dataclass
class HistoryEntry:
    completed_at: datetime
    data: ReportData
    saved_path: Optional[str] = None  # None 代表使用者還沒按過「儲存報表」


class SessionHistory:
    def __init__(self) -> None:
        self._entries: list[HistoryEntry] = []

    def add(self, data: ReportData) -> HistoryEntry:
        entry = HistoryEntry(completed_at=datetime.now(), data=data)
        self._entries.append(entry)
        return entry

    def mark_saved(self, entry: HistoryEntry, path: str) -> None:
        entry.saved_path = path

    @property
    def entries(self) -> list[HistoryEntry]:
        return list(self._entries)
