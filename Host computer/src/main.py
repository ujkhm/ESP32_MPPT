"""ESP32 MPPT host application entry point."""

from __future__ import annotations

import sys
from pathlib import Path


def _bootstrap_import_path() -> None:
    src_dir = Path(__file__).resolve().parent
    if str(src_dir) not in sys.path:
        sys.path.insert(0, str(src_dir))


def main() -> None:
    _bootstrap_import_path()
    from app.main_window import run_app

    run_app()


if __name__ == "__main__":
    main()
