"""Windows 11 の仮想カメラ (vcam/ でビルドする iphonecam.dll) を ctypes で操作する。

カメラは作ったプロセスが生きている間だけ存在し、終了すると Windows が自動で消す。
"""

from __future__ import annotations

import ctypes
import os
from pathlib import Path

import numpy as np

DLL_PATH = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "iPhoneCam" / "iphonecam.dll"


class VirtualCameraError(Exception):
    pass


def _check(hr: int, what: str) -> int:
    if hr < 0:
        raise VirtualCameraError(f"{what} に失敗しました (0x{hr & 0xFFFFFFFF:08X})")
    return hr


class _Api:
    _dll: ctypes.WinDLL | None = None

    @classmethod
    def get(cls) -> ctypes.WinDLL:
        if cls._dll is None:
            if not DLL_PATH.exists():
                raise VirtualCameraError(f"{DLL_PATH} がありません (vcam\\install_vcam.bat でインストール)")
            dll = ctypes.WinDLL(str(DLL_PATH))
            hr, ptr, u32 = ctypes.c_long, ctypes.c_void_p, ctypes.c_uint32
            dll.ICam_GetFormat.argtypes = [ctypes.POINTER(u32), ctypes.POINTER(u32)]
            dll.ICam_GetFormat.restype = hr
            dll.ICam_Create.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ptr)]
            dll.ICam_Create.restype = hr
            dll.ICam_Send.argtypes = [ptr, ptr, u32]
            dll.ICam_Send.restype = hr
            dll.ICam_InUse.argtypes = [ptr]
            dll.ICam_InUse.restype = ctypes.c_int
            dll.ICam_Remove.argtypes = [ptr]
            dll.ICam_Remove.restype = hr
            cls._dll = dll
        return cls._dll


def available() -> bool:
    return DLL_PATH.exists()


def frame_size() -> tuple[int, int]:
    """インストール時に決めた仮想カメラの解像度 (全カメラ共通)。"""
    w, h = ctypes.c_uint32(), ctypes.c_uint32()
    _check(_Api.get().ICam_GetFormat(ctypes.byref(w), ctypes.byref(h)), "解像度の取得")
    return w.value, h.value


class VirtualCamera:
    """NV12 のフレームを受け取る仮想カメラ 1 台。"""

    def __init__(self, name: str):
        self.name = name
        self.width, self.height = frame_size()
        self._frame_bytes = self.width * self.height * 3 // 2
        handle = ctypes.c_void_p()
        _check(_Api.get().ICam_Create(name, ctypes.byref(handle)), f"仮想カメラ '{name}' の作成")
        self._handle = handle

    def send(self, nv12: np.ndarray) -> bool:
        """フレームを書き込む。カメラを使っているアプリがあれば True。"""
        if self._handle is None:
            return False
        if nv12.nbytes != self._frame_bytes or not nv12.flags.c_contiguous:
            raise ValueError(f"NV12 {self.width}x{self.height} の連続したバッファが必要です")
        hr = _Api.get().ICam_Send(self._handle, nv12.ctypes.data, nv12.nbytes)
        return _check(hr, "フレームの送信") == 0

    def in_use(self) -> bool:
        return self._handle is not None and bool(_Api.get().ICam_InUse(self._handle))

    def close(self) -> None:
        if self._handle is not None:
            handle, self._handle = self._handle, None
            _check(_Api.get().ICam_Remove(handle), f"仮想カメラ '{self.name}' の削除")
