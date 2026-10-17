"""iPhone の Safari から WebRTC で受け取ったカメラ映像を PC の仮想カメラへ流す。

使い方: python server.py
PC のブラウザに QR コードが表示されるので、iPhone のカメラで読み取って Safari で開く。
iPhone をつなぐたびに仮想カメラ (iPhone Camera 1, 2, ...) が増える。
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import errno
import io
import ipaddress
import json
import logging
import secrets
import socket
import ssl
import sys
import threading
import time
import urllib.request
import webbrowser
from collections.abc import Callable
from pathlib import Path

import av
import numpy as np
import pyvirtualcam
import qrcode
from aiohttp import web
from aiortc import RTCConfiguration, RTCPeerConnection, RTCSessionDescription
from aiortc.mediastreams import MediaStreamError
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
from PIL import Image, ImageDraw, ImageFont

import mfcam
import rtc_tuning

ROOT = Path(__file__).resolve().parent
STATIC = ROOT / "static"
CERT_DIR = ROOT / "certs"

# この秒数フレームが届かなければ待機画面に切り替える
STALE_SEC = 2.0
# この秒数映像が届かない接続は閉じる (Safari を一時的に裏へ回した程度では切らない)
DEAD_SEC = 20
# 待機画面を送る間隔
IDLE_INTERVAL = 0.1
# 管理ページのプレビューの最大幅
PREVIEW_WIDTH = 960
# ポート使用中のエラー番号 (Windows は WSAEADDRINUSE)
ADDR_IN_USE = {errno.EADDRINUSE, getattr(errno, "WSAEADDRINUSE", errno.EADDRINUSE)}
# 仮想カメラのネイティブ形式 → PyAV のピクセル形式
PIX_FORMATS = {
    pyvirtualcam.PixelFormat.NV12: "nv12",
    pyvirtualcam.PixelFormat.RGBA: "rgba",
    pyvirtualcam.PixelFormat.RGB: "rgb24",
    pyvirtualcam.PixelFormat.BGR: "bgr24",
}

log = logging.getLogger("iphonecam")


def lan_ip() -> str:
    """iPhone から届く LAN 側の IP を推定する (UDP の connect はパケットを送らない)。"""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect(("8.8.8.8", 80))
            return s.getsockname()[0]
        except OSError:
            return "127.0.0.1"


def ensure_cert(ip: str) -> tuple[Path, Path]:
    """LAN IP 用の自己署名証明書を用意する (Safari の getUserMedia は HTTPS 必須)。"""
    CERT_DIR.mkdir(exist_ok=True)
    cert_path, key_path = CERT_DIR / "cert.pem", CERT_DIR / "key.pem"
    now = dt.datetime.now(dt.timezone.utc)
    addr = ipaddress.ip_address(ip)

    if cert_path.exists() and key_path.exists():
        cert = x509.load_pem_x509_certificate(cert_path.read_bytes())
        try:
            san = cert.extensions.get_extension_for_class(x509.SubjectAlternativeName).value
            if addr in san.get_values_for_type(x509.IPAddress) and cert.not_valid_after_utc - now > dt.timedelta(days=7):
                return cert_path, key_path
        except x509.ExtensionNotFound:
            pass

    log.info("自己署名証明書を作成します (%s)", ip)
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, f"iPhone Camera ({ip})")])
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - dt.timedelta(minutes=5))
        .not_valid_after(now + dt.timedelta(days=365))
        .add_extension(x509.SubjectAlternativeName([x509.IPAddress(addr), x509.DNSName("localhost")]), critical=False)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
        .sign(key, hashes.SHA256())
    )
    key_path.write_bytes(
        key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
    )
    cert_path.write_bytes(cert.public_bytes(serialization.Encoding.PEM))
    return cert_path, key_path


def fit_frame(frame: av.VideoFrame, width: int, height: int, fill: bool, fmt: str) -> np.ndarray:
    """アスペクト比を保ったまま出力サイズ・形式に合わせる (fit=黒帯を付ける, fill=中央を切り抜く)。

    fmt は PyAV のピクセル形式 (nv12 / rgba / rgb24 / bgr24)。width, height は偶数であること。
    """
    if fill:
        scale = max(width / frame.width, height / frame.height)
        sw = max(width, round(frame.width * scale))
        sh = max(height, round(frame.height * scale))
        sw, sh = sw + sw % 2, sh + sh % 2
    else:
        scale = min(width / frame.width, height / frame.height)
        sw = max(2, min(width, round(frame.width * scale)))
        sh = max(2, min(height, round(frame.height * scale)))
        sw, sh = sw - sw % 2, sh - sh % 2  # nv12 の色差は 2x2 画素単位なので偶数に揃える

    img = frame.reformat(width=sw, height=sh, format=fmt).to_ndarray()
    if (sw, sh) == (width, height):
        return img

    x, y = (abs(sw - width) // 2) & ~1, (abs(sh - height) // 2) & ~1
    if fmt == "nv12":
        luma, chroma = img[:sh], img[sh:]
        if fill:
            return np.vstack((luma[y : y + height, x : x + width], chroma[y // 2 : (y + height) // 2, x : x + width]))
        canvas = np.empty((height * 3 // 2, width), np.uint8)
        canvas[:height] = 16  # limited range の黒
        canvas[height:] = 128
        canvas[y : y + sh, x : x + sw] = luma
        canvas[height + y // 2 : height + (y + sh) // 2, x : x + sw] = chroma
        return canvas

    if fill:
        return np.ascontiguousarray(img[y : y + height, x : x + width])
    canvas = np.zeros((height, width, img.shape[2]), np.uint8)
    if img.shape[2] == 4:
        canvas[..., 3] = 255
    canvas[y : y + sh, x : x + sw] = img
    return canvas


def make_idle_frame(width: int, height: int, fmt: str, label: str) -> np.ndarray:
    img = Image.new("RGB", (width, height), (16, 16, 20))
    draw = ImageDraw.Draw(img)
    big = ImageFont.load_default(size=max(16, height // 14))
    small = ImageFont.load_default(size=max(12, height // 26))
    draw.text((width / 2, height / 2 - height // 20), label, fill=(230, 230, 235), font=big, anchor="mm")
    draw.text((width / 2, height / 2 + height // 20), "Waiting for iPhone...", fill=(150, 150, 160), font=small, anchor="mm")
    return av.VideoFrame.from_ndarray(np.asarray(img), format="rgb24").reformat(format=fmt).to_ndarray()


def to_jpeg(img: np.ndarray, fmt: str, max_width: int | None = None) -> bytes:
    frame = av.VideoFrame.from_ndarray(img, format=fmt)
    width, height = frame.width, frame.height
    if max_width and width > max_width:
        width, height = max_width, round(height * max_width / width)
    rgb = frame.reformat(width=width, height=height, format="rgb24").to_ndarray()
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, "JPEG", quality=75)
    return buf.getvalue()


# ---- 仮想カメラの出力先 ----


class Sink:
    """仮想カメラ 1 台分の出力先。"""

    fmt = "rgb24"
    device: str | None = None

    def __init__(self, width: int, height: int):
        self.width, self.height = width, height

    def send(self, img: np.ndarray) -> bool | None:
        """フレームを送る。戻り値はカメラを使っているアプリがあるか (分からなければ None)。"""
        return None

    def close(self) -> None:
        pass


class MFSink(Sink):
    """Windows 11 の仮想カメラ (vcam/ の iphonecam.dll)。何台でも作れる。"""

    fmt = "nv12"

    def __init__(self, name: str):
        self._cam = mfcam.VirtualCamera(name)
        super().__init__(self._cam.width, self._cam.height)
        self.device = name

    def send(self, img: np.ndarray) -> bool | None:
        return self._cam.send(img)

    def close(self) -> None:
        self._cam.close()


class PyVirtualCamSink(Sink):
    """OBS Virtual Camera など pyvirtualcam が扱える仮想カメラ。作れるのは 1 台だけ。"""

    def __init__(self, width: int, height: int, fps: int, backend: str | None):
        super().__init__(width, height)
        cam = pyvirtualcam.Camera(width, height, fps, fmt=pyvirtualcam.PixelFormat.RGB, backend=backend)
        # ドライバのネイティブ形式で開き直し、pyvirtualcam 内部の変換 (1080p で数 ms) を省く
        native = cam.native_fmt
        if native != pyvirtualcam.PixelFormat.RGB and native in PIX_FORMATS:
            cam.close()
            cam = pyvirtualcam.Camera(width, height, fps, fmt=native, backend=backend)
            self.fmt = PIX_FORMATS[native]
        self._cam = cam
        self.device = cam.device

    def send(self, img: np.ndarray) -> bool | None:
        self._cam.send(img)
        return None

    def close(self) -> None:
        self._cam.close()


class CameraOutput:
    """受信したフレームを届いた時点で変換し、仮想カメラ 1 台へ送る。

    変換と送出は専用スレッドで行う。追いつかないときは古いフレームを捨てて最新だけを送り、
    遅延を溜めない。iPhone が繋がっていない間は待機画面を送り続ける。
    """

    def __init__(self, label: str, open_sink: Callable[[], Sink], size: tuple[int, int], fill: bool):
        self.label, self.fill = label, fill
        self._open_sink = open_sink
        # 出力先を開くまでの仮の値。開いた後は出力先に合わせる
        self.width, self.height = size
        self.fmt = "rgb24"
        self.device: str | None = None
        self.error: str | None = None
        self.in_use: bool | None = None
        self.process_ms = 0.0
        self._cond = threading.Condition()
        self._pending: av.VideoFrame | None = None
        self._last: np.ndarray | None = None  # 最後に送った映像 (プレビュー用)
        self._last_at = 0.0
        self._listeners: set[Callable[[], None]] = set()
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, name=f"output-{label}", daemon=True)

    def start(self) -> None:
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        with self._cond:
            self._cond.notify()
        self._thread.join(timeout=3)

    def subscribe(self, callback: Callable[[], None]) -> Callable[[], None]:
        """フレームを送るたびに (変換スレッドから) 呼ばれる callback を登録する。戻り値で解除。"""
        self._listeners.add(callback)
        return lambda: self._listeners.discard(callback)

    def submit(self, frame: av.VideoFrame) -> None:
        with self._cond:
            self._pending = frame
            self._cond.notify()

    def latest(self) -> np.ndarray | None:
        with self._cond:
            if self._last is not None and time.monotonic() - self._last_at < STALE_SEC:
                return self._last
        return None

    def _next_frame(self) -> av.VideoFrame | None:
        with self._cond:
            if self._pending is None:
                self._cond.wait(IDLE_INTERVAL)
            frame, self._pending = self._pending, None
            return frame

    def _run(self) -> None:
        sink: Sink | None = None
        try:
            sink = self._open_sink()
        except Exception as e:  # ドライバ未導入など。プレビューだけは動かし続ける
            self.error = str(e)
            log.error("%s: 仮想カメラを開けません: %s", self.label, e)
        if sink is not None:
            self.width, self.height, self.fmt, self.device = sink.width, sink.height, sink.fmt, sink.device
            log.info("%s: 仮想カメラ出力 %s (%dx%d, %s)", self.label, sink.device, self.width, self.height, self.fmt)

        idle = make_idle_frame(self.width, self.height, self.fmt, self.label)
        try:
            while not self._stop.is_set():
                frame = self._next_frame()
                if frame is None:
                    if sink is not None and self.latest() is None:
                        self._send(sink, idle)
                    continue

                t0 = time.perf_counter()
                img = fit_frame(frame, self.width, self.height, self.fill, self.fmt)
                if sink is not None:
                    self._send(sink, img)
                with self._cond:
                    self._last, self._last_at = img, time.monotonic()
                self.process_ms = 0.9 * self.process_ms + 0.1 * (time.perf_counter() - t0) * 1000
                for callback in list(self._listeners):
                    callback()
        finally:
            if sink is not None:
                try:
                    sink.close()
                except Exception as e:
                    log.warning("%s: 仮想カメラを閉じられません: %s", self.label, e)

    def _send(self, sink: Sink, img: np.ndarray) -> None:
        try:
            in_use = sink.send(img)
        except Exception as e:
            log.warning("%s: フレームを送れません: %s", self.label, e)
            return
        if in_use is not None:
            self.in_use = in_use


# ---- iPhone との接続と、仮想カメラの割り当て ----


class Connection:
    """iPhone 1 台との WebRTC 接続。受け取った映像を割り当てられたカメラへ流す。"""

    def __init__(self, slot: Slot, peer: str, device_id: str, on_closed: Callable[[Connection], None]):
        self.slot, self.peer, self.device_id = slot, peer, device_id
        self.pc = RTCPeerConnection(RTCConfiguration(iceServers=[]))  # LAN 内なので STUN 不要
        self.source: tuple[int, int] | None = None
        self.fps = 0.0
        self.last_frame_at = time.monotonic()  # 接続直後は「今」とみなして猶予を与える
        self._on_closed = on_closed
        self._closed = False

        @self.pc.on("track")
        def on_track(track):
            if track.kind == "video":
                asyncio.create_task(self._consume(track))

        @self.pc.on("connectionstatechange")
        async def on_state():
            log.info("%s: 接続状態 %s (%s)", self.slot.name, self.pc.connectionState, self.peer)
            if self.pc.connectionState in ("failed", "closed"):
                await self.close()

    @property
    def connected(self) -> bool:
        return not self._closed and self.pc.connectionState == "connected"

    @property
    def silent_for(self) -> float:
        """最後にフレームが届いてからの秒数。

        Safari のタブを閉じても WebRTC の切断検知には数十秒かかるので、フレームの途絶で判断する。
        """
        return time.monotonic() - self.last_frame_at

    async def accept(self, offer: dict) -> RTCSessionDescription:
        await self.pc.setRemoteDescription(RTCSessionDescription(sdp=offer["sdp"], type=offer["type"]))
        await self.pc.setLocalDescription(await self.pc.createAnswer())
        return self.pc.localDescription

    async def _consume(self, track) -> None:
        count, t0 = 0, time.monotonic()
        while not self._closed:
            try:
                frame = await track.recv()
            except MediaStreamError:
                break
            self.slot.output.submit(frame)
            self.source = (frame.width, frame.height)
            count += 1
            now = self.last_frame_at = time.monotonic()
            if now - t0 >= 1.0:
                self.fps = count / (now - t0)
                count, t0 = 0, now
        await self.close()  # iPhone 側が配信を止めた (RTCP BYE など)

    def _ssrcs(self) -> list[int]:
        return [s.source for r in self.pc.getReceivers() for s in r.getSynchronizationSources()]

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        rtc_tuning.forget_stats(self._ssrcs())
        await self.pc.close()
        self._on_closed(self)

    def status(self) -> dict:
        stats = [rtc_tuning.rx_stats(ssrc) for ssrc in self._ssrcs()]
        return {
            "connected": self.connected,
            "peer": self.peer,
            "source": f"{self.source[0]}x{self.source[1]}" if self.source else None,
            "fps": round(self.fps, 1) if self.silent_for < 1.0 else 0.0,
            "mbps": round(sum(s.update_rate() for s in stats), 1),
            "stalls": sum(s.stalls for s in stats),
        }


class Slot:
    """仮想カメラ 1 台 (iPhone Camera N)。"""

    def __init__(self, slot_id: int, output: CameraOutput):
        self.id = slot_id
        self.name = output.label
        self.output = output
        self.conn: Connection | None = None
        self.device_id: str | None = None  # 最後にこのカメラへ配信した iPhone (再接続で同じカメラに戻す)
        self.idle_since = 0.0  # 最後に配信が終わった時刻 (空きカメラを選ぶ順番に使う)


class Hub:
    """iPhone の接続を受け付け、仮想カメラへ割り当てる。カメラは必要になるたびに増やす。

    割り当て方:
      - URL に cam=N があればそのカメラ (カメラごとの QR から開いたとき)
      - なければ、前回と同じ iPhone なら前回のカメラ。それ以外は空いているカメラ
        (一度も使われていないもの → 配信が終わってから長いもの の順)。空きが無ければ新しく作る。
        カメラの数は「同時につないだ iPhone の最大数」まで増える
    """

    MODES = ("auto", "fixed")

    def __init__(self, token: str, max_kbps: int, make_output: Callable[[int, str], CameraOutput]):
        self.token, self.max_kbps = token, max_kbps
        self.mode = "auto"
        self.slots: dict[int, Slot] = {}
        self._make_output = make_output
        self._next_id = 1

    def add_slot(self) -> Slot:
        slot_id = self._next_id
        self._next_id += 1
        output = self._make_output(slot_id, f"iPhone Camera {slot_id}")
        output.start()
        slot = self.slots[slot_id] = Slot(slot_id, output)
        log.info("%s を追加しました", slot.name)
        return slot

    async def remove_slot(self, slot_id: int) -> None:
        slot = self.slots.pop(slot_id, None)
        if slot is None:
            raise web.HTTPNotFound(text="no such camera")
        if slot.conn is not None:
            await slot.conn.close()
        await asyncio.get_running_loop().run_in_executor(None, slot.output.stop)
        log.info("%s を削除しました", slot.name)

    def _pick_slot(self, device_id: str, cam: int | None) -> Slot:
        if cam is not None:
            slot = self.slots.get(cam)
            if slot is None:
                raise web.HTTPNotFound(text="このカメラは削除されています")
            return slot
        for slot in self.slots.values():
            if device_id and slot.device_id == device_id:
                return slot
        # 映像が途絶えている接続のカメラも空きとみなす (タブを閉じた iPhone など)
        free = [s for s in self.slots.values() if s.conn is None or s.conn.silent_for > STALE_SEC]
        if free:
            return min(free, key=lambda s: (s.conn is not None, s.device_id is not None, s.idle_since, s.id))
        return self.add_slot()

    async def watchdog(self) -> None:
        """映像が長く途絶えた接続を閉じる (WebRTC の切断検知を待たない)。"""
        while True:
            await asyncio.sleep(2)
            for slot in list(self.slots.values()):
                if slot.conn is not None and slot.conn.silent_for > DEAD_SEC:
                    log.info("%s: %d 秒映像が届かないので切断します", slot.name, DEAD_SEC)
                    await slot.conn.close()

    async def offer(self, request: web.Request) -> web.Response:
        if not secrets.compare_digest(request.query.get("t", ""), self.token):
            return web.json_response({"error": "invalid token"}, status=403)
        params = await request.json()
        device_id = str(params.get("device") or "")[:64]
        cam = request.query.get("cam", "")

        slot = self._pick_slot(device_id, int(cam) if cam.isdigit() else None)
        if slot.conn is not None:
            # 同じ iPhone の再読み込み、またはカメラごとの QR で別の iPhone に入れ替わったとき
            await slot.conn.close()
        conn = Connection(slot, request.remote or "", device_id, self._on_closed)
        slot.conn = conn
        slot.device_id = device_id or slot.device_id
        log.info("%s に %s を割り当てました", slot.name, conn.peer)

        answer = await conn.accept(params)
        return web.json_response({
            "sdp": rtc_tuning.add_bandwidth(answer.sdp, self.max_kbps),
            "type": answer.type,
            "maxBitrate": self.max_kbps * 1000,
            "camera": slot.name,
        })

    def _on_closed(self, conn: Connection) -> None:
        if conn.slot.conn is conn:
            conn.slot.conn = None
            conn.slot.idle_since = time.monotonic()

    async def close(self) -> None:
        for slot_id in list(self.slots):
            await self.remove_slot(slot_id)

    def status(self) -> list[dict]:
        cameras = []
        for slot in sorted(self.slots.values(), key=lambda s: s.id):
            out = slot.output
            conn = slot.conn.status() if slot.conn else {"connected": False}
            cameras.append({
                "id": slot.id,
                "name": slot.name,
                **conn,
                "process_ms": round(out.process_ms, 1),
                "device": out.device,
                "error": out.error,
                "in_use": out.in_use,
                "size": f"{out.width}x{out.height}",
            })
        return cameras


# ---- HTTP ----


def build_phone_app(hub: Hub) -> web.Application:
    """iPhone 向け (HTTPS, LAN 公開)。"""
    app = web.Application()
    app.router.add_get("/", lambda _: web.FileResponse(STATIC / "phone.html", headers={"Cache-Control": "no-store"}))
    app.router.add_post("/offer", hub.offer)
    return app


def build_host_app(hub: Hub, phone_url: str, ui_port: int, backend_info: dict) -> web.Application:
    """PC 向けの管理ページ (127.0.0.1 のみ)。"""
    allowed_hosts = {f"127.0.0.1:{ui_port}", f"localhost:{ui_port}"}
    allowed_origins = {f"http://{h}" for h in allowed_hosts}

    @web.middleware
    async def local_only(request: web.Request, handler):
        # 他のサイトから管理 API を叩かれないよう、Host (DNS リバインディング対策) と Origin を確かめる
        if request.host not in allowed_hosts:
            raise web.HTTPForbidden()
        origin = request.headers.get("Origin")
        if request.method != "GET" and origin not in allowed_origins:
            raise web.HTTPForbidden()
        return await handler(request)

    def camera_url(cam: int | None) -> str:
        return f"{phone_url}&cam={cam}" if cam else phone_url

    qr_cache: dict[str, bytes] = {}

    def slot_output(request: web.Request) -> CameraOutput:
        slot = hub.slots.get(int(request.match_info["id"]))
        if slot is None:
            raise web.HTTPNotFound()
        return slot.output

    async def qr_png(request):
        cam = request.query.get("cam", "")
        url = camera_url(int(cam) if cam.isdigit() else None)
        if url not in qr_cache:
            qr = qrcode.QRCode(border=2, box_size=10)
            qr.add_data(url)
            buf = io.BytesIO()
            qr.make_image().save(buf)
            qr_cache[url] = buf.getvalue()
        return web.Response(body=qr_cache[url], content_type="image/png")

    async def status(_):
        cameras = hub.status()
        for camera in cameras:
            camera["url"] = camera_url(camera["id"])
        return web.json_response({"mode": hub.mode, "url": phone_url, "backend": backend_info, "cameras": cameras})

    async def set_mode(request):
        mode = (await request.json()).get("mode")
        if mode not in Hub.MODES:
            raise web.HTTPBadRequest(text="mode must be auto or fixed")
        hub.mode = mode
        return web.json_response({"mode": mode})

    async def add_camera(_):
        slot = hub.add_slot()
        return web.json_response({"id": slot.id, "name": slot.name})

    async def remove_camera(request):
        await hub.remove_slot(int(request.match_info["id"]))
        return web.json_response({})

    async def snapshot(request):
        output = slot_output(request)
        frame = output.latest()
        if frame is None:
            return web.Response(status=204)
        body = await asyncio.get_running_loop().run_in_executor(None, to_jpeg, frame, output.fmt)
        return web.Response(body=body, content_type="image/jpeg", headers={"Cache-Control": "no-store"})

    async def preview(request):
        """仮想カメラに送ったフレームを MJPEG で流す。エンコードが追いつかない分は間引く。"""
        output = slot_output(request)
        loop = asyncio.get_running_loop()
        updated = asyncio.Event()
        unsubscribe = output.subscribe(lambda: loop.call_soon_threadsafe(updated.set))
        resp = web.StreamResponse(headers={
            "Content-Type": "multipart/x-mixed-replace; boundary=frame",
            "Cache-Control": "no-store",
        })
        await resp.prepare(request)
        try:
            while True:
                await updated.wait()
                updated.clear()
                frame = output.latest()
                if frame is None:
                    continue
                jpeg = await loop.run_in_executor(None, to_jpeg, frame, output.fmt, PREVIEW_WIDTH)
                await resp.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n%s\r\n" % (len(jpeg), jpeg))
        except ConnectionResetError:
            pass
        finally:
            unsubscribe()
        return resp

    app = web.Application(middlewares=[local_only])
    app.router.add_get("/", lambda _: web.FileResponse(STATIC / "host.html", headers={"Cache-Control": "no-store"}))
    app.router.add_get("/qr.png", qr_png)
    app.router.add_get("/status", status)
    app.router.add_post("/mode", set_mode)
    app.router.add_post("/cameras", add_camera)
    app.router.add_delete("/cameras/{id:\\d+}", remove_camera)
    app.router.add_get("/cameras/{id:\\d+}/snapshot.jpg", snapshot)
    app.router.add_get("/cameras/{id:\\d+}/preview.mjpg", preview)
    return app


# ---- 起動 ----


def output_factory(args: argparse.Namespace) -> tuple[Callable[[int, str], CameraOutput], dict]:
    """仮想カメラの出力先を決める。Windows 11 の仮想カメラが入っていればそれを使い、何台でも作る。"""
    fill = args.mode == "fill"
    use_mf = args.backend in ("auto", "mf") and mfcam.available()
    if args.backend == "mf" and not use_mf:
        raise SystemExit("Windows 11 仮想カメラがインストールされていません (vcam\\install_vcam.bat)")

    if use_mf:
        size = mfcam.frame_size()
        info = {"kind": "mf", "name": "Windows 11 仮想カメラ", "size": f"{size[0]}x{size[1]}", "multi": True}

        def make(slot_id: int, name: str) -> CameraOutput:
            return CameraOutput(name, lambda: MFSink(name), size, fill)
    else:
        size = (args.width, args.height)
        backend = None if args.backend == "auto" else args.backend
        info = {"kind": "pyvirtualcam", "name": "OBS Virtual Camera など", "size": f"{size[0]}x{size[1]}", "multi": False}

        def make(slot_id: int, name: str) -> CameraOutput:
            def open_sink() -> Sink:
                if slot_id != 1:
                    raise RuntimeError("複数台には Windows 11 仮想カメラが必要です (vcam\\install_vcam.bat)")
                return PyVirtualCamSink(size[0], size[1], args.fps, backend)

            return CameraOutput(name, open_sink, size, fill)

    return make, info


class PortInUseError(Exception):
    def __init__(self, port: int):
        super().__init__(f"port {port} is in use")
        self.port = port


def is_already_running(ui_port: int) -> bool:
    """このツールが既に起動しているか (管理ページの /status が応答するか)。"""
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{ui_port}/status", timeout=1) as resp:
            return "url" in json.load(resp)
    except (OSError, ValueError):
        return False


async def serve(args: argparse.Namespace) -> None:
    ip = args.host or lan_ip()
    cert, key = ensure_cert(ip)
    token = secrets.token_urlsafe(12)
    phone_url = f"https://{ip}:{args.port}/?t={token}"
    host_url = f"http://127.0.0.1:{args.ui_port}/"

    max_kbps = int(args.bitrate * 1000)
    rtc_tuning.apply(max_kbps)
    make_output, backend_info = output_factory(args)
    hub = Hub(token, max_kbps, make_output)
    hub.mode = args.assign

    ssl_ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    ssl_ctx.load_cert_chain(cert, key)

    # 仮想カメラを開く前にポートを確保する (二重起動のときに仮想カメラを横取りしないため)
    runners = []
    for app, bind, port, ctx in (
        (build_phone_app(hub), "0.0.0.0", args.port, ssl_ctx),
        (build_host_app(hub, phone_url, args.ui_port, backend_info), "127.0.0.1", args.ui_port, None),
    ):
        runner = web.AppRunner(app, access_log=None)
        await runner.setup()
        runners.append(runner)
        try:
            await web.TCPSite(runner, bind, port, ssl_context=ctx).start()
        except OSError as e:
            for r in runners:
                await r.cleanup()
            if e.errno in ADDR_IN_USE:
                raise PortInUseError(port) from e
            raise
    hub.add_slot()  # 最初の 1 台は先に作っておく (アプリで先に選んでおけるように)
    watchdog = asyncio.create_task(hub.watchdog())

    qr = qrcode.QRCode(border=1)
    qr.add_data(phone_url)
    try:
        qr.print_ascii(invert=True)
    except UnicodeEncodeError:
        pass
    print(f"\n  iPhone で開く URL : {phone_url}")
    print(f"  PC の管理ページ   : {host_url}")
    print(f"  仮想カメラ        : {backend_info['name']} ({backend_info['size']})")
    print("  終了するには Ctrl+C\n")
    if not args.no_browser:
        webbrowser.open(host_url)

    try:
        await asyncio.Event().wait()
    finally:
        watchdog.cancel()
        await hub.close()
        for runner in runners:
            await runner.cleanup()


def main() -> None:
    ap = argparse.ArgumentParser(description="iPhone のカメラを Safari 経由で PC の仮想カメラにする")
    ap.add_argument("--host", help="QR に載せる PC の IP アドレス (既定: 自動検出)")
    ap.add_argument("--port", type=int, default=8443, help="iPhone 向け HTTPS ポート (既定: 8443)")
    ap.add_argument("--ui-port", type=int, default=8080, help="PC 管理ページのポート (既定: 8080)")
    ap.add_argument("--assign", choices=Hub.MODES, default="auto",
                    help="auto=接続順に自動割り当て / fixed=カメラごとの QR (管理ページで切替可)")
    ap.add_argument("--bitrate", type=float, default=16, help="iPhone 1 台あたりの送信ビットレート Mbps (既定: 16)")
    ap.add_argument("--mode", choices=["fit", "fill"], default="fit", help="fit=黒帯で全体表示 / fill=切り抜いて全面表示")
    ap.add_argument("--backend", choices=["auto", "mf", "obs", "unitycapture"], default="auto",
                    help="仮想カメラ。auto=Windows 11 仮想カメラがあればそれ、なければ OBS など (既定: auto)")
    ap.add_argument("--width", type=int, default=1920, help="OBS などを使うときの解像度の幅 (既定: 1920)")
    ap.add_argument("--height", type=int, default=1080, help="OBS などを使うときの解像度の高さ (既定: 1080)")
    ap.add_argument("--fps", type=int, default=60, help="OBS などを使うときの fps (既定: 60)")
    ap.add_argument("--no-browser", action="store_true", help="起動時に管理ページを開かない")
    args = ap.parse_args()
    if args.width % 2 or args.height % 2:
        ap.error("--width と --height は偶数にしてください")

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    for noisy in ("aioice", "aiortc"):
        logging.getLogger(noisy).setLevel(logging.WARNING)

    try:
        asyncio.run(serve(args))
    except KeyboardInterrupt:
        pass
    except PortInUseError as e:
        host_url = f"http://127.0.0.1:{args.ui_port}/"
        if is_already_running(args.ui_port):
            log.error("すでに起動しています。起動中の管理ページを開きます: %s", host_url)
            if not args.no_browser:
                webbrowser.open(host_url)
        else:
            log.error("ポート %d が他のアプリに使われています。--port / --ui-port で別のポートを指定してください", e.port)
        sys.exit(1)


if __name__ == "__main__":
    main()
