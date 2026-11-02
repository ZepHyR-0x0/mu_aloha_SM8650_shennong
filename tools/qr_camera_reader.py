from __future__ import annotations

import argparse
import ctypes
import math
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Any, Optional

try:
    import cv2
    import numpy as np
    import zxingcpp
    from PIL import Image, ImageTk
except ImportError as exc:
    DEPENDENCY_ERROR: Optional[str] = str(exc)
else:
    DEPENDENCY_ERROR = None


PREVIEW_WIDTH = 720
PREVIEW_HEIGHT = 405
DEEP_SCAN_INTERVAL_SECONDS = 0.30
PREVIEW_INTERVAL_SECONDS = 0.04

# Exposure is held at a fixed shutter time so the preview does not breathe
# between frames. 15.6 ms (1/64 s) is the usual webcam default and, unlike a
# round 20 ms, it is a value UVC cameras can actually hit: they quantise the
# shutter to whole stops and round the request up, so 20 ms silently becomes
# 31.25 ms. Override it with --exposure or the spin box.
DEFAULT_EXPOSURE_MS = 15.6
EXPOSURE_MIN_MS = 0.1
EXPOSURE_MAX_MS = 500.0
# Ceiling for the metered lock. Gain cannot be set through OpenCV on either
# Windows backend, so a shutter time is the only lever left; 66 ms keeps the
# preview above ~15 fps while still covering a normally lit room.
EXPOSURE_BUDGET_MS = 66.0
# Walk the ladder from here; a bright scene can settle on the first step.
EXPOSURE_LADDER_START_MS = 3.9
# A mean around 110 keeps QR contrast readable without pushing the shutter to
# the blur-prone end of the ladder.
EXPOSURE_TARGET_BRIGHTNESS = 110.0
# Measured on this camera: a new shutter value takes ~180 ms to reach the
# frames, so each ladder step has to be held this long before sampling.
EXPOSURE_SETTLE_SECONDS = 0.25

BG = "#f3f6fa"
TEXT = "#172033"
MUTED = "#64748b"
ACCENT = "#2563eb"
SUCCESS = "#15803d"
ERROR = "#b91c1c"
PREVIEW_BG = "#0b1220"

FONT_FAMILY = "Microsoft YaHei UI" if sys.platform == "win32" else "DejaVu Sans"


def enable_windows_dpi_awareness() -> None:
    if sys.platform != "win32":
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(2)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def show_dependency_error(detail: str) -> None:
    root = tk.Tk()
    root.withdraw()
    messagebox.showerror(
        "缺少运行依赖",
        "程序需要 opencv-python、zxing-cpp 和 Pillow。\n\n"
        "请在程序所在目录执行：\n"
        "python -m pip install -r requirements.txt\n\n"
        f"详细信息：{detail}",
    )
    root.destroy()


