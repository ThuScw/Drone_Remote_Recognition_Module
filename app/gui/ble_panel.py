"""BLE receive + decode + health-judge tab."""
from __future__ import annotations

import time
from typing import Any

from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QComboBox,
    QGroupBox,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPlainTextEdit,
    QPushButton,
    QSplitter,
    QTableWidget,
    QTableWidgetItem,
    QVBoxLayout,
    QWidget,
)

from rid.ble_scanner import extract_gb_from_adv
from rid.decoder import decode_gb_packet, parse_hex
from rid.health import StreamAssessor
from rid.models import HealthLevel
from .workers import BleScanWorker

VERDICT_STYLE = {
    HealthLevel.PASS: "background:#e8f5e9;color:#1b5e20;font-weight:bold;padding:8px;",
    HealthLevel.WARN: "background:#fff8e1;color:#b26a00;font-weight:bold;padding:8px;",
    HealthLevel.FAIL: "background:#ffebee;color:#b71c1c;font-weight:bold;padding:8px;",
}
VERDICT_TEXT = {
    HealthLevel.PASS: "正常 ✓",
    HealthLevel.WARN: "警告 ⚠",
    HealthLevel.FAIL: "故障 ✗",
}


class BlePanel(QWidget):
    def __init__(self, parent: Any = None) -> None:
        super().__init__(parent)
        self._worker: BleScanWorker | None = None
        self._assessor = StreamAssessor()
        self._last_pkt = None
        self._packet_count = 0
        self._devices: dict[str, dict] = {}

        self._build_ui()

        # periodic stream-level health refresh (rate / staleness)
        self._timer = QTimer(self)
        self._timer.setInterval(1000)
        self._timer.timeout.connect(self._refresh_stream_health)
        self._timer.start()

    # ------------------------------------------------------------------ UI
    def _build_ui(self) -> None:
        root = QVBoxLayout(self)

        # --- toolbar row ---
        bar = QHBoxLayout()
        bar.addWidget(QLabel("设备:"))
        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(280)
        bar.addWidget(self.device_combo, 1)
        self.btn_start = QPushButton("开始扫描")
        self.btn_stop = QPushButton("停止扫描")
        self.btn_stop.setEnabled(False)
        self.btn_hex = QPushButton("粘贴 HEX 解码")
        bar.addWidget(self.btn_start)
        bar.addWidget(self.btn_stop)
        bar.addWidget(self.btn_hex)
        root.addLayout(bar)

        # --- status strip ---
        status = QHBoxLayout()
        self.lbl_state = QLabel("● 未扫描")
        self.lbl_rssi = QLabel("信号: --")
        self.lbl_count = QLabel("已收包: 0")
        for w in (self.lbl_state, self.lbl_rssi, self.lbl_count):
            status.addWidget(w)
        status.addStretch(1)
        root.addLayout(status)

        splitter = QSplitter(Qt.Vertical)
        root.addWidget(splitter, 1)

        # --- health + fields ---
        top = QSplitter(Qt.Horizontal)
        splitter.addWidget(top)

        health_box = QGroupBox("内置判断器")
        hv = QVBoxLayout(health_box)
        self.lbl_verdict = QLabel("等待数据...")
        self.lbl_verdict.setWordWrap(True)
        hv.addWidget(self.lbl_verdict)
        self.issue_list = QListWidget()
        hv.addWidget(self.issue_list, 1)
        top.addWidget(health_box)

        fields_box = QGroupBox("GB 46750-2025 字段明细")
        fv = QVBoxLayout(fields_box)
        self.fields_table = QTableWidget(0, 2)
        self.fields_table.setHorizontalHeaderLabels(["字段", "值"])
        self.fields_table.horizontalHeader().setStretchLastSection(True)
        self.fields_table.verticalHeader().setVisible(False)
        fv.addWidget(self.fields_table)
        top.addWidget(fields_box)
        top.setSizes([320, 520])

        # --- raw + log ---
        bottom = QSplitter(Qt.Horizontal)
        splitter.addWidget(bottom)
        raw_box = QGroupBox("原始数据包")
        rv = QVBoxLayout(raw_box)
        self.lbl_raw = QLabel("--")
        self.lbl_raw.setWordWrap(True)
        self.lbl_raw.setTextInteractionFlags(Qt.TextSelectableByMouse)
        rv.addWidget(self.lbl_raw)
        bottom.addWidget(raw_box)

        log_box = QGroupBox("日志")
        lv = QVBoxLayout(log_box)
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(2000)
        lv.addWidget(self.log_view)
        bottom.addWidget(log_box)
        bottom.setSizes([480, 520])

        # --- wiring ---
        self.btn_start.clicked.connect(self._start_scan)
        self.btn_stop.clicked.connect(self._stop_scan)
        self.btn_hex.clicked.connect(self._paste_hex)
        self._refresh_device_combo()

    # --------------------------------------------------------------- actions
    def _start_scan(self) -> None:
        if self._worker is not None and self._worker.isRunning():
            return
        try:
            import bleak  # noqa: F401
        except ImportError:
            self._log("错误：未安装 bleak。请先运行: pip install bleak")
            return
        self._assessor.reset()
        self._packet_count = 0
        self._worker = BleScanWorker(self)
        self._worker.sig_packet.connect(self._on_packet)
        self._worker.sig_device.connect(self._on_device)
        self._worker.sig_log.connect(self._log)
        self._worker.sig_state.connect(self._on_scan_state)
        self._worker.sig_error.connect(self._on_error)
        self._worker.start()

    def _stop_scan(self) -> None:
        if self._worker is not None:
            self._worker.stop()

    def _paste_hex(self) -> None:
        text, ok = QInputDialog.getMultiLineText(
            self,
            "粘贴 HEX",
            "粘贴抓包工具的 HEX 十六进制字节，支持两种格式：\n"
            "  1) nRF Connect 的整个 Raw 广播帧（自动提取其中的 GB 包）\n"
            "  2) 从 FF 开始的 Service Data 载荷（原 GB 数据包）",
        )
        if not ok or not text.strip():
            return
        try:
            raw = parse_hex(text)
        except ValueError as e:
            self._log(f"HEX 解析失败: {e}")
            return
        before = raw
        raw = extract_gb_from_adv(raw)
        if len(raw) < 6:
            self._log("HEX 过短，无法解析")
            return
        if raw != before:
            self._log(f"从广播帧中提取到 GB 包（{len(raw)} 字节）")
        pkt = decode_gb_packet(raw, address="手动", rssi=0,
                               received_at_ms=int(time.monotonic() * 1000),
                               source="manual")
        self._show_packet(pkt)
        self._assessor.push(pkt)
        self._refresh_stream_health()
        self._log(f"已手动解码 {len(raw)} 字节: {raw.hex().upper()}")

    # ---------------------------------------------------------------- events
    def _on_scan_state(self, scanning: bool) -> None:
        self.btn_start.setEnabled(not scanning)
        self.btn_stop.setEnabled(scanning)
        self.lbl_state.setText("● 扫描中" if scanning else "● 已停止")
        self.lbl_state.setStyleSheet(
            "color:#1b5e20;font-weight:bold;" if scanning else "color:#666;"
        )
        if not scanning and not self._devices:
            self._refresh_device_combo()

    def _on_error(self, msg: str) -> None:
        self._log(f"错误: {msg}")

    def _on_device(self, info: dict) -> None:
        mac = info["mac"]
        prev_selected = self.device_combo.currentData()  # previously selected MAC
        self._devices[mac] = info
        self._refresh_device_combo(prev_selected)
        self.lbl_rssi.setText(f"信号: {info['rssi']} dBm")

    def _refresh_device_combo(self, prev_selected: str | None = None) -> None:
        self.device_combo.clear()
        if not self._devices:
            self.device_combo.addItem("（未发现模块 — 点“开始扫描”后自动填充）")
            item = self.device_combo.model().item(0)
            item.setEnabled(False)
            self.device_combo.setCurrentIndex(0)
            return
        for m, d in self._devices.items():
            label = f"{d['name'] or '(无名称)'}  {m}  RSSI {d['rssi']} dBm"
            if not d["has_packet"]:
                label += "  [未取到包]"
            self.device_combo.addItem(label, m)
        if prev_selected:
            idx = self.device_combo.findData(prev_selected)
            if idx >= 0:
                self.device_combo.setCurrentIndex(idx)

    def _on_packet(self, pkt: Any) -> None:
        self._packet_count += 1
        self.lbl_count.setText(f"已收包: {self._packet_count}")
        self._show_packet(pkt)
        self._assessor.push(pkt)

    def _refresh_stream_health(self) -> None:
        if self._last_pkt is None:
            return
        rep = self._assessor.report()
        self._render_report(rep)

    # ---------------------------------------------------------------- render
    def _show_packet(self, pkt: Any) -> None:
        self._last_pkt = pkt
        self.lbl_raw.setText(f"{pkt.address}  {pkt.raw.hex(' ').upper()}")
        # fields table
        rows = list(pkt.fmt.items())
        if pkt.structure_error:
            rows.insert(0, ("结构错误", pkt.structure_error))
        rows.insert(0, ("地址", pkt.address))
        self.fields_table.setRowCount(len(rows))
        for r, (k, v) in enumerate(rows):
            self.fields_table.setItem(r, 0, QTableWidgetItem(str(k)))
            self.fields_table.setItem(r, 1, QTableWidgetItem(str(v)))
        # per-packet issues
        self._render_report(self._assessor.report())

    def _render_report(self, rep: Any) -> None:
        self.lbl_verdict.setStyleSheet(VERDICT_STYLE[rep.level])
        extra = f"  速率 {rep.avg_rate_hz:.1f} 包/s" if rep.packets_seen else ""
        self.lbl_verdict.setText(f"{VERDICT_TEXT[rep.level]}  {rep.note}{extra}")
        self.issue_list.clear()
        if not rep.issues:
            self.issue_list.addItem("未发现问题")
            self.issue_list.item(0).setForeground(QColor("#1b5e20"))
        for iss in rep.issues:
            item = QListWidgetItem(f"[{iss.label}] {iss.message}")
            color = {
                HealthLevel.PASS: QColor("#1b5e20"),
                HealthLevel.WARN: QColor("#b26a00"),
                HealthLevel.FAIL: QColor("#b71c1c"),
            }[iss.level]
            item.setForeground(color)
            self.issue_list.addItem(item)

    def _log(self, msg: str) -> None:
        self.log_view.appendPlainText(msg)

    def shutdown(self) -> None:
        self._timer.stop()
        if self._worker is not None and self._worker.isRunning():
            self._worker.stop()
            self._worker.wait(2000)
