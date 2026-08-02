"""Serial flight-log extraction tab (DUMP protocol)."""
from __future__ import annotations

import os
import subprocess
import sys
import time
from datetime import datetime

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (
    QComboBox,
    QFileDialog,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QPlainTextEdit,
    QProgressBar,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from rid.serial_dump import list_ports
from .workers import SerialDumpWorker


class SerialPanel(QWidget):
    def __init__(self, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self._worker: SerialDumpWorker | None = None
        self._last_output = ""
        self._build_ui()
        self._refresh_ports()

    def _build_ui(self) -> None:
        root = QVBoxLayout(self)

        # --- port row ---
        row = QHBoxLayout()
        row.addWidget(QLabel("串口:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        row.addWidget(self.port_combo, 1)
        self.btn_refresh = QPushButton("刷新")
        row.addWidget(self.btn_refresh)
        row.addWidget(QLabel("波特率:"))
        self.baud_combo = QComboBox()
        self.baud_combo.addItems(["115200", "57600", "38400", "9600"])
        self.baud_combo.setCurrentText("115200")
        row.addWidget(self.baud_combo)
        root.addLayout(row)

        # --- action row ---
        act = QHBoxLayout()
        self.btn_dump = QPushButton("导出飞行日志")
        self.btn_dump.setEnabled(False)
        act.addWidget(self.btn_dump)
        self.btn_open = QPushButton("打开输出目录")
        self.btn_open.setEnabled(False)
        act.addWidget(self.btn_open)
        act.addStretch(1)
        root.addLayout(act)

        # --- progress ---
        self.progress = QProgressBar()
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        root.addWidget(self.progress)
        self.lbl_status = QLabel("就绪")
        root.addWidget(self.lbl_status)

        # --- log ---
        box = QGroupBox("日志")
        lv = QVBoxLayout(box)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(2000)
        lv.addWidget(self.log_view)
        root.addWidget(box, 1)

        self.btn_refresh.clicked.connect(self._refresh_ports)
        self.btn_dump.clicked.connect(self._start_dump)
        self.btn_open.clicked.connect(self._open_output_dir)

    # --------------------------------------------------------------- helpers
    def _refresh_ports(self) -> None:
        try:
            import serial  # noqa: F401
        except ImportError:
            self._log("错误：未安装 pyserial。请先运行: pip install pyserial")
            self.port_combo.clear()
            self.btn_dump.setEnabled(False)
            return
        self.port_combo.clear()
        for dev, desc in list_ports():
            self.port_combo.addItem(f"{dev} — {desc}", dev)
        self.btn_dump.setEnabled(self.port_combo.count() > 0)
        self._log(f"发现 {self.port_combo.count()} 个串口")

    def _start_dump(self) -> None:
        port = self.port_combo.currentData()
        if not port or self._worker is not None:
            return
        baud = int(self.baud_combo.currentText())
        default = f"flight_log_{datetime.now().strftime('%Y%m%d_%H%M%S')}.csv"
        path, _ = QFileDialog.getSaveFileName(self, "保存飞行日志", default, "CSV 文件 (*.csv)")
        if not path:
            return
        if not path.lower().endswith(".csv"):
            path += ".csv"
        self._last_output = path

        self.btn_dump.setEnabled(False)
        self.progress.setRange(0, 0)  # indeterminate until count known
        self.progress.setValue(0)
        self.lbl_status.setText(f"正在从 {port} 导出...")

        self._worker = SerialDumpWorker(port, baud, path, self)
        self._worker.sig_progress.connect(self._on_progress)
        self._worker.sig_done.connect(self._on_done)
        self._worker.sig_error.connect(self._on_error)
        self._worker.start()

    def _on_progress(self, msg: str, done: int, total: int) -> None:
        self.lbl_status.setText(msg)
        if total > 0:
            self.progress.setRange(0, total)
            self.progress.setValue(done)
        self._log(msg)

    def _on_done(self, result: dict) -> None:
        total = result["total"]
        bad = result["bad_crc"]
        written = result["written"]
        self.progress.setRange(0, 100)
        self.progress.setValue(100)
        self.lbl_status.setText(f"完成：{total} 条记录，{bad} 条 CRC 错误，已写 {written} 条")
        self._log(f"导出完成：{total} 条记录，{bad} 条 CRC 错误，CSV 已写 {written} 条")
        self._log(f"输出文件: {result['output']}")
        self.btn_dump.setEnabled(True)
        self.btn_open.setEnabled(written > 0)
        self._worker = None

    def _on_error(self, msg: str) -> None:
        self.progress.setRange(0, 100)
        self.progress.setValue(0)
        self.lbl_status.setText("导出失败")
        self._log(f"错误: {msg}")
        self.btn_dump.setEnabled(True)
        self._worker = None

    def _open_output_dir(self) -> None:
        if not self._last_output:
            return
        d = os.path.dirname(os.path.abspath(self._last_output)) or "."
        try:
            if sys.platform == "win32":
                os.startfile(d)  # type: ignore[attr-defined]
            else:
                subprocess.Popen(["xdg-open", d])
        except OSError as e:
            self._log(f"无法打开目录: {e}")

    def _log(self, msg: str) -> None:
        self.log_view.appendPlainText(msg)

    def shutdown(self) -> None:
        if self._worker is not None:
            self._worker.wait(3000)
