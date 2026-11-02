"""Real-time video ingest and frame pipeline for Mechanical TV.

Supports live HDMI capture via V4L2 (USB UVC dongles and TC358743 HDMI-to-CSI-2
bridges) through FFmpeg or OpenCV, procedural test patterns for optical and motor
calibration, and streaming pre-converted library clips.
Maps frames to the 32x25 Nipkow disk spiral geometry.
"""

from abc import ABC, abstractmethod
from collections import deque
import math
import os
from pathlib import Path
import re
import select
import shutil
import subprocess
import threading
import time
from typing import Dict, List, Optional, Union

try:
    import cv2
    import numpy as np
    HAS_CV2 = True
except ImportError:
    HAS_CV2 = False
    cv2 = None
    np = None

WIDTH = 32
HEIGHT = 25
FRAME_PIXELS = WIDTH * HEIGHT  # 800 pixels


def build_gamma_lut(gamma: float = 1.8, contrast: float = 1.0) -> bytes:
    """Precompute a 256-byte gamma and contrast lookup table."""
    lut = bytearray(256)
    gamma = max(0.1, min(4.0, gamma))
    contrast = max(0.1, min(3.0, contrast))
    for i in range(256):
        # Apply contrast centered at midpoint 128
        c_val = (i - 128.0) * contrast + 128.0
        c_val = max(0.0, min(255.0, c_val))
        val = int(round(255.0 * ((c_val / 255.0) ** gamma)))
        lut[i] = max(0, min(255, val))
    return bytes(lut)


def resize_and_grayscale_pure(
    raw_bytes: bytes, in_w: int, in_h: int, fit: str = "fit"
) -> bytes:
    """Fallback pure-Python resizer and grayscale converter with aspect ratio handling."""
    out = bytearray(FRAME_PIXELS)
    if in_w <= 0 or in_h <= 0 or not raw_bytes:
        return bytes(out)

    is_rgb = len(raw_bytes) >= in_w * in_h * 3
    bpp = 3 if is_rgb else 1

    if fit == "crop":
        target_aspect = WIDTH / HEIGHT
        in_aspect = in_w / in_h
        if in_aspect > target_aspect:
            crop_w = int(in_h * target_aspect)
            crop_h = in_h
            x_offset = (in_w - crop_w) // 2
            y_offset = 0
        else:
            crop_w = in_w
            crop_h = int(in_w / target_aspect)
            x_offset = 0
            y_offset = (in_h - crop_h) // 2

        scale_x = crop_w / WIDTH
        scale_y = crop_h / HEIGHT

        for y in range(HEIGHT):
            src_y = min(in_h - 1, int(y_offset + y * scale_y))
            row_offset = src_y * in_w * bpp
            out_row = y * WIDTH
            for x in range(WIDTH):
                src_x = min(in_w - 1, int(x_offset + x * scale_x))
                src_idx = row_offset + src_x * bpp
                if is_rgb:
                    gray = (raw_bytes[src_idx] * 77 + raw_bytes[src_idx + 1] * 150 + raw_bytes[src_idx + 2] * 29) >> 8
                else:
                    gray = raw_bytes[src_idx]
                out[out_row + x] = gray
    else:
        # "fit": letterbox / pillarbox with black borders
        target_aspect = WIDTH / HEIGHT
        in_aspect = in_w / in_h
        if in_aspect > target_aspect:
            scaled_w = WIDTH
            scaled_h = max(1, int(round(WIDTH / in_aspect)))
            pad_top = (HEIGHT - scaled_h) // 2
            pad_left = 0
        else:
            scaled_h = HEIGHT
            scaled_w = max(1, int(round(HEIGHT * in_aspect)))
            pad_top = 0
            pad_left = (WIDTH - scaled_w) // 2

        scale_x = in_w / scaled_w
        scale_y = in_h / scaled_h

        for y in range(scaled_h):
            dst_y = pad_top + y
            if 0 <= dst_y < HEIGHT:
                src_y = min(in_h - 1, int(y * scale_y))
                row_offset = src_y * in_w * bpp
                out_row = dst_y * WIDTH
                for x in range(scaled_w):
                    dst_x = pad_left + x
                    if 0 <= dst_x < WIDTH:
                        src_x = min(in_w - 1, int(x * scale_x))
                        src_idx = row_offset + src_x * bpp
                        if is_rgb:
                            gray = (raw_bytes[src_idx] * 77 + raw_bytes[src_idx + 1] * 150 + raw_bytes[src_idx + 2] * 29) >> 8
                        else:
                            gray = raw_bytes[src_idx]
                        out[out_row + dst_x] = gray

    return bytes(out)