class ExposureControl:
    """Turn the camera's automatic exposure off and hold a fixed shutter time.

    OpenCV expresses exposure differently on every backend:

    * DirectShow - ``AUTO_EXPOSURE`` 0.25 = manual, 0.75 = auto; the exposure
      value is log2 of the shutter time in seconds (-6 means 1/64 s).
    * Media Foundation - ``AUTO_EXPOSURE`` 0 = manual; the value is the raw
      ``CameraControl_Exposure``, i.e. log2 seconds rounded to an integer.
    * V4L2 - ``AUTO_EXPOSURE`` 1 = manual, 3 = auto; the value is in units of
      100 us.

    Two driver quirks shape the verification strategy. A UVC camera may accept
    the auto-exposure write but answer ``-1`` when the flag is read back, so the
    flag cannot be used to decide success. And the raw exposure value means
    completely different things per backend, so a convention is never written
    to a camera that runs on another one - 200 is a sane 20 ms on V4L2 and
    nonsense on DirectShow.
    """

    _MANUAL = {"dshow": 0.25, "msmf": 0.0, "v4l2": 1.0}
    _AUTO = {"dshow": 0.75, "msmf": 1.0, "v4l2": 3.0}
    # UVC cameras quantise the shutter to whole stops, so a request of 20 ms
    # legitimately comes back as 31.25 ms. The tolerance only has to tell
    # "quantised" apart from "the driver ignored the write".
    _TOLERANCE = {"dshow": 0.8, "msmf": 0.8, "v4l2": 30.0}

    def __init__(
        self,
        cap: "cv2.VideoCapture",
        backend_name: str,
        exposure_ms: float,
        manual: bool,
    ) -> None:
        self._cap = cap
        self.backend_name = backend_name
        self.exposure_ms = clamp_exposure(exposure_ms)
        self.actual_ms: Optional[float] = None
        self.manual = manual
        self.supported = False
        self.confirmed = False
        self.last_message = ""

    def apply(
        self,
        exposure_ms: Optional[float] = None,
        manual: Optional[bool] = None,
    ) -> tuple[bool, str]:
        if exposure_ms is not None:
            self.exposure_ms = clamp_exposure(exposure_ms)
        if manual is not None:
            self.manual = bool(manual)

        if self.manual:
            self.supported, self.confirmed = self._lock()
            if not self.supported:
                self.last_message = "该摄像头不接受手动曝光设置，仍由相机自行调整。"
            elif self.actual_ms is None:
                self.last_message = (
                    f"已关闭自动曝光，曝光请求为 {self.exposure_ms:.1f} ms；"
                    "该驱动不支持回读，无法确认实际值。"
                )
            else:
                note = ""
                if abs(self.actual_ms - self.exposure_ms) > max(
                    0.3, self.exposure_ms * 0.05
                ):
                    note = f"（相机按档位取整，请求 {self.exposure_ms:.1f} ms）"
                self.last_message = (
                    f"自动曝光已关闭，曝光锁定在 {self.actual_ms:.1f} ms{note}。"
                )
            return self.supported, self.last_message

        self.supported, self.confirmed = self._unlock()
        self.last_message = (
            "已恢复自动曝光。" if self.supported else "该摄像头不接受自动曝光设置。"
        )
        return self.supported, self.last_message

    def meter_and_lock(
        self,
        grab: Any,
        stop_event: Optional[threading.Event] = None,
        budget_ms: float = EXPOSURE_BUDGET_MS,
    ) -> tuple[bool, str]:
        """Pick a shutter time by measurement, then freeze it.

        Asking the camera's auto exposure and copying its answer does not work
        on this hardware: the first frames after AE is switched on are still
        black, so the "settled" value comes out near the minimum and the
        preview locks to a black image. Instead the exposure ladder is walked
        from short to long and the shortest step that reaches a usable
        brightness wins, which is deterministic and independent of AE.

        The result is still frozen afterwards, so the image never drifts.
        """
        best: Optional[tuple[float, float]] = None
        for step in self._ladder(budget_ms):
            if stop_event is not None and stop_event.is_set():
                break
            self.exposure_ms = step
            self.manual = True
            self.supported, self.confirmed = self._lock()
            if not self.supported:
                return False, "该摄像头不接受手动曝光设置，仍由相机自行调整。"

            # A shutter change needs roughly 200 ms before it shows up in the
            # frames; anything read before that still carries the old
            # exposure, which is what made an earlier attempt measure a black
            # image and lock to the minimum.
            deadline = time.monotonic() + EXPOSURE_SETTLE_SECONDS
            while time.monotonic() < deadline:
                if stop_event is not None and stop_event.is_set():
                    break
                grab()
            value = self._sample(grab, 3)
            if value is None:
                continue
            if best is None or value > best[1]:
                best = (step, value)
            if value >= EXPOSURE_TARGET_BRIGHTNESS:
                break

        if best is None:
            return self.apply(manual=True)

        step, brightness = best
        if self.actual_ms is None or abs(self.actual_ms - step) > 0.01:
            self.exposure_ms = step
            self.supported, self.confirmed = self._lock()

        actual = self.actual_ms if self.actual_ms is not None else self.exposure_ms
        if brightness >= EXPOSURE_TARGET_BRIGHTNESS:
            tail = f"，画面亮度 {brightness:.0f}"
        elif step >= budget_ms * 0.95:
            tail = f"，已到曝光上限，画面仍偏暗（亮度 {brightness:.0f}）"
        else:
            tail = f"，画面亮度 {brightness:.0f}"
        self.last_message = f"已校准曝光 {actual:.1f} ms{tail}。"
        return True, self.last_message

    @staticmethod
    def _ladder(budget_ms: float) -> list[float]:
        """Whole-stop shutter ladder, the granularity UVC cameras actually offer."""
        steps: list[float] = []
        value = EXPOSURE_LADDER_START_MS
        while value <= budget_ms + 0.01:
            steps.append(round(value, 2))
            value *= 2.0
        return steps or [clamp_exposure(budget_ms)]

    def _sample(self, grab: Any, count: int) -> Optional[float]:
        values: list[float] = []
        for _ in range(count):
            ok, frame = grab()
            if ok and frame is not None:
                values.append(float(frame.mean()))
        return sum(values) / len(values) if values else None

    def _order(self) -> list[tuple[str, float, float]]:
        seconds = max(self.exposure_ms, EXPOSURE_MIN_MS) / 1000.0
        log2_seconds = math.log2(seconds)
        raw = {
            "dshow": log2_seconds,
            "msmf": float(round(log2_seconds)),
            "v4l2": self.exposure_ms * 10.0,
        }
        if self.backend_name in raw:
            names = [self.backend_name]
        elif sys.platform == "win32":
            names = ["dshow", "msmf"]
        else:
            names = ["v4l2"]
        return [(name, self._MANUAL[name], raw[name]) for name in names]

    def _lock(self) -> tuple[bool, bool]:
        for name, auto_value, raw_value in self._order():
            try:
                self._cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, auto_value)
                accepted = bool(self._cap.set(cv2.CAP_PROP_EXPOSURE, raw_value))
            except (cv2.error, ValueError, TypeError):
                continue
            if not accepted:
                continue

            self.backend_name = name
            self.actual_ms = None
            readback = self._read(cv2.CAP_PROP_EXPOSURE)
            if readback is None:
                return True, False
            self.actual_ms = self._to_ms(name, readback)
            return True, abs(readback - raw_value) <= self._TOLERANCE[name]
        return False, False

    def _unlock(self) -> tuple[bool, bool]:
        self.actual_ms = None
        names = [self.backend_name] + [
            name for name in ("dshow", "msmf", "v4l2") if name != self.backend_name
        ]
        for name in names:
            try:
                accepted = bool(self._cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, self._AUTO[name]))
            except (cv2.error, ValueError, TypeError):
                continue
            if not accepted:
                continue
            self.backend_name = name
            readback = self._read(cv2.CAP_PROP_AUTO_EXPOSURE)
            if readback is None:
                return True, False
            return True, abs(readback - self._AUTO[name]) <= 0.02
        return False, False

    @staticmethod
    def _to_ms(name: str, raw: float) -> float:
        if name == "v4l2":
            return max(raw / 10.0, 0.0)
        try:
            return 1000.0 * (2.0 ** raw)
        except OverflowError:
            return float("inf")

    def _read(self, prop: int) -> Optional[float]:
        try:
            value = float(self._cap.get(prop))
        except (cv2.error, ValueError, TypeError):
            return None
        # An unsupported auto-exposure flag reads back as -1 on DirectShow and
        # Media Foundation. The exposure value itself is legitimately negative
        # on both backends (log2 seconds), so only the flag is filtered.
        if prop == cv2.CAP_PROP_AUTO_EXPOSURE and value < 0:
            return None
        return value


def clamp_exposure(value: float) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError):
        return DEFAULT_EXPOSURE_MS
    return max(EXPOSURE_MIN_MS, min(EXPOSURE_MAX_MS, number))


def _backend_name(backend: int) -> str:
    names = {
        cv2.CAP_DSHOW: "dshow",
        cv2.CAP_MSMF: "msmf",
        cv2.CAP_V4L2: "v4l2",
    }
    return names.get(backend, "any")


