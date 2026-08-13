"""GATT 双向配置 tab：反向写入 001/002/003/004 到模块。

地面态模块发出可连接广播（名称 GBI_RID_001 / 服务 0xFFF0），手机或本工具
通过 GATT 连接写入四个身份字段；起飞后模块广播写入值并拒绝再写。
"""
from __future__ import annotations

from typing import Any

from PySide6.QtWidgets import (
    QComboBox,
    QFormLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

from rid.gatt_config import (
    OP_CATEGORY_NAMES,
    UA_CLASS_NAMES,
    RidConfig,
)
from .workers import ConfigScanWorker, GattConfigWorker


class ConfigPanel(QWidget):
    def __init__(self, parent: Any = None) -> None:
        super().__init__(parent)
        self._scan_worker: ConfigScanWorker | None = None
        self._gatt_worker: GattConfigWorker | None = None
        self._devices: set[str] = set()
        self._connected_cfg: RidConfig | None = None

        self._build_ui()

    # ------------------------------------------------------------------ UI
    def _build_ui(self) -> None:
        root = QVBoxLayout(self)

        # --- device row ---
        bar = QHBoxLayout()
        bar.addWidget(QLabel("设备:"))
        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(240)
        bar.addWidget(self.device_combo, 1)
        self.btn_scan = QPushButton("扫描设备")
        self.btn_read = QPushButton("连接读取")
        self.btn_read.setEnabled(False)
        bar.addWidget(self.btn_scan)
        bar.addWidget(self.btn_read)
        root.addLayout(bar)

        # --- status ---
        self.lbl_state = QLabel("状态: --")
        root.addWidget(self.lbl_state)

        # --- fields ---
        fields_box = QGroupBox("身份字段（起飞前写入，起飞后广播）")
        form = QFormLayout(fields_box)

        self.edit_uas = QLineEdit()
        self.edit_uas.setMaxLength(20)
        self.edit_uas.setPlaceholderText("20 位 [0-9A-Z]，不含 O/I")
        form.addRow("唯一产品识别码", self.edit_uas)

        self.edit_realname = QLineEdit()
        self.edit_realname.setMaxLength(8)
        self.edit_realname.setPlaceholderText("8 位数字（UOM 实名号后 8 位）")
        form.addRow("实名登记标志", self.edit_realname)

        self.combo_op = QComboBox()
        for v in (0, 1, 2, 3):
            self.combo_op.addItem(f"{v} — {OP_CATEGORY_NAMES[v]}", v)
        form.addRow("运行类别", self.combo_op)

        self.combo_ua = QComboBox()
        for v in (0, 1, 2, 3, 4):
            self.combo_ua.addItem(f"{v} — {UA_CLASS_NAMES[v]}", v)
        form.addRow("无人机分类", self.combo_ua)

        root.addWidget(fields_box)

        # --- actions ---
        actions = QHBoxLayout()
        self.btn_write = QPushButton("写入配置")
        self.btn_write.setEnabled(False)
        self.btn_clear = QPushButton("清空输入")
        actions.addWidget(self.btn_write)
        actions.addWidget(self.btn_clear)
        actions.addStretch(1)
        root.addLayout(actions)

        # --- hint ---
        hint = QLabel(
            "说明：模块仅在地面态接受写入；空中态写锁定。写入成功后即持久化到 NVS，"
            "下次起飞即广播本次录入值。未写入直接起飞则广播占位值或上次存储值。"
        )
        hint.setWordWrap(True)
        root.addWidget(hint)

        # --- log ---
        self.log_view = QPlainTextEdit()
        self.log_view.setReadOnly(True)
        self.log_view.setMaximumBlockCount(2000)
        root.addWidget(self.log_view, 1)

        # --- wiring ---
        self.btn_scan.clicked.connect(self._start_scan)
        self.btn_read.clicked.connect(self._read_config)
        self.btn_write.clicked.connect(self._write_config)
        self.btn_clear.clicked.connect(self._clear_fields)
        self.device_combo.currentIndexChanged.connect(self._on_device_selected)

    # --------------------------------------------------------------- actions
    def _start_scan(self) -> None:
        if self._scan_worker is not None and self._scan_worker.isRunning():
            return
        try:
            import bleak  # noqa: F401
        except ImportError:
            self._log("错误：未安装 bleak。请先运行: pip install bleak")
            return
        self._devices.clear()
        self.device_combo.clear()
        self.btn_scan.setEnabled(False)
        self._scan_worker = ConfigScanWorker(self)
        self._scan_worker.sig_device.connect(self._on_device)
        self._scan_worker.sig_log.connect(self._log)
        self._scan_worker.sig_done.connect(self._on_scan_done)
        self._scan_worker.start()

    def _on_device(self, mac: str) -> None:
        if mac in self._devices:
            return
        self._devices.add(mac)
        self.device_combo.addItem(mac, mac)

    def _on_scan_done(self) -> None:
        self.btn_scan.setEnabled(True)
        if not self._devices:
            self.device_combo.addItem("（未发现模块 — 确认模块处于地面可连接态）")
            self.device_combo.model().item(0).setEnabled(False)
            self.btn_read.setEnabled(False)

    def _on_device_selected(self, _idx: int) -> None:
        mac = self.device_combo.currentData()
        self.btn_read.setEnabled(bool(mac))

    def _read_config(self) -> None:
        mac = self.device_combo.currentData()
        if not mac:
            return
        self._run_gatt(mac, "read")

    def _write_config(self) -> None:
        mac = self.device_combo.currentData()
        if not mac:
            return
        cfg = self._collect_fields()
        errs = cfg.validate()
        if errs:
            for e in errs:
                self._log(f"校验失败: {e}")
            return
        self._run_gatt(mac, "write", cfg)

    def _run_gatt(self, mac: str, action: str, cfg: RidConfig | None = None) -> None:
        if self._gatt_worker is not None and self._gatt_worker.isRunning():
            self._log("已有连接操作进行中，请稍候")
            return
        self.btn_read.setEnabled(False)
        self.btn_write.setEnabled(False)
        self._gatt_worker = GattConfigWorker(mac, action, cfg, self)
        self._gatt_worker.sig_read.connect(self._on_read)
        self._gatt_worker.sig_written.connect(self._on_written)
        self._gatt_worker.sig_log.connect(self._log)
        self._gatt_worker.sig_error.connect(self._on_error)
        self._gatt_worker.finished.connect(self._on_gatt_done)
        self._gatt_worker.start()

    def _on_gatt_done(self) -> None:
        mac = self.device_combo.currentData()
        self.btn_read.setEnabled(bool(mac))
        self.btn_write.setEnabled(bool(mac) and self._connected_cfg is not None)

    # ---------------------------------------------------------------- events
    def _on_read(self, cfg: RidConfig) -> None:
        self._connected_cfg = cfg
        self._populate_fields(cfg)
        self._log(f"读取成功 — 状态: {cfg.state_name}")

    def _on_written(self, cfg: RidConfig) -> None:
        self._connected_cfg = cfg
        self._populate_fields(cfg)
        self._log("写入成功 — 已持久化到模块 NVS")

    def _on_error(self, msg: str) -> None:
        self._log(f"错误: {msg}")

    # ---------------------------------------------------------------- fields
    def _collect_fields(self) -> RidConfig:
        return RidConfig(
            uas_id=self.edit_uas.text().strip(),
            realname=self.edit_realname.text().strip(),
            op_category=self.combo_op.currentData(),
            ua_class=self.combo_ua.currentData(),
        )

    def _populate_fields(self, cfg: RidConfig) -> None:
        self.edit_uas.setText(cfg.uas_id)
        self.edit_realname.setText(cfg.realname)
        if cfg.op_category in (0, 1, 2, 3):
            self.combo_op.setCurrentIndex(cfg.op_category)
        if cfg.ua_class in (0, 1, 2, 3, 4):
            self.combo_ua.setCurrentIndex(cfg.ua_class)
        self.lbl_state.setText(f"状态: {cfg.state_name}")
        self.btn_write.setEnabled(True)

    def _clear_fields(self) -> None:
        self.edit_uas.clear()
        self.edit_realname.clear()
        self.combo_op.setCurrentIndex(1)   # 开放类
        self.combo_ua.setCurrentIndex(1)   # 轻型
        self.lbl_state.setText("状态: --")

    # ----------------------------------------------------------------- log
    def _log(self, msg: str) -> None:
        self.log_view.appendPlainText(msg)

    def shutdown(self) -> None:
        if self._scan_worker is not None and self._scan_worker.isRunning():
            self._scan_worker.stop()
            self._scan_worker.wait(2000)
        if self._gatt_worker is not None and self._gatt_worker.isRunning():
            self._gatt_worker.wait(3000)