class VideoSource(ABC):
    """Abstract base class for real-time video inputs."""

    @abstractmethod
    def start(self) -> None:
        """Start capturing or generating frames."""
        pass

    @abstractmethod
    def stop(self) -> None:
        """Stop frame source."""
        pass

    @abstractmethod
    def get_frame(self) -> Optional[bytes]:
        """Return the newest 32x25 8-bit grayscale frame (800 bytes) or None."""
        pass

    @abstractmethod
    def get_telemetry(self) -> Dict[str, object]:
        """Return status information for UI/diagnostics."""
        pass


class TestPatternGenerator(VideoSource):
    """Generates procedural calibration and test patterns for mechanical TV."""

    __test__ = False

    PATTERNS = [
        "smpte_bars",
        "grayscale_ramp",
        "vertical_ramp",
        "grid",
        "rotating_bar",
        "pulse_test",
        "checkerboard",
        "single_column",
        "single_row",
        "diagonal_line",
    ]

    def __init__(self, pattern: str = "smpte_bars"):
        self.pattern = pattern if pattern in self.PATTERNS else "smpte_bars"
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._frame = bytes(FRAME_PIXELS)
        self._lock = threading.Lock()
        self._start_time = time.monotonic()
        self._fps = 10.0
        self._frame_count = 0
        self.update_pattern(self.pattern)

    def set_pattern(self, name: str) -> None:
        if name in self.PATTERNS:
            self.pattern = name

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._start_time = time.monotonic()
        self._thread = threading.Thread(target=self._loop, daemon=True, name="test-pattern-worker")
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None

    def _loop(self) -> None:
        interval = 1.0 / self._fps
        while self._running:
            t0 = time.monotonic()
            frame = self.generate_frame(t0 - self._start_time)
            with self._lock:
                self._frame = frame
                self._frame_count += 1
            elapsed = time.monotonic() - t0
            sleep_time = max(0.001, interval - elapsed)
            time.sleep(sleep_time)

    def generate_frame(self, t: float) -> bytes:
        buf = bytearray(FRAME_PIXELS)
        pat = self.pattern

        if pat == "smpte_bars":
            # 8 vertical bars of descending intensity across 32 columns
            # 32 / 8 = 4 columns per bar
            levels = [255, 219, 182, 146, 109, 73, 36, 0]
            for y in range(HEIGHT):
                row = y * WIDTH
                for x in range(WIDTH):
                    bar_idx = min(7, x // 4)
                    buf[row + x] = levels[bar_idx]

        elif pat == "grayscale_ramp":
            # Smooth gradient left to right (0 to 255)
            for y in range(HEIGHT):
                row = y * WIDTH
                for x in range(WIDTH):
                    buf[row + x] = int(round(x * 255.0 / (WIDTH - 1)))

        elif pat == "vertical_ramp":
            # Smooth gradient top to bottom (0 to 255)
            for y in range(HEIGHT):
                val = int(round(y * 255.0 / (HEIGHT - 1)))
                row = y * WIDTH
                for x in range(WIDTH):
                    buf[row + x] = val

        elif pat == "grid":
            # 1-pixel outer border, center crosshair, plus diagonal ticks
            cx = WIDTH // 2
            cy = HEIGHT // 2
            for y in range(HEIGHT):
                row = y * WIDTH
                for x in range(WIDTH):
                    is_border = (x == 0 or x == WIDTH - 1 or y == 0 or y == HEIGHT - 1)
                    is_cross = (x == cx or y == cy)
                    buf[row + x] = 255 if (is_border or is_cross) else 0

        elif pat == "rotating_bar":
            # Rotating clock-hand line around center (16, 12)
            # Angular speed: 0.5 revolutions per second
            angle = (t * 0.5 * 2.0 * math.pi) % (2.0 * math.pi)
            cx, cy = (WIDTH - 1) / 2.0, (HEIGHT - 1) / 2.0
            cos_a = math.cos(angle)
            sin_a = math.sin(angle)
            for y in range(HEIGHT):
                row = y * WIDTH
                dy = y - cy
                for x in range(WIDTH):
                    dx = x - cx
                    # Perpendicular distance to the line
                    dist = abs(dx * sin_a - dy * cos_a)
                    # Dot product along the line
                    along = dx * cos_a + dy * sin_a
                    if dist <= 0.8 and along >= -1.0:
                        buf[row + x] = 255
                    elif dist <= 1.5 and along >= -1.0:
                        buf[row + x] = 120
                    else:
                        buf[row + x] = 10

        elif pat == "pulse_test":
            # Flashing full black and full white (1 Hz square wave)
            val = 255 if int(t * 2.0) % 2 == 0 else 0
            for i in range(FRAME_PIXELS):
                buf[i] = val

        elif pat == "checkerboard":
            # 4x4 pixel checkerboard
            for y in range(HEIGHT):
                row = y * WIDTH
                for x in range(WIDTH):
                    bx = x // 4
                    by = y // 4
                    buf[row + x] = 255 if (bx + by) % 2 == 0 else 0

        elif pat == "single_column":
            # A single 1-pixel column scanning across columns
            active_col = int(t * 4.0) % WIDTH
            for y in range(HEIGHT):
                buf[y * WIDTH + active_col] = 255

        elif pat == "single_row":
            # A single 1-pixel row scanning down lines
            active_row = int(t * 4.0) % HEIGHT
            row_start = active_row * WIDTH
            for x in range(WIDTH):
                buf[row_start + x] = 255

        elif pat == "diagonal_line":
            # Moving diagonal line verifying spiral raster linearity
            shift = int(t * 8.0) % (WIDTH + HEIGHT)
            for y in range(HEIGHT):
                for x in range(WIDTH):
                    if (x + y) % (WIDTH // 2) == (shift % (WIDTH // 2)):
                        buf[y * WIDTH + x] = 255

        else:
            # Fallback blank
            pass

        return bytes(buf)

    def update_pattern(self, pattern: str) -> None:
        self.set_pattern(pattern)
        with self._lock:
            self._frame = self.generate_frame(0.0)

    def get_frame(self) -> Optional[bytes]:
        with self._lock:
            return self._frame

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "type": "pattern",
            "pattern": self.pattern,
            "running": self._running,
            "frames_generated": self._frame_count,
            "connected": True,
        }


V4L2_SYSFS = Path("/sys/class/video4linux")
PACKAGED_EDID = Path(__file__).resolve().parent / "edid" / "1080p30.txt"
# V4L2 node names of the Pi's CSI receivers (Pi 4 unicam, Pi 5 rp1-cfe) and the bridge chip.
CSI_RECEIVER_NAMES = ("unicam", "rp1-cfe", "tc358743")


def detect_csi_bridge(device: Union[str, int], sysfs: Path = V4L2_SYSFS) -> bool:
    """True when the V4L2 node is the Pi's CSI receiver rather than a USB UVC dongle.

    An HDMI-to-CSI-2 adapter (Toshiba TC358743) sits behind that receiver and needs an EDID
    and DV timings before it streams. USB dongles report their own product name here and
    return False. Off the Pi there is no sysfs entry, so this is False as well.
    """
    try:
        node = os.path.basename(os.path.realpath(str(device)))
        name = (sysfs / node / "name").read_text().strip().lower()
    except (OSError, ValueError):
        return False
    return any(tag in name for tag in CSI_RECEIVER_NAMES)


def parse_dv_timings(text: str):
    """Extract (width, height, fps) from `v4l2-ctl --query-dv-timings` output."""
    width = re.search(r"Active width:\s*(\d+)", text)
    height = re.search(r"Active height:\s*(\d+)", text)
    fps = re.search(r"\(([\d.]+)\s*frames per second\)", text)
    return (
        int(width.group(1)) if width else 0,
        int(height.group(1)) if height else 0,
        float(fps.group(1)) if fps else 0.0,
    )


def build_ffmpeg_command(device: str, fit: str = "fit", input_format: str = "mjpeg", ffmpeg: str = "ffmpeg") -> List[str]:
    """FFmpeg argv: V4L2 in, 10 fps 32x25 8-bit grayscale raw frames on stdout.

    input_format is a V4L2 pixel/codec name: "mjpeg" for USB dongles, "uyvy422" for the
    TC358743 CSI bridge, or "none" to let FFmpeg negotiate.
    """
    if fit == "crop":
        vf = (f"fps=10,scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase,"
              f"crop={WIDTH}:{HEIGHT},setsar=1,format=gray")
    else:
        vf = (f"fps=10,scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,"
              f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2,setsar=1,format=gray")
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-nostdin", "-f", "v4l2"]
    if input_format and input_format.lower() not in ("auto", "none"):
        cmd += ["-input_format", input_format]
    cmd += ["-i", str(device), "-vf", vf, "-pix_fmt", "gray", "-f", "rawvideo", "pipe:1"]
    return cmd


def downscale_bgr(frame, fit: str = "fit"):
    """OpenCV path: BGR frame -> 800-byte 32x25 grayscale with fit (letterbox) or crop."""
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    h, w = gray.shape
    target_aspect = WIDTH / HEIGHT
    current_aspect = w / h if h > 0 else target_aspect
    if fit == "crop":
        if current_aspect > target_aspect:
            new_w = int(h * target_aspect)
            x0 = (w - new_w) // 2
            gray = gray[:, x0:x0 + new_w]
        else:
            new_h = int(w / target_aspect)
            y0 = (h - new_h) // 2
            gray = gray[y0:y0 + new_h, :]
        return cv2.resize(gray, (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA).tobytes()
    resized = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
    if current_aspect > target_aspect:
        new_h = max(1, int(round(WIDTH / current_aspect)))
        small = cv2.resize(gray, (WIDTH, new_h), interpolation=cv2.INTER_AREA)
        y0 = (HEIGHT - new_h) // 2
        resized[y0:y0 + new_h, :] = small
    else:
        new_w = max(1, int(round(HEIGHT * current_aspect)))
        small = cv2.resize(gray, (new_w, HEIGHT), interpolation=cv2.INTER_AREA)
        x0 = (WIDTH - new_w) // 2
        resized[:, x0:x0 + new_w] = small
    return resized.tobytes()


def _last_line(text: str) -> str:
    lines = [line.strip() for line in (text or "").splitlines() if line.strip()]
    return lines[-1] if lines else ""


class HDMICapture(VideoSource):
    """Live HDMI ingest from a V4L2 device via FFmpeg (default) or OpenCV.

    Two adapter families are supported:

    * USB UVC capture dongles (MacroSilicon MS2109 class): plug and play, MJPEG by default.
    * HDMI-to-CSI-2 bridges (Toshiba TC358743: GeeekPi/52Pi, Geekworm, Waveshare, Auvidea
      B101) behind the Pi's CSI receiver. They present no EDID of their own, so the HDMI
      source sees no display until one is loaded, and they stream raw UYVY, never MJPEG.
      This class loads the packaged EDID once per service run, then before every (re)open
      locks the detected DV timings with v4l2-ctl and opens FFmpeg in UYVY.

    Failures are never silent: a dead FFmpeg, a stalled stream or a missing signal clears the
    last frame (the disk goes dark instead of freezing) and is reported in telemetry and the
    journal. The worker thread owns the capture process: each start() is a new generation,
    and a worker from an older generation can neither register a process nor release its
    successor's, so stop()/start() races cannot orphan ffmpeg or reuse a stale pipe.
    """

    STALL_TIMEOUT = 5.0       # seconds without a complete frame before the stream is reopened
    RETRY_DELAY = 1.0         # seconds between open attempts
    V4L2_TIMEOUT = 15.0       # seconds allowed per v4l2-ctl EDID/timing call
    STOP_JOIN_TIMEOUT = 3.0   # stop() waits this long for the worker; a slower worker exits on its own

    def __init__(
        self,
        device: Union[str, int] = "/dev/video0",
        fit: str = "fit",
        input_format: str = "auto",
        edid: Optional[str] = "auto",
        backend: str = "ffmpeg",
        bridge: str = "auto",
    ):
        if isinstance(device, int):
            device = f"/dev/video{device}"
        self.device = str(device)
        self.fit = fit if fit in ("fit", "crop") else "fit"
        self.input_format_setting = ((input_format or "").strip().lower()) or "auto"
        self.edid_setting = ((str(edid) if edid is not None else "").strip()) or "auto"
        self.backend = ((backend or "").strip().lower()) or "ffmpeg"
        self.bridge_setting = ((bridge or "").strip().lower()) or "auto"
        self._bridge = self.bridge_setting if self.bridge_setting in ("csi", "usb") else ""
        self._running = False
        self._generation = 0
        self._thread: Optional[threading.Thread] = None
        self._frame: Optional[bytes] = None
        self._lock = threading.Lock()         # guards _frame / _frame_count
        self._state_lock = threading.Lock()   # guards the _cap / _ffmpeg_proc / _connected handoff
        self._cap = None
        self._ffmpeg_proc: Optional[subprocess.Popen] = None
        self._stderr_tail: deque = deque(maxlen=12)
        self._connected = False
        self._opened_at = 0.0
        self._input_width = 0
        self._input_height = 0
        self._input_fps = 0.0
        self._frame_count = 0
        self._read_failures = 0
        self._last_frame_at = 0.0
        self._error = ""
        self._edid_loaded = False
        self._last_log = ""
        self._v4l2ctl = shutil.which("v4l2-ctl")
        self._ffmpeg = shutil.which("ffmpeg") or "ffmpeg"

    # -- configuration -------------------------------------------------------

    @property
    def bridge(self) -> str:
        """'csi' for a TC358743 behind the Pi CSI receiver, 'usb' for a UVC dongle."""
        if not self._bridge:
            self._bridge = "csi" if detect_csi_bridge(self.device) else "usb"
        return self._bridge

    @property
    def input_format(self) -> str:
        if self.input_format_setting != "auto":
            return self.input_format_setting
        return "uyvy422" if self.bridge == "csi" else "mjpeg"

    @property
    def edid_path(self) -> str:
        setting = self.edid_setting
        if setting.lower() in ("none", "off"):
            return ""
        if setting.lower() == "auto":
            return str(PACKAGED_EDID) if self.bridge == "csi" else ""
        return setting

    # -- lifecycle -----------------------------------------------------------

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._generation += 1
        self._thread = threading.Thread(
            target=self._capture_loop, args=(self._generation,), daemon=True, name="hdmi-capture-worker",
        )
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        thread = self._thread
        if thread and thread.is_alive() and thread is not threading.current_thread():
            thread.join(timeout=self.STOP_JOIN_TIMEOUT)
        self._thread = None
        self._cleanup()

    def _current(self, generation: int) -> bool:
        return self._running and self._generation == generation

    def _log(self, message: str) -> None:
        if message and message != self._last_log:
            self._last_log = message
            try:
                print(f"HDMI capture: {message}", flush=True)
            except OSError:
                pass  # journald stream gone; never let logging kill the worker

    def _sleep(self, seconds: float) -> None:
        deadline = time.monotonic() + seconds
        while self._running and time.monotonic() < deadline:
            time.sleep(0.05)

    @staticmethod
    def _release(cap, proc: Optional[subprocess.Popen]) -> None:
        if cap is not None:
            try:
                cap.release()
            except Exception:
                pass
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=1.0)
            except Exception:
                try:
                    proc.kill()
                    proc.wait(timeout=1.0)
                except Exception:
                    pass
            for stream in (proc.stdout, proc.stderr):
                try:
                    stream.close()
                except Exception:
                    pass

    def _cleanup(self, generation: Optional[int] = None) -> None:
        """Release the device. A worker passes its generation so a stale worker cannot kill its successor."""
        with self._state_lock:
            if generation is not None and generation != self._generation:
                return
            cap, self._cap = self._cap, None
            proc, self._ffmpeg_proc = self._ffmpeg_proc, None
            self._connected = False
            with self._lock:
                self._frame = None
        self._release(cap, proc)

    def _register(self, generation: int, cap=None, proc: Optional[subprocess.Popen] = None) -> bool:
        """Publish a freshly opened device; refused when stop() ran or a newer worker exists."""
        with self._state_lock:
            if not self._current(generation):
                return False
            self._cap, self._ffmpeg_proc = cap, proc
            self._connected = True
            self._opened_at = time.monotonic()
            self._error = ""
        return True

    def _fail(self, generation: int, message: str) -> None:
        if generation != self._generation:
            return
        self._cleanup(generation)   # clear frame/connection first so telemetry never pairs a new error with an old frame
        self._error = message
        self._log(message)
        self._sleep(self.RETRY_DELAY)

    # -- device preparation --------------------------------------------------

    def _v4l2(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self._v4l2ctl, "-d", self.device, *args],
            capture_output=True, text=True, timeout=self.V4L2_TIMEOUT,
        )

    def _prepare_csi(self) -> bool:
        """Load the EDID once and lock DV timings. False, with the reason, until a source is present."""
        if not self._v4l2ctl:
            self._error = "v4l2-ctl missing: install v4l-utils to configure the HDMI-to-CSI bridge"
            return False
        try:
            edid = self.edid_path
            if edid and not self._edid_loaded:
                if not os.path.exists(edid):
                    self._error = f"EDID file not found: {edid}"
                    return False
                result = self._v4l2("--set-edid", f"file={edid}")
                if result.returncode != 0:
                    detail = _last_line(result.stderr) or _last_line(result.stdout)
                    self._error = (f"EDID load failed: {detail} (is {self.device} the HDMI bridge "
                                   "and not a Pi camera?)")
                    return False
                self._edid_loaded = True
                self._log(f"EDID {edid} loaded; the HDMI source should now detect a display")
            result = self._v4l2("--set-dv-bt-timings", "query")
            if result.returncode != 0:
                state = "loaded" if self._edid_loaded else "not loaded"
                detail = _last_line(result.stderr) or _last_line(result.stdout)
                self._error = ("No HDMI signal on the CSI bridge: connect the source and check that it "
                               f"shows a second display (EDID {state}; v4l2-ctl: {detail or 'no detail'})")
                return False
            info = self._v4l2("--query-dv-timings")
            self._input_width, self._input_height, self._input_fps = parse_dv_timings(info.stdout)
            if self._input_height >= 1080 and self._input_fps > 50:
                self._log(f"{self._input_width}x{self._input_height}@{self._input_fps:.0f} exceeds the "
                          "Pi 4 2-lane CSI limit; expect errors. Use the shipped EDID.")
            return True
        except (OSError, subprocess.SubprocessError) as exc:
            self._error = f"v4l2-ctl failed: {exc}"
            return False

    def _open_device(self, generation: int) -> bool:
        self._cleanup(generation)
        if self.bridge_setting == "auto":
            self._bridge = "csi" if detect_csi_bridge(self.device) else "usb"
        if not os.path.exists(self.device):
            self._error = (f"Capture device {self.device} not found: USB dongle unplugged, or for an "
                           "HDMI-to-CSI adapter dtoverlay=tc358743 is missing from config.txt")
            return False
        if self.bridge == "csi" and not self._prepare_csi():
            return False
        if not self._current(generation):
            return False  # stop() ran while v4l2-ctl was working; do not open a device nobody owns
        if self.backend == "opencv":
            if HAS_CV2:
                if self._open_opencv(generation):
                    return True
            else:
                self._log("OpenCV backend requested but cv2 is not importable; using ffmpeg")
        return self._open_ffmpeg(generation)

    def _open_opencv(self, generation: int) -> bool:
        try:
            index: Union[str, int] = self.device
            if self.device.startswith("/dev/video"):
                try:
                    index = int(self.device[len("/dev/video"):])
                except ValueError:
                    pass
            cap = None
            if hasattr(cv2, "CAP_V4L2"):
                cap = cv2.VideoCapture(index, cv2.CAP_V4L2)
            if cap is None or not cap.isOpened():
                cap = cv2.VideoCapture(index)
            if not cap.isOpened():
                self._error = f"OpenCV could not open {self.device}"
                return False
            cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # minimal buffer for lowest latency
            self._input_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or self._input_width)
            self._input_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or self._input_height)
            self._input_fps = float(cap.get(cv2.CAP_PROP_FPS) or self._input_fps)
            self._read_failures = 0
            if not self._register(generation, cap=cap):
                self._release(cap, None)
                return False
            self._log(f"opened {self.device} via opencv ({self.bridge} adapter)")
            return True
        except Exception as exc:
            self._error = f"OpenCV capture open error: {exc}"
            return False

    def _ffmpeg_argv(self) -> List[str]:
        return build_ffmpeg_command(self.device, self.fit, self.input_format, ffmpeg=self._ffmpeg)

    def _open_ffmpeg(self, generation: int) -> bool:
        try:
            proc = subprocess.Popen(
                self._ffmpeg_argv(),
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0,
            )
        except OSError as exc:
            self._error = f"Cannot start ffmpeg: {exc}"
            return False
        if not self._register(generation, proc=proc):
            self._release(None, proc)  # stop() or a newer worker won the race
            return False
        self._stderr_tail.clear()
        try:
            threading.Thread(target=self._drain_stderr, args=(proc,), daemon=True, name="hdmi-ffmpeg-stderr").start()
        except RuntimeError:
            self._stderr_tail.append("ffmpeg stderr unavailable (thread limit reached)")
        self._log(f"opened {self.device} via ffmpeg ({self.bridge} adapter, {self.input_format})")
        return True

    def _drain_stderr(self, proc: subprocess.Popen) -> None:
        try:
            for raw in iter(proc.stderr.readline, b""):
                line = raw.decode("utf-8", "replace").strip()
                if line:
                    self._stderr_tail.append(line)
        except (OSError, ValueError):
            pass

    def _last_stderr(self) -> str:
        return self._stderr_tail[-1] if self._stderr_tail else "no error output"

    # -- capture loop --------------------------------------------------------

    def _capture_loop(self, generation: int) -> None:
        buf = bytearray()
        try:
            while self._current(generation):
                try:
                    if not self._connected:
                        if not self._open_device(generation):
                            self._log(self._error)
                            self._sleep(self.RETRY_DELAY)
                            continue
                        buf.clear()
                    if self._cap is not None:
                        self._read_opencv(generation)
                    elif self._ffmpeg_proc is not None:
                        self._read_ffmpeg(generation, buf)
                    else:
                        self._connected = False
                except Exception as exc:
                    # A dead worker would disable HDMI for the rest of the service's life; recover instead.
                    self._fail(generation, f"capture worker error: {type(exc).__name__}: {exc}")
        finally:
            if self._generation == generation:
                self._cleanup(generation)
                self._running = False

    def _store(self, frame: bytes) -> None:
        with self._lock:
            self._frame = frame
            self._frame_count += 1
        self._last_frame_at = time.monotonic()

    def _read_ffmpeg(self, generation: int, buf: bytearray) -> None:
        proc = self._ffmpeg_proc
        if proc is None:
            return
        try:
            fd = proc.stdout.fileno()
            ready, _, _ = select.select([fd], [], [], 0.5)
            chunk = os.read(fd, FRAME_PIXELS - len(buf)) if ready else None
        except (OSError, ValueError) as exc:
            self._fail(generation, f"ffmpeg pipe error: {exc}")
            return
        if chunk:
            buf += chunk
            if len(buf) == FRAME_PIXELS:
                self._store(bytes(buf))
                buf.clear()
            return
        if chunk == b"" or proc.poll() is not None:
            try:
                rc: Union[int, str] = proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                rc = "hung"
            time.sleep(0.1)  # let the stderr drain thread collect the final line
            self._fail(generation, f"ffmpeg exited ({rc}): {self._last_stderr()}")
            return
        if time.monotonic() - max(self._last_frame_at, self._opened_at) > self.STALL_TIMEOUT:
            self._fail(generation, f"no frames for {self.STALL_TIMEOUT:g}s (source stopped or changed mode); reopening")

    def _read_opencv(self, generation: int) -> None:
        cap = self._cap
        if cap is None:
            return
        ret, frame = cap.read()
        if not ret or frame is None:
            self._read_failures += 1
            if self._read_failures >= 50:
                self._fail(generation, "OpenCV read failed repeatedly; reopening")
            else:
                time.sleep(0.05)
            return
        self._read_failures = 0
        try:
            self._store(downscale_bgr(frame, self.fit))
        except Exception as exc:
            self._error = f"Processing error: {exc}"
            time.sleep(0.05)

    # -- status --------------------------------------------------------------

    def get_frame(self) -> Optional[bytes]:
        with self._lock:
            return self._frame

    def get_telemetry(self) -> Dict[str, object]:
        age = round(time.monotonic() - self._last_frame_at, 2) if self._last_frame_at else None
        signal = bool(self._connected and age is not None and age < self.STALL_TIMEOUT)
        backend = "opencv" if self._cap is not None else ("ffmpeg" if self._ffmpeg_proc is not None else "")
        return {
            "type": "hdmi",
            "device": self.device,
            "bridge": self.bridge,
            "backend": backend,
            "input_format": self.input_format,
            "edid": self.edid_path,
            "edid_loaded": self._edid_loaded,
            "connected": self._connected,
            "signal": signal,
            "running": self._running,
            "input_resolution": f"{self._input_width}x{self._input_height}" if self._input_width else "",
            "input_fps": self._input_fps,
            "frames_captured": self._frame_count,
            "last_frame_age": age,
            "error": self._error,
        }


