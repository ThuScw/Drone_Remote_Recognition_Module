"""QThread workers that run blocking/asyncio work off the GUI thread."""
from __future__ import annotations

import asyncio
import threading
import time
from typing import Any

from PySide6.QtCore import QThread, Signal

from rid.ble_scanner import extract_packet, format_mac, is_target
from rid.decoder import decode_gb_packet
from rid.serial_dump import dump_flight_log, records_to_csv, verify_record


class BleScanWorker(QThread):
    """Runs a bleak active scan in its own asyncio loop.

    Emits a DecodedPacket only for *distinct* GB packets (the module re-broadcasts
    the same packet on 3 channels per adv event), so the UI stays responsive.
    """

    sig_packet = Signal(object)   # DecodedPacket
    sig_device = Signal(object)   # dict: {mac, name, rssi, has_packet}
    sig_log = Signal(str)
    sig_state = Signal(bool)      # True = scanning
    sig_error = Signal(str)

    def __init__(self, parent: Any = None) -> None:
        super().__init__(parent)
        self._stop = threading.Event()
        self._last_raw = b""
        self._dev_throttle: dict[str, float] = {}

    def stop(self) -> None:
        self._stop.set()

    def run(self) -> None:
        self._stop.clear()
        self.sig_state.emit(True)
        self.sig_log.emit("正在开启蓝牙扫描...")
        try:
            asyncio.run(self._scan_loop())
        except (ImportError, OSError) as e:
            self.sig_error.emit(str(e))
        except Exception as e:  # noqa: BLE001 - surface any scan failure to UI
            self.sig_error.emit(f"扫描异常: {e}")
        finally:
            self.sig_state.emit(False)

    async def _scan_loop(self) -> None:
        from bleak import BleakScanner

        scanner = BleakScanner(detection_callback=self._on_detect, scanning_mode="active")
        await scanner.start()
        self.sig_log.emit("蓝牙扫描已开始，等待模块广播（目标 UUID 0x0D50 / 名称 GBI_RID_001）...")
        try:
            while not self._stop.is_set():
                await asyncio.sleep(0.05)
        finally:
            await scanner.stop()
            self.sig_log.emit("扫描已停止")

    def _on_detect(self, device: Any, adv: Any) -> None:
        name = (device.name or "") or (getattr(adv, "local_name", "") or "")
        if not is_target(name, adv):
            return
        mac = format_mac(str(device.address))
        rssi = getattr(adv, "rssi", 0) or 0
        packet = extract_packet(adv)

        # throttle per-device signals to ~1/s to avoid flooding the UI
        now = time.monotonic()
        if now - self._dev_throttle.get(mac, 0.0) >= 1.0 or packet is not None:
            self._dev_throttle[mac] = now
            self.sig_device.emit({"mac": mac, "name": name, "rssi": rssi,
                                  "has_packet": packet is not None})

        if not packet:
            return
        if packet == self._last_raw:
            return
        self._last_raw = packet
        pkt = decode_gb_packet(
            packet,
            address=mac,
            rssi=rssi,
            received_at_ms=int(time.monotonic() * 1000),
            source="ble",
        )
        self.sig_packet.emit(pkt)


class SerialDumpWorker(QThread):
    """Runs the DUMP protocol, then writes a CSV."""

    sig_progress = Signal(str, int, int)   # (message, done, total)
    sig_done = Signal(object)              # dict result
    sig_error = Signal(str)

    def __init__(self, port: str, baudrate: int, output_path: str, parent: Any = None) -> None:
        super().__init__(parent)
        self._port = port
        self._baudrate = baudrate
        self._output_path = output_path

    def run(self) -> None:
        try:
            self.sig_progress.emit("开始导出...", -1, -1)
            records = dump_flight_log(
                self._port, self._baudrate, progress=self.sig_progress.emit
            )
            if records is None:
                self.sig_error.emit("导出失败：设备无响应")
                return
            total = len(records)
            bad = sum(0 if verify_record(r.raw) else 1 for r in records)
            written = records_to_csv(records, self._output_path) if records else 0
            self.sig_done.emit({
                "total": total,
                "bad_crc": bad,
                "written": written,
                "output": self._output_path,
            })
        except Exception as e:  # noqa: BLE001
            self.sig_error.emit(f"导出异常: {e}")
