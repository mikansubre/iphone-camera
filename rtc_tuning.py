"""aiortc の受信設定を LAN 内の高画質・低遅延向けに調整する。

aiortc の既定値はインターネット越しの汎用的な通話向けで、そのまま iPhone の 1080p60 を
受けると次の理由で画質と遅延が大きく悪化する:

- 受信側帯域推定 (REMB) が Python のイベントループの揺らぎを回線混雑と誤認し、
  送信側のビットレートを絞る → iPhone が解像度・fps を落とす
- H.264 を Constrained Baseline / Level 3.1 (720p30 相当) としか宣言しない
- ジッタバッファが「次のフレームの到着」を待ってから出力する (常に 1 フレーム遅れる)
- ジッタバッファが 128 パケットしかなく、1080p のキーフレームが収まらない
  → 破棄してキーフレーム再要求、を繰り返す
"""

from __future__ import annotations

import copy
import time

import aiortc.codecs as codecs
import aiortc.rtcrtpreceiver as rtcrtpreceiver
from aiortc.codecs.h264 import H264Decoder
from aiortc.jitterbuffer import JitterBuffer, JitterFrame
from av.codec.context import Flags

ABS_SEND_TIME = "http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time"
H264_LEVEL = 0x34  # Level 5.2: 1080p120 / 4K30 まで受け取れると宣言する
H264_PROFILES = ("640c", "6400", "42e0", "4200")  # Constrained High, High, Constrained Baseline, Baseline
VIDEO_BUFFER_PACKETS = 2048
STALL_SEC = 0.25  # この間フレームが完成しなければ欠落とみなしてキーフレームを要求する


class RxStats:
    """1 本の映像ストリーム (SSRC) の受信ビットレートと欠落回数。イベントループのスレッドからのみ更新される。"""

    def __init__(self) -> None:
        self.bytes = 0
        self.stalls = 0
        self._mark_bytes = 0
        self._mark_time = time.monotonic()
        self.mbps = 0.0

    def update_rate(self) -> float:
        now = time.monotonic()
        if now - self._mark_time >= 0.5:
            self.mbps = (self.bytes - self._mark_bytes) * 8 / (now - self._mark_time) / 1e6
            self._mark_bytes, self._mark_time = self.bytes, now
        return self.mbps


_rx_stats: dict[int, RxStats] = {}


def rx_stats(ssrc: int) -> RxStats:
    stats = _rx_stats.get(ssrc)
    if stats is None:
        stats = _rx_stats[ssrc] = RxStats()
    return stats


def forget_stats(ssrcs: list[int]) -> None:
    for ssrc in ssrcs:
        _rx_stats.pop(ssrc, None)


class LowLatencyJitterBuffer(JitterBuffer):
    """フレームの最終パケット (marker bit) が揃った時点でフレームを出すジッタバッファ。"""

    def __init__(self, capacity: int, prefetch: int = 0, is_video: bool = False) -> None:
        super().__init__(VIDEO_BUFFER_PACKETS if is_video else capacity, prefetch=prefetch, is_video=is_video)
        # origin から連続して届いている、まだ完成していないフレームのパケット
        self._partial: list = []
        self._waiting_since: float | None = None

    def add(self, packet):
        if not self._is_video:
            return super().add(packet)

        stats = rx_stats(packet.ssrc)
        stats.bytes += len(packet.payload)
        pli, frame = super().add(packet)
        now = time.monotonic()
        if frame is not None or pli:
            self._waiting_since = None
        elif self._waiting_since is None:
            self._waiting_since = now
        elif now - self._waiting_since > STALL_SEC:
            # 再送でも埋まらない欠落。溜まった分を捨ててキーフレームからやり直す
            self._packets = [None] * self._capacity
            self._origin = None
            self._partial = []
            self._waiting_since = None
            stats.stalls += 1
            return True, None
        return pli, frame

    def _remove_frame(self, sequence_number: int) -> JitterFrame | None:
        if not self._is_video:
            return super()._remove_frame(sequence_number)

        packets = self._partial
        timestamp = packets[0].timestamp if packets else None
        for count in range(len(packets), self._capacity):
            packet = self._packets[(self._origin + count) % self._capacity]
            if packet is None:
                return None
            if timestamp is None:
                timestamp = packet.timestamp
            elif packet.timestamp != timestamp:
                # marker bit が無いまま次のフレームが始まった
                return self._pop_frame(count, packets)
            packets.append(packet)
            if packet.marker:
                return self._pop_frame(count + 1, packets)
        return None

    def _pop_frame(self, count: int, packets: list) -> JitterFrame:
        frame = JitterFrame(data=b"".join(p._data for p in packets), timestamp=packets[0].timestamp)
        self.remove(count)
        return frame

    def remove(self, count: int) -> None:
        self._partial = []
        super().remove(count)

    def smart_remove(self, count: int) -> bool:
        self._partial = []
        return super().smart_remove(count)


def _low_delay_decoder_init(self: H264Decoder, _orig=H264Decoder.__init__) -> None:
    _orig(self)
    self.codec.flags |= int(Flags.low_delay)


def apply(max_kbps: int) -> None:
    """aiortc に調整を適用する。RTCPeerConnection を作る前に一度だけ呼ぶ。"""
    video = codecs.CODECS["video"]

    # 帯域推定 (REMB) をやめる。iPhone 側は指定ビットレートで送り、パケットロスでのみ下げる
    codecs.HEADER_EXTENSIONS["video"] = [e for e in codecs.HEADER_EXTENSIONS["video"] if e.uri != ABS_SEND_TIME]
    for codec in video:
        codec.rtcpFeedback = [fb for fb in codec.rtcpFeedback if fb.type != "goog-remb"]

    # H.264 は High プロファイルも受け付け、レベルを引き上げる
    base = next(c for c in video if c.mimeType == "video/H264")
    rtx = next(c for c in video if c.mimeType == "video/rtx")
    next_pt = max(c.payloadType for c in video) + 1
    h264_pts = {c.payloadType for c in video if c.mimeType == "video/H264"}
    video[:] = [c for c in video if c.payloadType not in h264_pts and c.parameters.get("apt") not in h264_pts]
    for profile in H264_PROFILES:
        h264 = copy.deepcopy(base)
        h264.payloadType = next_pt
        h264.parameters["profile-level-id"] = f"{profile}{H264_LEVEL:02x}"
        h264_rtx = copy.deepcopy(rtx)
        h264_rtx.payloadType = next_pt + 1
        h264_rtx.parameters = {"apt": next_pt}
        video += [h264, h264_rtx]
        next_pt += 2

    # 送信側 (libwebrtc) に最初から高いビットレートで送らせる
    for codec in video:
        if codec.mimeType != "video/rtx":
            codec.parameters.update({
                "x-google-start-bitrate": max_kbps,
                "x-google-min-bitrate": max_kbps // 4,
                "x-google-max-bitrate": max_kbps,
            })

    rtcrtpreceiver.JitterBuffer = LowLatencyJitterBuffer
    H264Decoder.__init__ = _low_delay_decoder_init


def add_bandwidth(sdp: str, max_kbps: int) -> str:
    """回答 SDP の映像セクションに b=AS (受信できる最大ビットレート) を入れる。"""
    out, in_video = [], False
    for line in sdp.splitlines():
        if line.startswith("m="):
            in_video = line.startswith("m=video")
        if line.startswith("b=") and in_video:
            continue
        out.append(line)
        if in_video and line.startswith("c="):
            out.append(f"b=AS:{max_kbps}")
    return "\r\n".join(out) + "\r\n"