class LibraryClipSource(VideoSource):
    """Streams prepared 32x25 raw clips from the library in real-time."""

    def __init__(self, raw_path: Union[str, Path], fps: float = 10.0, loop: bool = True):
        self.path = Path(raw_path)
        self.fps = fps
        self.loop = loop
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._frames: bytes = b""
        self._total_frames = 0
        self._current_index = 0
        self._lock = threading.Lock()
        self._frame = bytes(FRAME_PIXELS)
        self._load()

    def _load(self) -> None:
        if self.path.exists() and self.path.is_file():
            self._frames = self.path.read_bytes()
            self._total_frames = len(self._frames) // FRAME_PIXELS
            if self._total_frames > 0:
                self._frame = self._frames[0:FRAME_PIXELS]

    def start(self) -> None:
        if self._running or self._total_frames == 0:
            return
        self._running = True
        self._thread = threading.Thread(target=self._loop, daemon=True, name="library-clip-worker")
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None

    def _loop(self) -> None:
        interval = 1.0 / self.fps
        idx = 0
        while self._running and self._total_frames > 0:
            t0 = time.monotonic()
            offset = idx * FRAME_PIXELS
            chunk = self._frames[offset:offset + FRAME_PIXELS]
            with self._lock:
                self._frame = chunk
                self._current_index = idx

            idx += 1
            if idx >= self._total_frames:
                if self.loop:
                    idx = 0
                else:
                    break

            elapsed = time.monotonic() - t0
            time.sleep(max(0.001, interval - elapsed))

    def get_frame(self) -> Optional[bytes]:
        with self._lock:
            return self._frame

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "type": "library",
            "path": str(self.path),
            "running": self._running,
            "total_frames": self._total_frames,
            "current_frame": self._current_index,
            "connected": bool(self._total_frames > 0),
        }


