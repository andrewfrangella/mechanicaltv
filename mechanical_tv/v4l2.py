"""Direct V4L2 capture of the TC358743 HDMI-to-CSI bridge, without FFmpeg.

FFmpeg pulls every 1080p30 UYVY frame (4 MB each, 124 MB/s) out of the driver's uncached
DMA buffers, copies it, and scales it. On a Pi 4 that saturates two cores, the driver's
buffer queue backs up, and the picture on the disk runs seconds behind the source.

This reader keeps four buffers queued in the driver, dequeues each frame as it lands and
requeues it untouched, except for the frames it actually converts (NATIVE_FPS per second).
Those are sampled sparsely with numpy: the 320x250 colour preview reads one source pixel
per preview pixel, and the 32x25 disk frame is the box average of that preview. The cost
is a few milliseconds per converted frame and at most one frame of delay.

Struct layouts follow <linux/videodev2.h> on LP64 (aarch64, x86_64); sizes are checked
at import so a mismatch disables this backend instead of corrupting memory.
"""

import ctypes
import fcntl
import mmap
import os
import select
import struct
from typing import Optional, Tuple

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:  # pragma: no cover - exercised only where numpy is missing
    np = None
    HAS_NUMPY = False

WIDTH = 32
HEIGHT = 25
PREVIEW_SCALE = 10
PREVIEW_W = WIDTH * PREVIEW_SCALE    # 320
PREVIEW_H = HEIGHT * PREVIEW_SCALE   # 250
PREVIEW_BYTES = PREVIEW_W * PREVIEW_H * 3
NATIVE_FPS = 15.0                    # frames converted per second; the rest are requeued untouched
BUFFER_COUNT = 4

# -- ioctl plumbing ----------------------------------------------------------------------

_IOC_WRITE, _IOC_READ = 1, 2


def _ioc(direction: int, nr: int, size: int) -> int:
    return (direction << 30) | (size << 16) | (ord("V") << 8) | nr


class v4l2_capability(ctypes.Structure):
    _fields_ = [("driver", ctypes.c_uint8 * 16), ("card", ctypes.c_uint8 * 32), ("bus_info", ctypes.c_uint8 * 32),
                ("version", ctypes.c_uint32), ("capabilities", ctypes.c_uint32), ("device_caps", ctypes.c_uint32),
                ("reserved", ctypes.c_uint32 * 3)]


class v4l2_pix_format(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("pixelformat", ctypes.c_uint32),
                ("field", ctypes.c_uint32), ("bytesperline", ctypes.c_uint32), ("sizeimage", ctypes.c_uint32),
                ("colorspace", ctypes.c_uint32), ("priv", ctypes.c_uint32), ("flags", ctypes.c_uint32),
                ("ycbcr_enc", ctypes.c_uint32), ("quantization", ctypes.c_uint32), ("xfer_func", ctypes.c_uint32)]


class _format_union(ctypes.Union):
    _fields_ = [("pix", v4l2_pix_format), ("raw_data", ctypes.c_uint8 * 200), ("_align", ctypes.c_uint64)]


class v4l2_format(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("fmt", _format_union)]


class v4l2_requestbuffers(ctypes.Structure):
    _fields_ = [("count", ctypes.c_uint32), ("type", ctypes.c_uint32), ("memory", ctypes.c_uint32),
                ("capabilities", ctypes.c_uint32), ("flags", ctypes.c_uint8), ("reserved", ctypes.c_uint8 * 3)]