class QrDecoder:
    def __init__(self) -> None:
        self._opencv = cv2.QRCodeDetector()
        self._clahe = cv2.createCLAHE(clipLimit=2.5, tileGridSize=(8, 8))

    def decode(self, frame: np.ndarray, deep: bool = False) -> Optional[tuple[bytes, str, Any]]:
        found = self._decode_frame(frame, deep=False)
        if found is not None:
            return found

        mirrored = cv2.flip(frame, 1)
        found = self._decode_frame(mirrored, deep=False)
        if found is not None:
            return self._unmirror(found, frame.shape[1])

        if not deep:
            return None

        found = self._decode_frame(frame, deep=True)
        if found is not None:
            return found

        found = self._decode_frame(mirrored, deep=True)
        if found is not None:
            return self._unmirror(found, frame.shape[1])
        return None

    @staticmethod
    def _unmirror(
        found: tuple[bytes, str, Any], width: int
    ) -> tuple[bytes, str, Any]:
        raw, text, points = found
        if points is not None:
            try:
                points = np.asarray(points, dtype=np.float32).copy()
                points[..., 0] = width - 1 - points[..., 0]
            except (ValueError, IndexError, TypeError):
                points = None
        return raw, text, points

    def _decode_frame(self, frame: np.ndarray, deep: bool) -> Optional[tuple[bytes, str, Any]]:
        found = self._decode_with_zxing(frame)
        if found is not None or not deep:
            return found

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

        try:
            data, points, _ = self._opencv.detectAndDecodeCurved(gray)
            if data:
                return data.encode("utf-8"), data, points
        except cv2.error:
            pass

        blue, green, red = cv2.split(frame)
        hsv = cv2.cvtColor(frame, cv2.COLOR_BGR2HSV)
        for channel in (red, green, blue, hsv[:, :, 1], hsv[:, :, 2]):
            found = self._decode_with_zxing(channel)
            if found is not None:
                return found

        sharpened = cv2.addWeighted(
            gray,
            1.9,
            cv2.GaussianBlur(gray, (0, 0), 1.2),
            -0.9,
            0,
        )
        found = self._decode_with_zxing(sharpened)
        if found is not None:
            return found

        equalized = self._clahe.apply(gray)
        found = self._decode_with_zxing(equalized)
        if found is not None:
            return found

        adaptive = cv2.adaptiveThreshold(
            gray,
            255,
            cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
            cv2.THRESH_BINARY,
            31,
            5,
        )
        found = self._decode_with_zxing(adaptive)
        if found is not None:
            return found

        scale = 1.6
        enlarged = cv2.resize(
            gray,
            None,
            fx=scale,
            fy=scale,
            interpolation=cv2.INTER_CUBIC,
        )
        enlarged = cv2.addWeighted(
            enlarged,
            1.8,
            cv2.GaussianBlur(enlarged, (0, 0), 1.4),
            -0.8,
            0,
        )
        return self._decode_with_zxing(enlarged, scale=scale)

    @staticmethod
    def _decode_with_zxing(
        image: np.ndarray,
        scale: float = 1.0,
    ) -> Optional[tuple[bytes, str, Any]]:
        try:
            results = zxingcpp.read_barcodes(
                image,
                formats=zxingcpp.BarcodeFormat.QRCode,
                try_rotate=True,
                try_downscale=True,
                try_invert=True,
            )
        except Exception:
            return None

        for result in results:
            if not result.valid:
                continue

            # LOGAUTO packets begin with a 0x00 index byte, so text is not a
            # reliable payload: keep the decoded bytes and use text only for
            # ordinary codes.
            try:
                raw = bytes(result.bytes)
            except Exception:
                raw = b""
            if not raw:
                continue

            points = None
            try:
                position = result.position
                points = np.array(
                    [
                        [position.top_left.x, position.top_left.y],
                        [position.top_right.x, position.top_right.y],
                        [position.bottom_right.x, position.bottom_right.y],
                        [position.bottom_left.x, position.bottom_left.y],
                    ],
                    dtype=np.float32,
                ).reshape(1, 4, 2) / scale
            except Exception:
                pass
            return raw, result.text or "", points
        return None


class LogPacketAssembler:
    """Reassemble the LOGAUTO QR stream.

    Packet 0 is  b"\\x00" + total + payload  (total includes packet 0).
    Packet n is  bytes([n]) + payload        for 1 <= n < total.

    The device advances every 500 ms and repeats nothing, so a packet can be
    missed. Packets that arrive ahead of the sequence are held back and emitted
    in order once the gap is filled; everything already emitted is remembered by
    its raw bytes, which is also what makes re-scanning a page harmless.
    """

    def __init__(self) -> None:
        self.total = 0
        self.active = False
        self.complete = False
        self._next = 0
        self._seen: set[bytes] = set()
        self._buffered: dict[int, bytes] = {}
        self._start_raw: Optional[bytes] = None

    def reset(self) -> None:
        self.total = 0
        self.active = False
        self.complete = False
        self._next = 0
        self._seen = set()
        self._buffered = {}
        self._start_raw = None

    @staticmethod
    def is_start(raw: bytes) -> bool:
        return len(raw) >= 2 and raw[0] == 0 and raw[1] >= 1

    def owns(self, raw: bytes) -> bool:
        if self.is_start(raw):
            return True
        return self.active and len(raw) >= 1 and 1 <= raw[0] < self.total

    def feed(self, raw: bytes) -> list[dict[str, Any]]:
        if self.is_start(raw):
            return self._feed_start(raw)

        if not self.active or not raw:
            return []
        index = raw[0]
        if index < 1 or index >= self.total or raw in self._seen:
            return []
        self._seen.add(raw)

        if index < self._next:
            return []
        if index > self._next:
            self._buffered[index] = raw
            return [
                {
                    "gap": True,
                    "index": index,
                    "expected": self._next,
                    "total": self.total,
                }
            ]

        events = [self._event(index, raw[1:])]
        self._next += 1
        events.extend(self._flush())
        if self.complete and not events[-1]["complete"]:
            events[-1]["complete"] = True
        return events

    def _feed_start(self, raw: bytes) -> list[dict[str, Any]]:
        #
        # The same broadcast seen again (the device re-entered LOGAUTO, or the
        # camera caught packet 0 twice): keep the state so the retry can fill
        # gaps instead of appending the whole log a second time.
        #
        if self.active and raw == self._start_raw:
            return []

        self.total = raw[1]
        self.active = True
        self.complete = False
        self._next = 1
        self._seen = {raw}
        self._buffered = {}
        self._start_raw = raw

        events = [self._event(0, raw[2:])]
        events.extend(self._flush())
        if self.complete and not events[-1]["complete"]:
            events[-1]["complete"] = True
        return events

    def _flush(self) -> list[dict[str, Any]]:
        events: list[dict[str, Any]] = []
        while self._next in self._buffered:
            raw = self._buffered.pop(self._next)
            events.append(self._event(self._next, raw[1:]))
            self._next += 1

        if self._next >= self.total:
            self.complete = True
            if events:
                events[-1]["complete"] = True
        return events

    def _event(self, index: int, payload: bytes) -> dict[str, Any]:
        return {
            "index": index,
            "total": self.total,
            "received": index + 1,
            "complete": False,
            # The boot log is ASCII; replacement keeps the widget usable if a
            # line ever carries a non-UTF-8 byte.
            "text": payload.decode("utf-8", errors="replace"),
        }