class FramePipeline:
    """Manages active video sources, optical transformations, and Nipkow serialization."""

    def __init__(
        self,
        hdmi_device: Union[str, int] = "/dev/video0",
        hdmi_fit: str = "fit",
        hdmi_format: str = "auto",
        hdmi_edid: Optional[str] = "auto",
        hdmi_backend: str = "ffmpeg",
        hdmi_bridge: str = "auto",
    ):
        self.hdmi = HDMICapture(
            device=hdmi_device, fit=hdmi_fit, input_format=hdmi_format,
            edid=hdmi_edid, backend=hdmi_backend, bridge=hdmi_bridge,
        )
        self.pattern_gen = TestPatternGenerator("smpte_bars")
        self.library_source: Optional[LibraryClipSource] = None

        self.source_mode: str = "pattern"  # 'hdmi', 'pattern', 'library'
        self.gamma: float = 1.8
        self.contrast: float = 1.0
        self._gamma_lut = build_gamma_lut(self.gamma, self.contrast)
        self.brightness: float = 0.7  # 0.0 to 1.0 master brightness
        self.invert_x: bool = False
        self.invert_y: bool = False
        self.phase_offset: int = 0  # 0 to 799 pixel phase offset

        self._lock = threading.Lock()
        self._cached_pixel_map: Optional[List[int]] = None
        self._build_pixel_map()

        # Start default source
        self.pattern_gen.start()

    def set_source_mode(self, mode: str, library_path: Optional[Union[str, Path]] = None) -> None:
        """Switch video input between 'hdmi', 'pattern', and 'library'."""
        with self._lock:
            if mode not in ("hdmi", "pattern", "library"):
                raise ValueError(f"Unknown source mode: {mode}")

            # Stop current
            if self.source_mode == "hdmi":
                self.hdmi.stop()
            elif self.source_mode == "pattern":
                self.pattern_gen.stop()
            elif self.source_mode == "library" and self.library_source:
                self.library_source.stop()

            self.source_mode = mode

            # Start new
            if mode == "hdmi":
                self.hdmi.start()
            elif mode == "pattern":
                self.pattern_gen.start()
            elif mode == "library":
                if library_path:
                    self.library_source = LibraryClipSource(library_path)
                if self.library_source:
                    self.library_source.start()

    def set_calibration(
        self,
        phase_offset: Optional[int] = None,
        invert_x: Optional[bool] = None,
        invert_y: Optional[bool] = None,
        gamma: Optional[float] = None,
        brightness: Optional[float] = None,
        contrast: Optional[float] = None,
    ) -> None:
        with self._lock:
            changed_map = False
            if phase_offset is not None:
                self.phase_offset = int(phase_offset) % FRAME_PIXELS
                changed_map = True
            if invert_x is not None:
                self.invert_x = bool(invert_x)
                changed_map = True
            if invert_y is not None:
                self.invert_y = bool(invert_y)
                changed_map = True
            if gamma is not None or contrast is not None:
                if gamma is not None:
                    self.gamma = max(0.2, min(3.5, float(gamma)))
                if contrast is not None:
                    self.contrast = max(0.1, min(3.0, float(contrast)))
                self._gamma_lut = build_gamma_lut(self.gamma, self.contrast)
            if brightness is not None:
                self.brightness = max(0.0, min(1.0, float(brightness)))

            if changed_map:
                self._build_pixel_map()

    def _build_pixel_map(self) -> None:
        """Precomputes the 800-entry coordinate mapping from row-major (32x25)

        to Nipkow disk scan sequence matching bitluni's spiral geometry:
        uint32_t x = xres - 1 - (pixelIndex / yres);
        uint32_t y = yres - 1 - (pixelIndex % yres);
        """
        mapping = []
        for i in range(FRAME_PIXELS):
            shifted_i = (i + self.phase_offset) % FRAME_PIXELS
            col = shifted_i // HEIGHT
            row = shifted_i % HEIGHT

            x = col if self.invert_x else (WIDTH - 1 - col)
            y = row if self.invert_y else (HEIGHT - 1 - row)

            row_major_idx = y * WIDTH + x
            mapping.append(row_major_idx)

        self._cached_pixel_map = mapping

    def get_latest_frame(self) -> bytes:
        """Fetch newest raw 32x25 frame from the active source."""
        frame = None
        if self.source_mode == "hdmi":
            # No signal shows black, never a stale frame or a test pattern, so a capture
            # failure is visible on the disk and in the UI instead of being masked.
            frame = self.hdmi.get_frame()
        elif self.source_mode == "pattern":
            frame = self.pattern_gen.get_frame()
        elif self.source_mode == "library":
            if self.library_source:
                frame = self.library_source.get_frame()
            if not frame:
                frame = self.pattern_gen.get_frame()

        if not frame or len(frame) != FRAME_PIXELS:
            return bytes(FRAME_PIXELS)
        return frame

    def serialize_nipkow_stream(self, frame: bytes) -> bytes:
        """Serialize a 32x25 frame into the 800-pixel Nipkow pulse stream.

        Applies gamma LUT and master brightness scaling.
        """
        if not self._cached_pixel_map:
            self._build_pixel_map()

        lut = self._gamma_lut
        b_scale = self.brightness
        mapping = self._cached_pixel_map
        stream = bytearray(FRAME_PIXELS)

        for i, src_idx in enumerate(mapping):
            raw_val = frame[src_idx]
            gamma_val = lut[raw_val]
            scaled = int(gamma_val * b_scale)
            stream[i] = max(0, min(255, scaled))

        return bytes(stream)

    def reconstruct_display_preview(self, nipkow_stream: bytes) -> bytes:
        """Reverse-maps a Nipkow pulse stream back to a 32x25 display grid

        so the operator sees on the monitor exactly what the spinning disk shows.
        """
        if not self._cached_pixel_map:
            self._build_pixel_map()

        out = bytearray(FRAME_PIXELS)
        mapping = self._cached_pixel_map
        for i, grid_idx in enumerate(mapping):
            out[grid_idx] = nipkow_stream[i]
        return bytes(out)

    def get_telemetry(self) -> Dict[str, object]:
        source_telem = {}
        if self.source_mode == "hdmi":
            source_telem = self.hdmi.get_telemetry()
        elif self.source_mode == "pattern":
            source_telem = self.pattern_gen.get_telemetry()
        elif self.source_mode == "library" and self.library_source:
            source_telem = self.library_source.get_telemetry()

        return {
            "source_mode": self.source_mode,
            "gamma": self.gamma,
            "brightness": self.brightness,
            "contrast": self.contrast,
            "invert_x": self.invert_x,
            "invert_y": self.invert_y,
            "phase_offset": self.phase_offset,
            "source_details": source_telem,
        }

    def close(self) -> None:
        self.hdmi.stop()
        self.pattern_gen.stop()
        if self.library_source:
            self.library_source.stop()
