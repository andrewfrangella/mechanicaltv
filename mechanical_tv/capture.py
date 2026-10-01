"""Real-time video ingest and frame pipeline for Mechanical TV.

Supports live HDMI capture via V4L2/OpenCV/FFmpeg, procedural test patterns
for optical and motor calibration, and streaming pre-converted library clips.
Maps frames to the 32x25 Nipkow disk spiral geometry.
"""

from abc import ABC, abstractmethod
import math
import os
from pathlib import Path
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


class HDMICapture(VideoSource):
    """Captures live video from an HDMI capture device via V4L2/OpenCV or FFmpeg."""

    def __init__(self, device: Union[str, int] = "/dev/video0", fit: str = "fit"):
        self.device = device
        self.fit = fit
        self._running = False
        self._thread: Optional[threading.Thread] = None
        self._frame: Optional[bytes] = None
        self._lock = threading.Lock()
        self._cap = None
        self._ffmpeg_proc: Optional[subprocess.Popen] = None
        self._connected = False
        self._input_width = 0
        self._input_height = 0
        self._input_fps = 0.0
        self._frame_count = 0
        self._error = ""

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._thread = threading.Thread(target=self._capture_loop, daemon=True, name="hdmi-capture-worker")
        self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.5)
        self._thread = None
        self._cleanup()

    def _cleanup(self) -> None:
        if self._cap:
            try:
                self._cap.release()
            except Exception:
                pass
            self._cap = None
        if self._ffmpeg_proc:
            try:
                self._ffmpeg_proc.terminate()
                self._ffmpeg_proc.wait(timeout=1.0)
            except Exception:
                try:
                    self._ffmpeg_proc.kill()
                except Exception:
                    pass
            self._ffmpeg_proc = None
        self._connected = False

    def _open_device(self) -> bool:
        self._cleanup()
        # Strategy A: OpenCV VideoCapture
        if HAS_CV2:
            try:
                dev_arg = self.device
                if isinstance(dev_arg, str) and dev_arg.startswith("/dev/video"):
                    try:
                        dev_arg = int(dev_arg.replace("/dev/video", ""))
                    except ValueError:
                        pass
                cap = None
                if hasattr(cv2, "CAP_V4L2"):
                    try:
                        cap = cv2.VideoCapture(dev_arg, cv2.CAP_V4L2)
                    except Exception:
                        cap = None
                if cap is None or not cap.isOpened():
                    cap = cv2.VideoCapture(dev_arg)

                if cap.isOpened():
                    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)  # Minimal buffer for lowest latency
                    self._input_width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 0)
                    self._input_height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 0)
                    self._input_fps = float(cap.get(cv2.CAP_PROP_FPS) or 0.0)
                    self._cap = cap
                    self._connected = True
                    self._error = ""
                    return True
            except Exception as exc:
                self._error = f"OpenCV capture open error: {exc}"

        # Strategy B: FFmpeg subprocess reading v4l2
        if isinstance(self.device, str) and os.path.exists(self.device):
            try:
                filter_str = (
                    f"fps=10,scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,"
                    f"pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2,format=gray"
                    if self.fit == "fit"
                    else f"fps=10,scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase,"
                    f"crop={WIDTH}:{HEIGHT},format=gray"
                )
                cmd = [
                    "ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
                    "-f", "v4l2", "-input_format", "mjpeg", "-i", self.device,
                    "-vf", filter_str,
                    "-pix_fmt", "gray", "-f", "rawvideo", "-"
                ]
                proc = subprocess.Popen(
                    cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=FRAME_PIXELS * 2
                )
                self._ffmpeg_proc = proc
                self._connected = True
                self._error = ""
                return True
            except Exception as exc:
                self._error = f"FFmpeg capture open error: {exc}"

        self._connected = False
        if not self._error:
            self._error = f"Capture device {self.device} not found"
        return False

    def _capture_loop(self) -> None:
        while self._running:
            if not self._connected or (not self._cap and not self._ffmpeg_proc):
                if not self._open_device():
                    time.sleep(1.0)
                    continue

            # If using OpenCV
            if self._cap:
                ret, frame = self._cap.read()
                if not ret or frame is None:
                    time.sleep(0.05)
                    continue
                # Downsample and grayscale
                try:
                    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                    h, w = gray.shape
                    target_aspect = WIDTH / HEIGHT
                    current_aspect = w / h if h > 0 else target_aspect

                    if self.fit == "crop":
                        if current_aspect > target_aspect:
                            new_w = int(h * target_aspect)
                            x0 = (w - new_w) // 2
                            gray = gray[:, x0:x0 + new_w]
                        else:
                            new_h = int(w / target_aspect)
                            y0 = (h - new_h) // 2
                            gray = gray[y0:y0 + new_h, :]
                        resized = cv2.resize(gray, (WIDTH, HEIGHT), interpolation=cv2.INTER_AREA)
                    else:
                        # fit: aspect-ratio preserving letterbox / pillarbox
                        if current_aspect > target_aspect:
                            new_w = WIDTH
                            new_h = max(1, int(round(WIDTH / current_aspect)))
                            small = cv2.resize(gray, (new_w, new_h), interpolation=cv2.INTER_AREA)
                            resized = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
                            y0 = (HEIGHT - new_h) // 2
                            resized[y0:y0 + new_h, :] = small
                        else:
                            new_h = HEIGHT
                            new_w = max(1, int(round(HEIGHT * current_aspect)))
                            small = cv2.resize(gray, (new_w, new_h), interpolation=cv2.INTER_AREA)
                            resized = np.zeros((HEIGHT, WIDTH), dtype=np.uint8)
                            x0 = (WIDTH - new_w) // 2
                            resized[:, x0:x0 + new_w] = small

                    raw = resized.tobytes()
                    with self._lock:
                        self._frame = raw
                        self._frame_count += 1
                except Exception as exc:
                    self._error = f"Processing error: {exc}"
                    time.sleep(0.05)

            # If using FFmpeg pipe
            elif self._ffmpeg_proc and self._ffmpeg_proc.stdout:
                try:
                    chunk = self._ffmpeg_proc.stdout.read(FRAME_PIXELS)
                    if len(chunk) == FRAME_PIXELS:
                        with self._lock:
                            self._frame = chunk
                            self._frame_count += 1
                    else:
                        time.sleep(0.02)
                except Exception:
                    self._cleanup()
                    time.sleep(0.5)

    def get_frame(self) -> Optional[bytes]:
        with self._lock:
            return self._frame

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "type": "hdmi",
            "device": str(self.device),
            "connected": self._connected,
            "running": self._running,
            "input_resolution": f"{self._input_width}x{self._input_height}",
            "input_fps": self._input_fps,
            "frames_captured": self._frame_count,
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

    def __init__(self, hdmi_device: Union[str, int] = "/dev/video0"):
        self.hdmi = HDMICapture(device=hdmi_device)
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
            frame = self.hdmi.get_frame()
            if not frame:
                # Fallback to test pattern if HDMI has no signal
                frame = self.pattern_gen.get_frame()
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
