"""Main application window: tabbed BLE + Serial."""
from __future__ import annotations

from PySide6.QtWidgets import QMainWindow, QTabWidget

from .ble_panel import BlePanel
from .serial_panel import SerialPanel


class MainWindow(QMainWindow):
    def __init__(self) -> None:
        super().__init__()
        self.setWindowTitle("RID 模块检测工具 — GB 46750-2025")
        self.resize(960, 720)

        tabs = QTabWidget()
        self.ble_panel = BlePanel()
        self.serial_panel = SerialPanel()
        tabs.addTab(self.ble_panel, "BLE 接收 / 解码 / 自检")
        tabs.addTab(self.serial_panel, "串口飞行日志提取")
        self.setCentralWidget(tabs)

        self.statusBar().showMessage("就绪")

    def closeEvent(self, event) -> None:  # noqa: ANN001
        self.ble_panel.shutdown()
        self.serial_panel.shutdown()
        super().closeEvent(event)