class QrCameraReader:
    def __init__(
        self,
        root: tk.Tk,
        camera_index: int = 0,
        exposure_ms: float = DEFAULT_EXPOSURE_MS,
        lock_exposure: bool = True,
        meter: bool = True,
    ) -> None:
        self.root = root
        self.camera_index = camera_index

        self._scanning = False
        self._session_active = False
        self._camera_ready = False
        self._stopping = False
        self._closing = False
        self._scan_count = 0
        self._worker: Optional[threading.Thread] = None
        self._stop_event: Optional[threading.Event] = None
        self._scan_requested: Optional[threading.Event] = None
        self._keep_camera_open = threading.Event()
        self._keep_camera_open.set()
        self._reset_packets = threading.Event()
        self._events: queue.Queue[tuple[str, Any]] = queue.Queue()

        self._exposure_lock = threading.Lock()
        self._exposure_ms = clamp_exposure(exposure_ms)
        self._exposure_manual = bool(lock_exposure)
        # None means "no explicit choice yet": the camera meters the scene and
        # locks that instead of honouring the default.
        self._exposure_user_value: Optional[float] = (
            clamp_exposure(exposure_ms) if not meter else None
        )
        self._exposure_pending: Optional[tuple[float, bool]] = None
        self._exposure_remeasure = threading.Event()

        self._frame_lock = threading.Lock()
        self._latest_frame: Optional[np.ndarray] = None
        self._preview_photo: Optional[ImageTk.PhotoImage] = None
        self.keep_open_var = tk.BooleanVar(value=True)

        # The decoder runs on its own thread so a slow scan cannot stall the
        # preview; it picks up the newest frame and never queues up a backlog.
        self._decode_lock = threading.Lock()
        self._decode_slot: Optional[np.ndarray] = None
        self._decode_wake = threading.Event()
        self._points_lock = threading.Lock()
        self._last_points: Any = None

        self._build_window()
        self._build_ui()
        self._show_placeholder(
            "摄像头预览",
            "点击「开始识别」或按空格键",
        )

        self.root.protocol("WM_DELETE_WINDOW", self._on_close)
        self.root.bind("<space>", self._on_space)
        self.root.bind("<Escape>", self._on_escape)
        self.root.after(20, self._poll)

    def _build_window(self) -> None:
        self.root.title("摄像头二维码录入")
        self.root.geometry("820x880")
        self.root.minsize(760, 720)
        self.root.configure(bg=BG)

        style = ttk.Style(self.root)
        if "clam" in style.theme_names():
            style.theme_use("clam")

        style.configure("App.TFrame", background=BG)
        style.configure(
            "Title.TLabel",
            background=BG,
            foreground=TEXT,
            font=(FONT_FAMILY, 19, "bold"),
        )
        style.configure(
            "Hint.TLabel",
            background=BG,
            foreground=MUTED,
            font=(FONT_FAMILY, 10),
        )
        style.configure(
            "Section.TLabel",
            background=BG,
            foreground=TEXT,
            font=(FONT_FAMILY, 10, "bold"),
        )
        style.configure(
            "Primary.TButton",
            background=ACCENT,
            foreground="#ffffff",
            borderwidth=0,
            relief="flat",
            padding=(22, 9),
            font=(FONT_FAMILY, 10, "bold"),
        )
        style.map(
            "Primary.TButton",
            background=[("active", "#1d4ed8"), ("pressed", "#1e40af"), ("disabled", "#93c5fd")],
            foreground=[("disabled", "#eff6ff")],
        )
        style.configure(
            "Secondary.TButton",
            background="#e5e7eb",
            foreground=TEXT,
            borderwidth=0,
            relief="flat",
            padding=(18, 9),
            font=(FONT_FAMILY, 10),
        )
        style.map(
            "Secondary.TButton",
            background=[("active", "#d1d5db"), ("pressed", "#cbd5e1")],
        )
        style.configure(
            "App.TCheckbutton",
            background=BG,
            foreground=MUTED,
            font=(FONT_FAMILY, 10),
        )
        style.map(
            "App.TCheckbutton",
            background=[("active", BG)],
            foreground=[("active", TEXT), ("disabled", MUTED)],
        )
        style.configure(
            "Exposure.TSpinbox",
            foreground=TEXT,
            fieldbackground="#ffffff",
            padding=(4, 3),
            font=(FONT_FAMILY, 10),
        )

    def _build_ui(self) -> None:
        outer = ttk.Frame(self.root, style="App.TFrame", padding=(24, 20, 24, 20))
        outer.pack(fill="both", expand=True)
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(5, weight=1)

        header = ttk.Frame(outer, style="App.TFrame")
        header.grid(row=0, column=0, sticky="ew")
        ttk.Label(header, text="摄像头二维码录入", style="Title.TLabel").pack(anchor="w")
        ttk.Label(
            header,
            text="对准二维码会自动识别并追加；LOGAUTO 日志包会按编号自动拼接。",
            style="Hint.TLabel",
        ).pack(anchor="w", pady=(4, 0))
        ttk.Label(
            header,
            text="曝光先测光再锁定：亮度与自动曝光一致，画面不再漂移。",
            style="Hint.TLabel",
        ).pack(anchor="w", pady=(2, 0))

        preview_wrap = tk.Frame(outer, bg=PREVIEW_BG, highlightthickness=0)
        preview_wrap.grid(row=1, column=0, sticky="n", pady=(14, 0))
        self.preview = tk.Canvas(
            preview_wrap,
            width=PREVIEW_WIDTH,
            height=PREVIEW_HEIGHT,
            bg=PREVIEW_BG,
            highlightthickness=0,
        )
        self.preview.pack()

        self.status_var = tk.StringVar(value="就绪。点击「开始识别」或按空格键。")
        self.status_label = tk.Label(
            outer,
            textvariable=self.status_var,
            bg=BG,
            fg=MUTED,
            font=(FONT_FAMILY, 10),
            anchor="w",
            justify="left",
            wraplength=760,
        )
        self.status_label.grid(row=2, column=0, sticky="ew", pady=(12, 0))

        controls = ttk.Frame(outer, style="App.TFrame")
        controls.grid(row=3, column=0, sticky="ew", pady=(10, 0))
        controls.columnconfigure(2, weight=1)

        self.start_button = ttk.Button(
            controls,
            text="开始识别",
            style="Primary.TButton",
            command=self._toggle_scan,
            takefocus=False,
        )
        self.start_button.grid(row=0, column=0, sticky="w")

        self.keep_open_check = ttk.Checkbutton(
            controls,
            text="停止后保持摄像头开启",
            variable=self.keep_open_var,
            command=self._on_keep_open_changed,
            style="App.TCheckbutton",
            takefocus=False,
        )
        self.keep_open_check.grid(row=0, column=1, sticky="w", padx=(16, 0))

        exposure_box = ttk.Frame(controls, style="App.TFrame")
        exposure_box.grid(row=1, column=0, columnspan=5, sticky="w", pady=(10, 0))

        self.lock_exposure_var = tk.BooleanVar(value=self._exposure_manual)
        self.lock_exposure_check = ttk.Checkbutton(
            exposure_box,
            text="锁定曝光",
            variable=self.lock_exposure_var,
            command=self._on_exposure_changed,
            style="App.TCheckbutton",
            takefocus=False,
        )
        self.lock_exposure_check.pack(side="left")

        ttk.Label(exposure_box, text="曝光", style="Hint.TLabel").pack(
            side="left", padx=(12, 4)
        )
        self.exposure_var = tk.DoubleVar(value=self._exposure_ms)
        self.exposure_spin = ttk.Spinbox(
            exposure_box,
            from_=EXPOSURE_MIN_MS,
            to=EXPOSURE_MAX_MS,
            increment=1.0,
            width=6,
            format="%.1f",
            textvariable=self.exposure_var,
            command=self._on_exposure_changed,
            style="Exposure.TSpinbox",
            justify="center",
        )
        self.exposure_spin.pack(side="left")
        self.exposure_spin.bind("<Return>", self._on_exposure_changed)
        self.exposure_spin.bind("<FocusOut>", self._on_exposure_changed)
        ttk.Label(exposure_box, text="ms", style="Hint.TLabel").pack(
            side="left", padx=(4, 0)
        )
        self.remeasure_button = ttk.Button(
            exposure_box,
            text="重新测光",
            style="Secondary.TButton",
            command=self._on_remeasure,
            takefocus=False,
        )
        self.remeasure_button.pack(side="left", padx=(10, 0))
        self._sync_exposure_widgets()

        self.copy_button = ttk.Button(
            controls,
            text="复制",
            style="Secondary.TButton",
            command=self._copy_text,
            takefocus=False,
        )
        self.copy_button.grid(row=0, column=3, sticky="e", padx=(0, 8))

        self.clear_button = ttk.Button(
            controls,
            text="清除",
            style="Secondary.TButton",
            command=self._clear_text,
            takefocus=False,
        )
        self.clear_button.grid(row=0, column=4, sticky="e")

        result_header = ttk.Frame(outer, style="App.TFrame")
        result_header.grid(row=4, column=0, sticky="ew", pady=(16, 0))
        ttk.Label(result_header, text="识别结果（可直接编辑）", style="Section.TLabel").pack(
            side="left"
        )
        ttk.Label(
            result_header,
            text="编辑时空格用于输入，Esc 退出编辑",
            style="Hint.TLabel",
        ).pack(side="right")

        text_frame = tk.Frame(outer, bg="#dbe3ec", highlightthickness=0)
        text_frame.grid(row=5, column=0, sticky="nsew", pady=(8, 0))
        text_frame.columnconfigure(0, weight=1)
        text_frame.rowconfigure(0, weight=1)

        self.text = tk.Text(
            text_frame,
            height=6,
            wrap="word",
            undo=True,
            maxundo=-1,
            bg="#ffffff",
            fg=TEXT,
            insertbackground=TEXT,
            selectbackground="#bfdbfe",
            selectforeground=TEXT,
            relief="flat",
            highlightthickness=1,
            highlightbackground="#dbe3ec",
            highlightcolor=ACCENT,
            font=(FONT_FAMILY, 11),
            padx=12,
            pady=10,
            spacing1=2,
            spacing3=2,
        )
        text_scroll = ttk.Scrollbar(text_frame, orient="vertical", command=self.text.yview)
        self.text.configure(yscrollcommand=text_scroll.set)
        self.text.grid(row=0, column=0, sticky="nsew")
        text_scroll.grid(row=0, column=1, sticky="ns")

    def _show_placeholder(self, title: str, subtitle: str = "") -> None:
        self.preview.delete("all")
        center_x = PREVIEW_WIDTH // 2
        center_y = PREVIEW_HEIGHT // 2
        offset = -14 if subtitle else 0
        self.preview.create_text(
            center_x,
            center_y + offset,
            text=title,
            fill="#94a3b8",
            font=(FONT_FAMILY, 16),
        )
        if subtitle:
            self.preview.create_text(
                center_x,
                center_y + 22,
                text=subtitle,
                fill="#64748b",
                font=(FONT_FAMILY, 10),
            )

    def _set_status(self, text: str, tone: str = "normal") -> None:
        colors = {
            "normal": MUTED,
            "active": ACCENT,
            "success": SUCCESS,
            "error": ERROR,
        }
        self.status_var.set(text)
        self.status_label.configure(fg=colors.get(tone, MUTED))

    def _on_space(self, _event: tk.Event) -> Optional[str]:
        focus = self.root.focus_get()
        editable_widgets = (tk.Text, tk.Entry, ttk.Entry, ttk.Combobox, ttk.Spinbox)
        if isinstance(focus, editable_widgets + (tk.Button, ttk.Button, ttk.Checkbutton)):
            return None
        self._toggle_scan()
        return "break"

    def _on_escape(self, _event: tk.Event) -> str:
        self.root.focus_set()
        return "break"

    def _on_keep_open_changed(self) -> None:
        if self.keep_open_var.get():
            self._keep_camera_open.set()
        else:
            self._keep_camera_open.clear()
            if (
                self._session_active
                and self._camera_ready
                and not self._scanning
                and not self._stopping
            ):
                self._begin_camera_shutdown("正在关闭摄像头…")
        self.root.focus_set()

    def _sync_exposure_widgets(self) -> None:
        manual = bool(self.lock_exposure_var.get())
        self.exposure_spin.configure(state="normal" if manual else "disabled")

    def _on_exposure_changed(self, _event: Optional[tk.Event] = None) -> Optional[str]:
        manual = bool(self.lock_exposure_var.get())
        try:
            value = clamp_exposure(self.exposure_var.get())
        except (tk.TclError, ValueError):
            value = self._exposure_ms
        self.exposure_var.set(value)
        self._sync_exposure_widgets()

        with self._exposure_lock:
            self._exposure_ms = value
            self._exposure_manual = manual
            self._exposure_user_value = value if manual else None
            self._exposure_pending = (value, manual)

        if not self._session_active:
            self._set_status(
                f"曝光将锁定在 {value:.1f} ms（相机可能按档位取整）。"
                if manual
                else "已恢复自动曝光。",
                "normal",
            )
        return None

    def _on_remeasure(self) -> None:
        if not self._session_active or not self._camera_ready:
            self._set_status("请先开始识别，测光需要摄像头取流。", "normal")
            self.root.focus_set()
            return
        self.lock_exposure_var.set(True)
        self._sync_exposure_widgets()
        with self._exposure_lock:
            self._exposure_manual = True
        self._exposure_remeasure.set()
        self._set_status("正在测光，请让画面保持正常取景…", "active")
        self.root.focus_set()

    def _toggle_scan(self) -> None:
        if self._stopping:
            return
        if self._scanning:
            self._request_stop()
            return
        self._start_scan()

    def _start_scan(self) -> None:
        if self._scanning or self._stopping or self._closing:
            return

        if self._session_active and self._camera_ready:
            if self._scan_requested is None:
                return
            self._scan_requested.set()
            self._scanning = True
            self.start_button.configure(text="停止识别", state="normal")
            self._set_status("正在自动识别，对准二维码即可…", "active")
            self.root.focus_set()
            return

        if self._session_active:
            return

        self._scanning = True
        self._session_active = True
        self._camera_ready = False
        self._stopping = False
        self._stop_event = threading.Event()
        self._scan_requested = threading.Event()
        self._scan_requested.set()
        self.start_button.configure(text="停止识别", state="normal")
        self._set_status("正在打开摄像头…", "active")
        self._show_placeholder("正在打开摄像头…", "首次启动可能需要几秒钟")
        with self._frame_lock:
            self._latest_frame = None
        self.root.focus_set()

        self._worker = threading.Thread(
            target=self._scan_worker,
            args=(self._stop_event, self._scan_requested),
            name="qr-camera-scanner",
            daemon=True,
        )
        self._worker.start()

    def _request_stop(self) -> None:
        if not self._scanning or self._stopping:
            return

        if self._scan_requested is not None:
            self._scan_requested.clear()
        self._scanning = False

        if self._camera_ready and self._keep_camera_open.is_set():
            self.start_button.configure(text="继续识别", state="normal")
            self._set_status("已停止识别，摄像头保持开启。按空格继续。", "normal")
            self.root.focus_set()
            return

        self._begin_camera_shutdown("正在停止识别并关闭摄像头…")

    def _begin_camera_shutdown(self, message: str) -> None:
        if self._stopping:
            return
        if self._scan_requested is not None:
            self._scan_requested.clear()
        self._scanning = False
        self._stopping = True
        self.start_button.configure(text="正在关闭…", state="disabled")
        self._set_status(message, "normal")
        if self._stop_event is not None:
            self._stop_event.set()

    def _open_camera(
        self, stop_event: threading.Event
    ) -> Optional[tuple[cv2.VideoCapture, ExposureControl]]:
        if sys.platform == "win32":
            backends = [cv2.CAP_DSHOW, cv2.CAP_MSMF]
        else:
            backends = []
        backends.append(cv2.CAP_ANY)

        with self._exposure_lock:
            exposure_ms = self._exposure_ms
            manual = self._exposure_manual

        for backend in dict.fromkeys(backends):
            if stop_event.is_set():
                return None
            cap = cv2.VideoCapture(self.camera_index, backend)
            if not cap.isOpened():
                cap.release()
                continue

            cap.set(cv2.CAP_PROP_FRAME_WIDTH, 1280)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, 720)
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)

            for _ in range(10):
                if stop_event.is_set():
                    cap.release()
                    return None
                ok, frame = cap.read()
                if ok and frame is not None:
                    #
                    # DirectShow resets the camera controls when the capture
                    # graph starts, so exposure is configured by the caller
                    # only after the first frame has arrived.
                    #
                    return cap, ExposureControl(
                        cap, _backend_name(backend), exposure_ms, manual
                    )
                time.sleep(0.08)
            cap.release()

        return None

    def _scan_worker(
        self,
        stop_event: threading.Event,
        scan_requested: threading.Event,
    ) -> None:
        cap: Optional[cv2.VideoCapture] = None
        control: Optional[ExposureControl] = None
        error_message: Optional[str] = None

        try:
            opened = self._open_camera(stop_event)
            if opened is None:
                if not stop_event.is_set():
                    error_message = (
                        f"无法打开摄像头 {self.camera_index}。\n\n"
                        "请确认摄像头没有被其他程序占用，并检查系统相机权限。\n"
                        "如果有多个摄像头，可尝试使用 --camera 1 启动。"
                    )
            else:
                cap, control = opened
                self._events.put(("opened", None))

                def grab() -> tuple[bool, Optional[np.ndarray]]:
                    """Read one frame and keep the preview alive while we do."""
                    ok, frame = cap.read()
                    if ok and frame is not None:
                        self._publish_frame(self._make_preview(frame, None))
                    return ok, frame

                #
                # A fixed shutter cannot be right for every room, and this
                # driver offers no gain control to make up the difference, so
                # the camera meters first and we freeze what it chose. Only an
                # explicit user value skips the measurement.
                #
                with self._exposure_lock:
                    user_value = self._exposure_user_value
                if control.manual and user_value is None:
                    self._events.put(("status", "正在校准曝光，请保持正常取景…"))
                    ok, message = control.meter_and_lock(grab, stop_event)
                    with self._exposure_lock:
                        self._exposure_ms = control.exposure_ms
                    self._events.put(("exposure-value", control.exposure_ms))
                else:
                    ok, message = control.apply()
                self._events.put(("exposure", (ok, message)))

                decoder_thread = threading.Thread(
                    target=self._decode_loop,
                    args=(stop_event, scan_requested),
                    name="qr-camera-decoder",
                    daemon=True,
                )
                decoder_thread.start()

                failed_reads = 0
                last_preview = 0.0

                while not stop_event.is_set():
                    ok, frame = grab()
                    if not ok or frame is None:
                        failed_reads += 1
                        if failed_reads >= 30:
                            error_message = "摄像头读取失败，请检查连接后重试。"
                            break
                        time.sleep(0.03)
                        continue

                    failed_reads = 0
                    now = time.monotonic()

                    with self._exposure_lock:
                        pending = self._exposure_pending
                        self._exposure_pending = None
                    if pending is not None:
                        applied, message = control.apply(*pending)
                        self._events.put(("exposure", (applied, message)))

                    if self._exposure_remeasure.is_set():
                        self._exposure_remeasure.clear()
                        applied, message = control.meter_and_lock(grab, stop_event)
                        with self._exposure_lock:
                            self._exposure_ms = control.exposure_ms
                            self._exposure_user_value = None
                        self._events.put(("exposure-value", control.exposure_ms))
                        self._events.put(("exposure", (applied, message)))

                    if now - last_preview >= PREVIEW_INTERVAL_SECONDS:
                        last_preview = now
                        self._publish_frame(
                            self._make_preview(frame, self._take_points())
                        )

                    #
                    # Hand the frame to the decoder and move on. A deep scan
                    # costs about 0.24 s; running it here held the preview to
                    # under 4 fps, which reads as a frozen camera.
                    #
                    if scan_requested.is_set():
                        with self._decode_lock:
                            self._decode_slot = frame
                        self._decode_wake.set()

                decoder_thread.join(timeout=2.0)
        except Exception as exc:
            error_message = f"扫码过程中发生错误：{exc}"
        finally:
            if cap is not None:
                cap.release()

            if not self._closing:
                if error_message is not None:
                    self._events.put(("error", error_message))
                else:
                    self._events.put(("stopped", None))

    def _take_points(self) -> Any:
        with self._points_lock:
            return self._last_points

    def _decode_loop(
        self,
        stop_event: threading.Event,
        scan_requested: threading.Event,
    ) -> None:
        """Decode the newest frame off the capture thread."""
        decoder = QrDecoder()
        packets = LogPacketAssembler()
        last_deep = 0.0
        last_emitted: Optional[bytes] = None
        was_scanning = False

        while not stop_event.is_set():
            self._decode_wake.wait(0.05)
            self._decode_wake.clear()

            with self._decode_lock:
                frame = self._decode_slot
                self._decode_slot = None
            if frame is None:
                continue

            if self._reset_packets.is_set():
                self._reset_packets.clear()
                packets.reset()

            scanning = scan_requested.is_set()
            if scanning and not was_scanning:
                last_emitted = None
                packets.reset()
                with self._points_lock:
                    self._last_points = None
            was_scanning = scanning
            if not scanning:
                continue

            now = time.monotonic()
            deep = now - last_deep >= DEEP_SCAN_INTERVAL_SECONDS
            if deep:
                last_deep = now

            result = decoder.decode(frame, deep=deep)
            if result is None:
                continue

            raw, text, points = result
            with self._points_lock:
                self._last_points = points

            if raw == last_emitted or not scan_requested.is_set():
                continue
            last_emitted = raw
            if packets.owns(raw):
                for event in packets.feed(raw):
                    self._events.put(("packet", event))
            elif raw[0] < 0x20:
                # An index-only packet whose 0x00 start was missed; appending
                # it alone would corrupt the log, so drop it but tell the user.
                self._events.put(("packet-orphan", raw[0]))
            else:
                self._events.put(
                    ("found", text or raw.decode("utf-8", errors="replace"))
                )

    def _make_preview(self, frame: np.ndarray, points: Any) -> np.ndarray:
        frame_height, frame_width = frame.shape[:2]
        scale = min(PREVIEW_WIDTH / frame_width, PREVIEW_HEIGHT / frame_height)
        resized_width = max(1, int(round(frame_width * scale)))
        resized_height = max(1, int(round(frame_height * scale)))
        resized = cv2.resize(
            frame,
            (resized_width, resized_height),
            interpolation=cv2.INTER_AREA,
        )

        left = (PREVIEW_WIDTH - resized_width) // 2
        right = PREVIEW_WIDTH - resized_width - left
        top = (PREVIEW_HEIGHT - resized_height) // 2
        bottom = PREVIEW_HEIGHT - resized_height - top
        canvas = cv2.copyMakeBorder(
            resized,
            top,
            bottom,
            left,
            right,
            cv2.BORDER_CONSTANT,
            value=(32, 18, 11),
        )

        if points is not None and len(points) > 0:
            try:
                pts = np.asarray(points).reshape(-1, 2).astype(np.float32)
                pts = pts * scale + np.array([left, top], dtype=np.float32)
                pts = np.rint(pts).astype(np.int32)
                cv2.polylines(canvas, [pts], True, (80, 220, 120), 3, cv2.LINE_AA)
                for point in pts:
                    cv2.circle(canvas, tuple(point), 4, (255, 255, 255), -1, cv2.LINE_AA)
            except (ValueError, cv2.error):
                pass

        canvas = cv2.flip(canvas, 1)
        return cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)

    def _publish_frame(self, frame: np.ndarray) -> None:
        with self._frame_lock:
            self._latest_frame = frame

    def _render_latest_frame(self) -> None:
        with self._frame_lock:
            frame = self._latest_frame
            self._latest_frame = None
        if frame is None:
            return

        self._preview_photo = ImageTk.PhotoImage(Image.fromarray(frame))
        self.preview.delete("all")
        self.preview.create_image(0, 0, image=self._preview_photo, anchor="nw")

    def _poll(self) -> None:
        if self._closing:
            return

        self._render_latest_frame()

        try:
            while True:
                event_name, payload = self._events.get_nowait()
                self._handle_event(event_name, payload)
        except queue.Empty:
            pass

        self.root.after(30, self._poll)

    def _handle_event(self, event_name: str, payload: Any) -> None:
        if self._closing:
            return

        if event_name == "opened":
            self._camera_ready = True
            if self._scanning and not self._stopping:
                self._set_status("正在自动识别，对准二维码即可…", "active")
            return

        if event_name == "status":
            self._set_status(str(payload), "active")
            return

        if event_name == "exposure-value":
            try:
                value = clamp_exposure(payload)
            except (TypeError, ValueError):
                return
            with self._exposure_lock:
                self._exposure_ms = value
            self.exposure_var.set(round(value, 1))
            return

        if event_name == "exposure":
            ok, message = payload
            self._set_status(str(message), "normal" if ok else "error")
            return

        if event_name == "found":
            if not self._scanning:
                return
            self._scan_count += 1
            self._append_text(str(payload))
            self.start_button.configure(text="停止识别", state="normal")
            self._set_status(
                f"已自动追加（本次共 {self._scan_count} 个）。继续对准下一个二维码…",
                "success",
            )
            self.root.focus_set()
            return

        if event_name == "packet-orphan":
            if not self._scanning:
                return
            self._set_status(
                f"收到日志包 {payload + 1}，但缺少带总包数的起始包；"
                "请在设备上重新进入 LOGAUTO。",
                "error",
            )
            return

        if event_name == "packet":
            if not self._scanning:
                return
            if payload.get("gap"):
                self._set_status(
                    f"收到日志包 {payload['index'] + 1}/{payload['total']}，"
                    f"等待第 {payload['expected'] + 1} 包…",
                    "active",
                )
                return

            self._scan_count += 1
            self._append_text(
                str(payload["text"]),
                new_block=(payload["index"] == 0),
            )
            self.start_button.configure(text="停止识别", state="normal")

            total = payload["total"]
            if payload["complete"]:
                self._set_status(
                    f"日志接收完成：{total} 个包已全部追加。",
                    "success",
                )
            elif payload["index"] == 0:
                self._set_status(f"开始接收日志（共 {total} 包）…", "active")
            else:
                self._set_status(
                    f"已追加日志包 {payload['received']}/{total}，继续对准…",
                    "active",
                )
            self.root.focus_set()
            return

        if event_name == "error":
            self._scanning = False
            self._session_active = False
            self._camera_ready = False
            self._stopping = False
            self._worker = None
            self.start_button.configure(
                text="继续识别" if self._scan_count else "开始识别",
                state="normal",
            )
            self._show_placeholder("摄像头不可用", "请检查摄像头后重试")
            self._set_status("扫码失败，请检查摄像头后重试。", "error")
            self.root.focus_set()
            messagebox.showerror("无法继续扫码", str(payload), parent=self.root)
            return

        if event_name == "stopped":
            self._scanning = False
            self._session_active = False
            self._camera_ready = False
            self._stopping = False
            self._worker = None
            self.start_button.configure(
                text="继续识别" if self._scan_count else "开始识别",
                state="normal",
            )
            self._show_placeholder("摄像头已关闭", "按空格或点击按钮重新开启")
            self._set_status("摄像头已关闭。按空格或点击按钮开始识别。", "normal")
            self.root.focus_set()

    def _append_text(self, content: str, new_block: bool = True) -> None:
        current = self.text.get("1.0", "end-1c")
        if new_block and current and not current.endswith("\n"):
            self.text.insert("end", "\n")
        self.text.insert("end", content)
        self.text.see("end")

    def _copy_text(self) -> None:
        content = self.text.get("1.0", "end-1c")
        if not content:
            self._set_status("编辑框为空，没有可复制的内容。", "normal")
            self.root.focus_set()
            return

        self.root.clipboard_clear()
        self.root.clipboard_append(content)
        self._set_status(f"已复制 {len(content)} 个字符到剪贴板。", "success")
        self.root.focus_set()

    def _clear_text(self) -> None:
        self.text.delete("1.0", "end")
        self._scan_count = 0
        self._reset_packets.set()
        if not self._scanning and not self._stopping:
            self.start_button.configure(text="开始识别")
        self._set_status("已清空编辑框。按空格可开始识别。", "normal")
        self.root.focus_set()

    def _on_close(self) -> None:
        if self._closing:
            return
        self._closing = True
        if self._stop_event is not None:
            self._stop_event.set()
        if self._worker is not None and self._worker.is_alive():
            self._worker.join(timeout=2.0)
        self.root.destroy()


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="通过摄像头逐个识别二维码并追加到编辑框")
    parser.add_argument(
        "--camera",
        type=int,
        default=0,
        help="摄像头编号，默认为 0",
    )
    parser.add_argument(
        "--exposure",
        type=float,
        default=DEFAULT_EXPOSURE_MS,
        metavar="毫秒",
        help=f"直接指定曝光时间（跳过测光），默认 {DEFAULT_EXPOSURE_MS:g}",
    )
    parser.add_argument(
        "--auto-exposure",
        action="store_true",
        help="保留摄像头的自动曝光（默认先测光再锁定）",
    )
    parser.add_argument(
        "--no-meter",
        action="store_true",
        help="不测光，直接用 --exposure 的值锁定",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if DEPENDENCY_ERROR is not None:
        show_dependency_error(DEPENDENCY_ERROR)
        return 1

    enable_windows_dpi_awareness()
    root = tk.Tk()
    QrCameraReader(
        root,
        camera_index=args.camera,
        exposure_ms=args.exposure,
        lock_exposure=not args.auto_exposure,
        meter=not args.no_meter,
    )
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