class timeval(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_usec", ctypes.c_long)]


class v4l2_timecode(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("flags", ctypes.c_uint32), ("frames", ctypes.c_uint8),
                ("seconds", ctypes.c_uint8), ("minutes", ctypes.c_uint8), ("hours", ctypes.c_uint8),
                ("userbits", ctypes.c_uint8 * 4)]


class _buffer_m(ctypes.Union):
    _fields_ = [("offset", ctypes.c_uint32), ("userptr", ctypes.c_ulong), ("planes", ctypes.c_void_p),
                ("fd", ctypes.c_int32)]


class v4l2_buffer(ctypes.Structure):
    _fields_ = [("index", ctypes.c_uint32), ("type", ctypes.c_uint32), ("bytesused", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("field", ctypes.c_uint32), ("timestamp", timeval),
                ("timecode", v4l2_timecode), ("sequence", ctypes.c_uint32), ("memory", ctypes.c_uint32),
                ("m", _buffer_m), ("length", ctypes.c_uint32), ("reserved2", ctypes.c_uint32),
                ("request_fd", ctypes.c_int32)]


VIDIOC_QUERYCAP = _ioc(_IOC_READ, 0, ctypes.sizeof(v4l2_capability))
VIDIOC_G_FMT = _ioc(_IOC_READ | _IOC_WRITE, 4, ctypes.sizeof(v4l2_format))
VIDIOC_S_FMT = _ioc(_IOC_READ | _IOC_WRITE, 5, ctypes.sizeof(v4l2_format))
VIDIOC_REQBUFS = _ioc(_IOC_READ | _IOC_WRITE, 8, ctypes.sizeof(v4l2_requestbuffers))
VIDIOC_QUERYBUF = _ioc(_IOC_READ | _IOC_WRITE, 9, ctypes.sizeof(v4l2_buffer))
VIDIOC_QBUF = _ioc(_IOC_READ | _IOC_WRITE, 15, ctypes.sizeof(v4l2_buffer))
VIDIOC_DQBUF = _ioc(_IOC_READ | _IOC_WRITE, 17, ctypes.sizeof(v4l2_buffer))
VIDIOC_STREAMON = _ioc(_IOC_WRITE, 18, ctypes.sizeof(ctypes.c_int))
VIDIOC_STREAMOFF = _ioc(_IOC_WRITE, 19, ctypes.sizeof(ctypes.c_int))

V4L2_BUF_TYPE_VIDEO_CAPTURE = 1
V4L2_MEMORY_MMAP = 1
V4L2_FIELD_NONE = 1
V4L2_CAP_VIDEO_CAPTURE = 0x00000001
V4L2_CAP_STREAMING = 0x04000000
V4L2_BUF_FLAG_ERROR = 0x00000040
V4L2_PIX_FMT_UYVY = struct.unpack("<I", b"UYVY")[0]

# Sizes from <linux/videodev2.h> compiled on LP64; any other value means the ctypes mirror is wrong.
EXPECTED_SIZES = {v4l2_capability: 104, v4l2_format: 208, v4l2_requestbuffers: 20, v4l2_buffer: 88}
LAYOUT_OK = all(ctypes.sizeof(s) == n for s, n in EXPECTED_SIZES.items()) and ctypes.sizeof(ctypes.c_long) == 8


def available() -> Tuple[bool, str]:
    """Whether this backend can run here, with the reason when it cannot."""
    if not HAS_NUMPY:
        return False, "numpy is not installed (apt install python3-numpy)"
    if not LAYOUT_OK:
        return False, "V4L2 struct layout not verified for this platform"
    if not hasattr(os, "O_NONBLOCK") or os.name != "posix":
        return False, "not a POSIX system"
    return True, ""


def fourcc_name(code: int) -> str:
    return struct.pack("<I", code).decode("ascii", "replace")


class V4L2Reader:
    """Streams memory-mapped UYVY frames from one V4L2 capture device."""

    def __init__(self, device: str, buffers: int = BUFFER_COUNT):
        self.device = device
        self.fd = -1
        self.width = 0
        self.height = 0
        self.stride = 0
        self.pixelformat = 0
        self.frame_bytes = 0
        self.error_frames = 0        # buffers the driver flagged as bad or delivered short
        self._maps = []
        self._views = []
        self._streaming = False
        self._open(buffers)

    # -- setup ---------------------------------------------------------------------------

    def _ioctl(self, request: int, arg) -> None:
        fcntl.ioctl(self.fd, request, arg, True)

    def _open(self, buffers: int) -> None:
        self.fd = os.open(self.device, os.O_RDWR | os.O_NONBLOCK)
        try:
            cap = v4l2_capability()
            self._ioctl(VIDIOC_QUERYCAP, cap)
            caps = cap.device_caps or cap.capabilities
            if not (caps & V4L2_CAP_VIDEO_CAPTURE) or not (caps & V4L2_CAP_STREAMING):
                raise OSError(f"{self.device} is not a streaming video capture device")

            fmt = v4l2_format()
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE
            self._ioctl(VIDIOC_G_FMT, fmt)
            if fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY:
                fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_UYVY
                fmt.fmt.pix.field = V4L2_FIELD_NONE
                self._ioctl(VIDIOC_S_FMT, fmt)
                if fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_UYVY:
                    raise OSError(f"{self.device} cannot deliver UYVY (driver offers {fourcc_name(fmt.fmt.pix.pixelformat)})")
            self.width = int(fmt.fmt.pix.width)
            self.height = int(fmt.fmt.pix.height)
            self.stride = int(fmt.fmt.pix.bytesperline) or self.width * 2
            self.pixelformat = int(fmt.fmt.pix.pixelformat)
            if self.width < 2 or self.height < 1 or self.stride < self.width * 2:
                raise OSError(f"{self.device} reports an unusable format {self.width}x{self.height} stride {self.stride}")
            self.frame_bytes = self.stride * self.height

            req = v4l2_requestbuffers()
            req.count, req.type, req.memory = buffers, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP
            self._ioctl(VIDIOC_REQBUFS, req)
            if req.count < 2:
                raise OSError(f"{self.device} granted only {req.count} buffer(s)")
            for index in range(req.count):
                buf = v4l2_buffer()
                buf.index, buf.type, buf.memory = index, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP
                self._ioctl(VIDIOC_QUERYBUF, buf)
                mapped = mmap.mmap(self.fd, buf.length, flags=mmap.MAP_SHARED,
                                   prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=buf.m.offset)
                self._maps.append(mapped)
                self._views.append(np.frombuffer(mapped, dtype=np.uint8))
                self._ioctl(VIDIOC_QBUF, buf)
            self._ioctl(VIDIOC_STREAMON, ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE))
            self._streaming = True
        except BaseException:
            self.close()
            raise

    @property
    def buffer_count(self) -> int:
        return len(self._maps)

    # -- streaming -----------------------------------------------------------------------

    def dequeue(self, timeout: float) -> Optional[Tuple[int, "np.ndarray"]]:
        """The next good frame as (index, uint8 array over the raw bytes), or None on timeout.

        A buffer the driver flagged as bad (V4L2_BUF_FLAG_ERROR, set by unicam around mode
        changes and signal loss) or delivered short goes straight back to the driver and
        counts in error_frames. The caller hands a returned index back to requeue().
        """
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return None
        buf = v4l2_buffer()
        buf.type, buf.memory = V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP
        try:
            self._ioctl(VIDIOC_DQBUF, buf)
        except BlockingIOError:
            return None
        index = int(buf.index)
        used = int(buf.bytesused) or self.frame_bytes
        if buf.flags & V4L2_BUF_FLAG_ERROR or used < self.frame_bytes or index >= len(self._views):
            self.error_frames += 1
            if index < len(self._views):
                self.requeue(index)
            return None
        return index, self._views[index][:used]

    def requeue(self, index: int) -> None:
        buf = v4l2_buffer()
        buf.index, buf.type, buf.memory = index, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP
        self._ioctl(VIDIOC_QBUF, buf)

    def close(self) -> None:
        if self.fd < 0:
            return
        if self._streaming:
            try:
                self._ioctl(VIDIOC_STREAMOFF, ctypes.c_int(V4L2_BUF_TYPE_VIDEO_CAPTURE))
            except OSError:
                pass
            self._streaming = False
        self._views = []
        for mapped in self._maps:
            try:
                mapped.close()
            except (BufferError, ValueError):
                pass
        self._maps = []
        try:
            os.close(self.fd)
        finally:
            self.fd = -1


# -- frame conversion ------------------------------------------------------------------------


def placement(src_w: int, src_h: int, box_w: int, box_h: int, grid: int = 1) -> Tuple[int, int, int, int]:
    """(x, y, w, h) of a source picture scaled to fit a box, its offset snapped to a `grid` lattice.

    Snapping the offset keeps the top/left letterbox edge on a disk-cell boundary: a 16:9
    picture becomes exactly rows 3-20 of the 25 disk rows. The far edge is only whole-cell
    when the scaled size is a multiple of the grid (16:9, 4:3); 720x480 or 5:4 sources end in
    a partially lit last row or column, identically on the FFmpeg path.
    """
    scale = min(box_w / src_w, box_h / src_h)
    w = max(1, min(box_w, int(round(src_w * scale))))
    h = max(1, min(box_h, int(round(src_h * scale))))
    x = ((box_w - w) // 2) // grid * grid
    y = ((box_h - h) // 2) // grid * grid
    return x, y, w, h


def _centres(count: int, span: float, origin: float = 0.0):
    """Source coordinates of `count` sample points spread evenly over `span` pixels."""
    return np.floor(origin + (np.arange(count) + 0.5) * (span / count)).astype(np.intp)


def _yuv_tables(bt709: bool):
    """Limited-range YUV to RGB as int16 lookup tables: R = Y' + Rv, G = Y' - Gu - Gv, B = Y' + Bu.

    HD sources (720 lines and up) are BT.709, SD ones BT.601; coefficients are x256 fixed point.
    """
    i = np.arange(256, dtype=np.int32)
    kr, kgu, kgv, kb = (459, 55, 136, 541) if bt709 else (409, 100, 208, 516)
    yl = (298 * (i - 16) + 128) >> 8
    rv = (kr * (i - 128) + 128) >> 8
    gu = (kgu * (i - 128) + 128) >> 8
    gv = (kgv * (i - 128) + 128) >> 8
    bu = (kb * (i - 128) + 128) >> 8
    return tuple(t.astype(np.int16) for t in (yl, rv, gu, gv, bu))


class UYVYSampler:
    """Converts raw UYVY frames of one size into the 320x250 RGB preview and the 32x25 disk frame."""

    def __init__(self, width: int, height: int, stride: int, fit: str = "fit"):
        if not HAS_NUMPY:
            raise RuntimeError("numpy is required for the native V4L2 backend")
        self.width, self.height, self.stride = width, height, stride
        self.fit = fit if fit in ("fit", "crop") else "fit"
        self.ox, self.oy, self.pw, self.ph = placement(width, height, PREVIEW_W, PREVIEW_H, PREVIEW_SCALE)
        self._rows = _centres(self.ph, height)
        xs = _centres(self.pw, width)
        self._y_cols = 2 * xs + 1
        self._u_cols = (xs // 2) * 4
        self._v_cols = self._u_cols + 2
        self._canvas = np.zeros((PREVIEW_H, PREVIEW_W, 3), dtype=np.uint8)
        self._luma = np.zeros((PREVIEW_H, PREVIEW_W), dtype=np.uint8)
        self._yl, self._rv, self._gu, self._gv, self._bu = _yuv_tables(bt709=height >= 720)
        # Full-range luma for the disk frame, matching FFmpeg's format=gray (video black 16 -> 0, white 235 -> 255).
        self._yl8 = np.clip(self._yl, 0, 255).astype(np.uint8)
        self._r = np.empty((self.ph, self.pw), dtype=np.int16)   # planar scratch: contiguous writes
        self._g = np.empty((self.ph, self.pw), dtype=np.int16)
        self._b = np.empty((self.ph, self.pw), dtype=np.int16)
        if self.fit == "crop":
            # Centre crop of the active picture to the disk aspect, sampled 5x5 per cell.
            k = 5
            aspect = WIDTH / HEIGHT
            if self.pw / self.ph > aspect:
                ch, cw = self.ph, self.ph * aspect
            else:
                cw, ch = self.pw, self.pw / aspect
            cx0, cy0 = self.ox + (self.pw - cw) / 2.0, self.oy + (self.ph - ch) / 2.0
            self._crop_rows = _centres(HEIGHT * k, ch, cy0)
            self._crop_cols = _centres(WIDTH * k, cw, cx0)
            self._crop_k = k

    def convert(self, raw: "np.ndarray") -> Tuple[bytes, bytes]:
        """(preview RGB bytes, 32x25 grayscale frame) from one raw UYVY buffer."""
        needed = self.stride * self.height
        if raw.size < needed:
            raise ValueError(f"short frame: {raw.size} of {needed} bytes")
        rows = raw[:needed].reshape(self.height, self.stride)[self._rows]   # copies only the sampled rows
        y = rows[:, self._y_cols]
        u = rows[:, self._u_cols]
        v = rows[:, self._v_cols]
        r, g, b = self._r, self._g, self._b
        yl = self._yl[y]
        np.add(yl, self._rv[v], out=r)
        np.subtract(yl, self._gu[u], out=g)
        np.subtract(g, self._gv[v], out=g)
        np.add(yl, self._bu[u], out=b)
        for plane in (r, g, b):
            np.clip(plane, 0, 255, out=plane)
        picture = self._canvas[self.oy:self.oy + self.ph, self.ox:self.ox + self.pw]
        picture[..., 0] = r
        picture[..., 1] = g
        picture[..., 2] = b
        canvas = self._canvas
        luma = self._luma
        luma[self.oy:self.oy + self.ph, self.ox:self.ox + self.pw] = self._yl8[y]
        if self.fit == "crop":
            k = self._crop_k
            cells = luma[np.ix_(self._crop_rows, self._crop_cols)].reshape(HEIGHT, k, WIDTH, k)
        else:
            k = PREVIEW_SCALE
            cells = luma.reshape(HEIGHT, k, WIDTH, k)
        frame = ((cells.sum(axis=(1, 3), dtype=np.uint32) + k * k // 2) // (k * k)).astype(np.uint8)
        return canvas.tobytes(), frame.tobytes()
