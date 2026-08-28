"""Bluetooth Classic (SPP) device discovery + pairing via Windows Runtime (winsdk).

This is the "let the user find and pair the micro generator from inside the app"
piece. Windows still shows its own one-time consent/PIN prompt during pairing
(that is a Windows security requirement, not something an app can skip), but the
scanning and triggering of pairing itself happens in-app, without leaving to
Settings.

Runs its own asyncio event loop in a background thread (Tkinter's mainloop is not
asyncio-based). All callbacks fire on that background thread -- callers must hop
back to the UI thread themselves, exactly like core.serial_worker.SerialWorker
already does with its polling-queue pattern.

Windows-only; requires the `winsdk` package (see requirements.txt).

★實測踩過的坑(見對話紀錄 2026-08-27)★：
1. 用 AEP(Association Endpoint) selector 的 create_watcher() 掃「附近裝置」在這台機器上
   會不定期直接回傳 0 個結果(即使已配對裝置明明存在)，看起來是本機藍牙堆疊本身的
   問題(使用者反映過這台電腦的藍牙曾經整個不能用、重灌驅動才恢復)，不能只依賴這條路。
2. 已經配對過的裝置，Windows 早就建立好對應的虛擬 COM 埠(不是「配對當下」才建立)，
   所以「比對配對前後多出來的 COM 埠」這個方法對「已配對裝置」完全找不到東西
   (根本沒有新埠)，只對「這台電腦從來沒配對過」的全新裝置才有效。
   因此已配對裝置改用 BluetoothDevice.from_id_async().bluetooth_address 拿到真正的
   MAC，直接去比對 COM 埠 hwid 字串裡有沒有含這個位址(見 serial_worker.find_port_by_mac)，
   這條路徑快(不必等)、也可靠(不依賴掃描結果)。
"""

from __future__ import annotations

import asyncio
import threading
from concurrent.futures import Future
from dataclasses import dataclass
from typing import Callable, List, Optional

try:
    from winsdk.windows.devices.bluetooth import BluetoothDevice
    from winsdk.windows.devices.enumeration import (
        DeviceInformation,
        DeviceInformationKind,
        DevicePairingKinds,
        DevicePairingResultStatus,
    )
    from winsdk.windows.devices.radios import Radio, RadioKind, RadioState

    WINSDK_AVAILABLE = True
    WINSDK_IMPORT_ERROR = ""
except ImportError as _exc:  # pragma: no cover - environment dependent
    WINSDK_AVAILABLE = False
    WINSDK_IMPORT_ERROR = str(_exc)

# Well-known AQS selector for "Bluetooth Classic AEP": matches both paired and
# unpaired nearby devices. Same GUID Microsoft's own DeviceEnumerationAndPairing
# sample uses (Windows-universal-samples/DeviceEnumerationAndPairing). 用來找
# "還沒配對過"的附近裝置；在這台測試機上偶爾會回傳空清單(見檔案開頭說明)，
# 因此不是唯一的裝置來源，另見 list_paired_devices()。
_BT_CLASSIC_AEP_SELECTOR = 'System.Devices.Aep.ProtocolId:="{e0cbf06c-cd8b-4647-bb8a-263b43f0f974}"'


@dataclass
class BtDeviceInfo:
    id: str
    name: str
    is_paired: bool
    mac: Optional[str] = None  # 12 碼十六進位(無分隔符)，已配對裝置才會有


def _format_mac(addr: int) -> str:
    return f"{addr:012X}"


