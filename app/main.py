#!/usr/bin/env python3
"""RID 模块检测工具 (GB 46750-2025) — entry point.

Run:
    python main.py

Features:
  1. BLE 扫描并接收模块广播（目标 UUID 0x0D50）
  2. GB 46750-2025 数据包解码
  3. 内置判断器：检查模块/广播是否正常
  4. 串口 DUMP：提取模块内部飞行日志 → CSV
"""
from __future__ import annotations

import sys


def main() -> int:
    # --- PySide6 is required for the whole app ---
    try:
        from PySide6.QtWidgets import QApplication
    except ImportError:
        print("缺少 PySide6，请先安装依赖：\n    pip install -r requirements.txt")
        return 1

    # warn (non-fatally) about optional deps
    try:
        import bleak  # noqa: F401
    except ImportError:
        print("[提示] 未安装 bleak，BLE 接收功能不可用（串口功能不受影响）")
    try:
        import serial  # noqa: F401
    except ImportError:
        print("[提示] 未安装 pyserial，串口飞行日志提取不可用（BLE 功能不受影响）")

    from gui.main_window import MainWindow

    app = QApplication(sys.argv)
    app.setApplicationName("RID 模块检测工具")
    window = MainWindow()
    window.show()
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