class BtPairingWorker:
    """Owns a background thread + asyncio loop for every WinRT call.

    Usage: start() once at app startup, then list_paired_devices()/start_scan()/
    pair()/stop_scan() from the UI thread as needed; stop() when the app closes.
    """

    def __init__(self) -> None:
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._watcher = None
        self._ready = threading.Event()

    @property
    def available(self) -> bool:
        return WINSDK_AVAILABLE

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._run_loop, daemon=True, name="BtPairingWorker")
        self._thread.start()
        self._ready.wait(timeout=5.0)

    def _run_loop(self) -> None:
        self._loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self._loop)
        self._ready.set()
        self._loop.run_forever()

    def stop(self) -> None:
        self.stop_scan()
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(self._loop.stop)

    def _schedule(self, coro) -> "Future":
        if not self._loop:
            raise RuntimeError("BtPairingWorker.start() must be called first")
        return asyncio.run_coroutine_threadsafe(coro, self._loop)

    # ---- 系統藍牙開關狀態 ----
    def check_bluetooth_ready(
        self,
        on_result: Callable[[bool, str], None],
    ) -> None:
        """回呼 (ready, message)。ready=False 時 message 為給使用者看的說明。"""
        if not WINSDK_AVAILABLE:
            on_result(True, "")
            return
        self._schedule(self._check_bluetooth_ready_coro(on_result))

    async def _check_bluetooth_ready_coro(self, on_result) -> None:
        try:
            radios = await Radio.get_radios_async()
            for radio in radios:
                if radio.kind == RadioKind.BLUETOOTH:
                    if radio.state == RadioState.ON:
                        on_result(True, "")
                    else:
                        on_result(False, "系統藍牙目前是關閉的，請先在 Windows 設定中開啟藍牙後再連線。")
                    return
            on_result(False, "找不到本機藍牙無線電裝置，請確認驅動程式與藍牙硬體是否正常。")
        except Exception as exc:  # noqa: BLE001
            on_result(False, f"無法查詢藍牙狀態：{exc}")

    # ---- 已配對裝置(可靠、快，優先用這個) ----
    def list_paired_devices(
        self,
        on_result: Callable[[List[BtDeviceInfo]], None],
        on_error: Callable[[str], None],
    ) -> None:
        """一次性查詢目前所有已配對的傳統藍牙裝置，順便解析出每個裝置的 MAC。
        不需要「掃描」等待，Windows 直接回傳已知清單，比 AEP watcher 可靠很多。"""
        if not WINSDK_AVAILABLE:
            on_error(f"winsdk 套件未安裝或無法載入：{WINSDK_IMPORT_ERROR}")
            return
        self._schedule(self._list_paired_coro(on_result, on_error))

    async def _list_paired_coro(self, on_result, on_error) -> None:
        try:
            selector = BluetoothDevice.get_device_selector()
            infos = await DeviceInformation.find_all_async(selector, [])
            result: List[BtDeviceInfo] = []
            for info in infos:
                mac = None
                try:
                    bt_dev = await BluetoothDevice.from_id_async(info.id)
                    if bt_dev is not None:
                        mac = _format_mac(bt_dev.bluetooth_address)
                except Exception:
                    mac = None
                result.append(BtDeviceInfo(id=info.id, name=info.name or "(未命名裝置)",
                                            is_paired=True, mac=mac))
            on_result(result)
        except Exception as exc:  # noqa: BLE001
            on_error(f"查詢已配對裝置失敗：{exc}")

    # ---- 掃描附近尚未配對的裝置(在這台機器上實測會不定期回傳空清單，僅供輔助) ----
    def start_scan(
        self,
        on_device_found: Callable[[BtDeviceInfo], None],
        on_error: Callable[[str], None],
    ) -> None:
        if not WINSDK_AVAILABLE:
            on_error(f"winsdk 套件未安裝或無法載入：{WINSDK_IMPORT_ERROR}")
            return
        self.stop_scan()
        self._schedule(self._scan_coro(on_device_found, on_error))

    async def _scan_coro(self, on_device_found, on_error) -> None:
        try:
            watcher = DeviceInformation.create_watcher(
                _BT_CLASSIC_AEP_SELECTOR, [], DeviceInformationKind.ASSOCIATION_ENDPOINT
            )
            self._watcher = watcher

            def _added(_watcher_obj, info) -> None:
                on_device_found(
                    BtDeviceInfo(
                        id=info.id,
                        name=info.name or "(未命名裝置)",
                        is_paired=bool(info.pairing and info.pairing.is_paired),
                    )
                )

            def _updated(_watcher_obj, _update) -> None:
                pass

            watcher.add_added(_added)
            watcher.add_updated(_updated)
            watcher.start()
        except Exception as exc:  # noqa: BLE001 - any WinRT failure must reach the UI
            on_error(f"啟動藍牙掃描失敗：{exc}")

    def stop_scan(self) -> None:
        w = self._watcher
        self._watcher = None
        if w is not None:
            try:
                w.stop()
            except Exception:
                pass

    # ---- 配對 ----
    def pair(self, device_id: str, on_done: Callable[[bool, str, Optional[str]], None]) -> None:
        """觸發配對(裝置已經配對過的話直接視為成功，不會重新跑一次配對流程)。
        on_done(success, message, mac_or_None) 只會被呼叫一次。"""
        if not WINSDK_AVAILABLE:
            on_done(False, f"winsdk 套件未安裝或無法載入：{WINSDK_IMPORT_ERROR}", None)
            return
        self._schedule(self._pair_coro(device_id, on_done))

    async def _pair_coro(self, device_id: str, on_done: Callable[[bool, str, Optional[str]], None]) -> None:
        try:
            info = await DeviceInformation.create_from_id_async(device_id)
            if info is None:
                on_done(False, "找不到裝置(可能已離開範圍，請重新掃描)", None)
                return
            if info.pairing is None:
                on_done(False, "此裝置不支援配對", None)
                return

            async def _resolve_mac() -> Optional[str]:
                try:
                    bt_dev = await BluetoothDevice.from_id_async(info.id)
                    return _format_mac(bt_dev.bluetooth_address) if bt_dev is not None else None
                except Exception:
                    return None

            if info.pairing.is_paired:
                on_done(True, "裝置已經配對過", await _resolve_mac())
                return
            if not info.pairing.can_pair:
                on_done(False, "此裝置目前無法配對", None)
                return

            custom = info.pairing.custom

            def _pairing_requested(_sender, args) -> None:
                # ESP32 BluetoothSerial 預設走「Just Works」，不需要輸入 PIN，
                # 直接接受即可；Windows 仍可能彈一次系統同意視窗，這是安全機制、
                # 無法也不應該從應用程式端略過。
                args.accept()

            token = custom.add_pairing_requested(_pairing_requested)
            try:
                result = await custom.pair_async(DevicePairingKinds.CONFIRM_ONLY)
            finally:
                custom.remove_pairing_requested(token)

            ok = result.status in (
                DevicePairingResultStatus.PAIRED,
                DevicePairingResultStatus.ALREADY_PAIRED,
            )
            on_done(ok, str(result.status), await _resolve_mac() if ok else None)
        except Exception as exc:  # noqa: BLE001
            on_done(False, f"配對時發生例外：{exc}", None)
