"""Tests for the in-process V4L2 capture path: struct layout, letterboxing, UYVY sampling, worker integration."""
import ctypes
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

from mechanical_tv import v4l2
from mechanical_tv.capture import FRAME_PIXELS, HDMICapture, PREVIEW_BYTES

LP64 = ctypes.sizeof(ctypes.c_long) == 8


def wait_for(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


@unittest.skipUnless(LP64, "struct sizes are checked against the LP64 kernel ABI")
class LayoutTests(unittest.TestCase):
    def test_struct_sizes_and_ioctl_numbers_match_the_kernel_headers(self):
        # Values printed by a C program compiled against <linux/videodev2.h> on aarch64.
        self.assertTrue(v4l2.LAYOUT_OK)
        self.assertEqual(ctypes.sizeof(v4l2.v4l2_buffer), 88)
        self.assertEqual(v4l2.v4l2_buffer.timestamp.offset, 24)
        self.assertEqual(v4l2.v4l2_buffer.m.offset, 64)
        self.assertEqual(v4l2.v4l2_buffer.length.offset, 72)
        self.assertEqual(v4l2.v4l2_format.fmt.offset, 8)
        self.assertEqual(v4l2.VIDIOC_QUERYCAP, 0x80685600)
        self.assertEqual(v4l2.VIDIOC_G_FMT, 0xC0D05604)
        self.assertEqual(v4l2.VIDIOC_REQBUFS, 0xC0145608)
        self.assertEqual(v4l2.VIDIOC_QUERYBUF, 0xC0585609)
        self.assertEqual(v4l2.VIDIOC_QBUF, 0xC058560F)
        self.assertEqual(v4l2.VIDIOC_DQBUF, 0xC0585611)
        self.assertEqual(v4l2.VIDIOC_STREAMON, 0x40045612)
        self.assertEqual(v4l2.V4L2_PIX_FMT_UYVY, 0x59565955)


class PlacementTests(unittest.TestCase):
    def test_letterbox_snaps_to_whole_disk_cells(self):
        self.assertEqual(v4l2.placement(1920, 1080, 320, 250, 10), (0, 30, 320, 180))
        self.assertEqual(v4l2.placement(640, 480, 320, 250, 10), (0, 0, 320, 240))
        self.assertEqual(v4l2.placement(1280, 1024, 320, 250, 10), (0, 0, 312, 250))
        self.assertEqual(v4l2.placement(1920, 1080, 32, 25), (0, 3, 32, 18))


def synthetic_uyvy(width, height):
    """Top third black; below it the left half is red (Y81 U90 V240), the right half mid-gray."""
    import numpy as np
    frame = np.zeros((height, width * 2), dtype=np.uint8)
    frame[:, 1::2] = 128
    frame[:, 0::4] = 128
    frame[:, 2::4] = 128
    frame[:, 1:width:2] = 81
    frame[:, 0:width:4] = 90
    frame[:, 2:width:4] = 240
    frame[:height // 3, 1::2] = 16
    frame[:height // 3, 0::2] = 128   # video black carries neutral chroma
    return frame.reshape(-1)


@unittest.skipUnless(v4l2.HAS_NUMPY, "numpy required")
class SamplerTests(unittest.TestCase):
    def test_fit_preview_and_disk_frame_from_1080p(self):
        import numpy as np
        sampler = v4l2.UYVYSampler(1920, 1080, 3840, "fit")
        preview, frame = sampler.convert(synthetic_uyvy(1920, 1080))
        self.assertEqual((len(preview), len(frame)), (PREVIEW_BYTES, FRAME_PIXELS))
        pv = np.frombuffer(preview, np.uint8).reshape(250, 320, 3)
        dk = np.frombuffer(frame, np.uint8).reshape(25, 32)
        self.assertEqual(pv[:30].max(), 0, "letterbox above the picture")
        self.assertEqual(pv[210:].max(), 0, "letterbox below the picture")
        self.assertEqual(pv[40, 10].tolist(), [0, 0, 0], "black band is video black, shown as RGB black")
        red = pv[150, 10].tolist()
        self.assertEqual((red[0], red[2]), (255, 0), f"red (BT.709 for HD): {red}")
        self.assertLess(red[1], 40, f"red has little green: {red}")
        self.assertEqual(pv[150, 300].tolist(), [130, 130, 130], "limited-range mid gray expands to 130")
        self.assertEqual(dk[:3].max(), 0)
        self.assertEqual(dk[21:].max(), 0)
        # Disk luma is expanded to full range exactly like FFmpeg's format=gray: 16 -> 0, 81 -> 76, 128 -> 130.
        self.assertEqual((int(dk[3, 0]), int(dk[12, 0]), int(dk[12, 31])), (0, 76, 130))

    def test_crop_fills_every_disk_row(self):
        import numpy as np
        sampler = v4l2.UYVYSampler(1920, 1080, 3840, "crop")
        preview, frame = sampler.convert(synthetic_uyvy(1920, 1080))
        dk = np.frombuffer(frame, np.uint8).reshape(25, 32)
        self.assertEqual((int(dk[0, 0]), int(dk[24, 0]), int(dk[24, 31])), (0, 76, 130))
        pv = np.frombuffer(preview, np.uint8).reshape(250, 320, 3)
        self.assertEqual(pv[:30].max(), 0, "the preview always shows the whole picture")

    def test_short_frame_is_rejected(self):
        import numpy as np
        sampler = v4l2.UYVYSampler(640, 480, 1280, "fit")
        with self.assertRaises(ValueError):
            sampler.convert(np.zeros(100, dtype=np.uint8))

    def test_sd_uses_bt601_and_matches_ffmpeg_gray_levels(self):
        import numpy as np
        sampler = v4l2.UYVYSampler(640, 480, 1280, "fit")
        preview, frame = sampler.convert(synthetic_uyvy(640, 480))
        pv = np.frombuffer(preview, np.uint8).reshape(250, 320, 3)
        self.assertEqual(pv[150, 10].tolist(), [255, 0, 0], "BT.601 red is exact")
        dk = np.frombuffer(frame, np.uint8).reshape(25, 32)
        self.assertEqual((int(dk[0, 0]), int(dk[12, 0]), int(dk[12, 31])), (0, 76, 130))


class FakeIoctl:
    """Answers the V4L2 ioctls V4L2Reader issues, recording their order, without a device."""

    def __init__(self, width=640, height=480, flags_for_dqbuf=0, bytesused=None):
        self.calls = []
        self.width, self.height = width, height
        self.sizeimage = width * 2 * height
        self.flags_for_dqbuf = flags_for_dqbuf
        self.bytesused = self.sizeimage if bytesused is None else bytesused
        self.queued = []

    def __call__(self, fd, request, arg=None, mutate=False):
        self.calls.append(request)
        if request == v4l2.VIDIOC_QUERYCAP:
            arg.device_caps = v4l2.V4L2_CAP_VIDEO_CAPTURE | v4l2.V4L2_CAP_STREAMING
        elif request == v4l2.VIDIOC_G_FMT:
            arg.fmt.pix.width, arg.fmt.pix.height = self.width, self.height
            arg.fmt.pix.pixelformat = v4l2.V4L2_PIX_FMT_UYVY
            arg.fmt.pix.bytesperline, arg.fmt.pix.sizeimage = self.width * 2, self.sizeimage
        elif request == v4l2.VIDIOC_REQBUFS:
            pass
        elif request == v4l2.VIDIOC_QUERYBUF:
            arg.length, arg.m.offset = self.sizeimage, arg.index * self.sizeimage
        elif request == v4l2.VIDIOC_QBUF:
            self.queued.append(arg.index)
        elif request == v4l2.VIDIOC_DQBUF:
            if not self.queued:
                raise BlockingIOError(11, "Resource temporarily unavailable")
            arg.index = self.queued.pop(0)
            arg.flags, arg.bytesused = self.flags_for_dqbuf, self.bytesused
        elif request in (v4l2.VIDIOC_STREAMON, v4l2.VIDIOC_STREAMOFF):
            pass
        else:
            raise OSError(25, f"unexpected ioctl {request:#x}")
        return 0


class FakeMap(bytearray):
    def __init__(self, *args, **kwargs):
        super().__init__(kwargs.get("length", 0))
        self.closed = False

    def close(self):
        self.closed = True


@unittest.skipUnless(v4l2.HAS_NUMPY and LP64, "numpy and LP64 required")
class ReaderSequenceTests(unittest.TestCase):
    """V4L2Reader against fake ioctls: the setup order, bad-buffer handling and teardown order."""

    def _reader(self, ioctl):
        self.closed_fds = []
        self.maps = []

        def fake_mmap(fd, length, flags=0, prot=0, offset=0):
            self.assertEqual(offset % self.ioctl.sizeimage, 0)
            m = FakeMap(length=length)
            self.maps.append(m)
            return m

        self.ioctl = ioctl
        patches = [
            mock.patch.object(v4l2.os, "open", lambda path, flags: 42),
            mock.patch.object(v4l2.os, "close", lambda fd: self.closed_fds.append(fd)),
            mock.patch.object(v4l2.fcntl, "ioctl", ioctl),
            mock.patch.object(v4l2.mmap, "mmap", fake_mmap),
            mock.patch.object(v4l2.select, "select", lambda r, w, x, t: (list(r), [], [])),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        return v4l2.V4L2Reader("/dev/fake")

    def test_setup_streams_and_teardown_stops_before_unmapping_and_closing(self):
        reader = self._reader(FakeIoctl())
        calls = self.ioctl.calls
        self.assertEqual(calls[:3], [v4l2.VIDIOC_QUERYCAP, v4l2.VIDIOC_G_FMT, v4l2.VIDIOC_REQBUFS])
        self.assertEqual(calls[3:11], [v4l2.VIDIOC_QUERYBUF, v4l2.VIDIOC_QBUF] * 4, "each buffer queried then queued")
        self.assertEqual(calls[11], v4l2.VIDIOC_STREAMON)
        self.assertEqual((reader.width, reader.height, reader.stride, reader.buffer_count), (640, 480, 1280, 4))
        got = reader.dequeue(0.1)
        self.assertIsNotNone(got)
        index, raw = got
        self.assertEqual((index, raw.size), (0, 640 * 2 * 480))
        del raw, got
        reader.requeue(index)
        self.assertEqual(self.ioctl.queued, [1, 2, 3, 0])
        reader.close()
        self.assertEqual(calls[-1], v4l2.VIDIOC_STREAMOFF)
        self.assertTrue(all(m.closed for m in self.maps), "every mapping unmapped")
        self.assertEqual(self.closed_fds, [42])
        reader.close()   # idempotent
        self.assertEqual(self.closed_fds, [42])

    def test_error_flagged_buffer_is_requeued_not_returned(self):
        reader = self._reader(FakeIoctl(flags_for_dqbuf=v4l2.V4L2_BUF_FLAG_ERROR))
        self.assertIsNone(reader.dequeue(0.1))
        self.assertEqual(reader.error_frames, 1)
        self.assertEqual(self.ioctl.queued, [1, 2, 3, 0], "the bad buffer went straight back to the driver")
        reader.close()

    def test_short_buffer_is_requeued_not_returned(self):
        reader = self._reader(FakeIoctl(bytesused=100))
        self.assertIsNone(reader.dequeue(0.1))
        self.assertEqual(reader.error_frames, 1)
        reader.close()

    def test_empty_queue_means_no_frame_not_an_error(self):
        reader = self._reader(FakeIoctl())
        for _ in range(4):
            index, _raw = reader.dequeue(0.1)
            del _raw
        self.assertIsNone(reader.dequeue(0.1), "EAGAIN from DQBUF is a timeout, not a failure")
        reader.close()

    def test_non_uyvy_device_is_refused_and_closed(self):
        class YUYVIoctl(FakeIoctl):
            def __call__(self, fd, request, arg=None, mutate=False):
                r = super().__call__(fd, request, arg, mutate)
                if request in (v4l2.VIDIOC_G_FMT, v4l2.VIDIOC_S_FMT):
                    arg.fmt.pix.pixelformat = v4l2.V4L2_PIX_FMT_UYVY + 1   # driver refuses UYVY
                return r

        with self.assertRaises(OSError):
            self._reader(YUYVIoctl())
        self.assertEqual(self.closed_fds, [42], "a refused open must not leak the descriptor")


class FakeReader:
    """Stands in for V4L2Reader: 30 fps of a fixed synthetic 640x480 UYVY frame, two buffers."""
    instances = []

    def __init__(self, device, buffers=4):
        import numpy as np
        self.device = device
        self.width, self.height, self.stride = 640, 480, 1280
        self.buffer_count = 2
        self.closed = False
        self.requeued = 0
        self.dequeued = 0
        self.error_frames = 0
        self._frames = [synthetic_uyvy(640, 480), synthetic_uyvy(640, 480)]
        self._queued = [0, 1]
        self._lock = threading.Lock()
        FakeReader.instances.append(self)

    short_first = False   # deliver one short frame before the good ones

    def dequeue(self, timeout):
        time.sleep(1 / 30)
        if self.closed:
            raise OSError(9, "Bad file descriptor")
        with self._lock:
            if not self._queued:
                return None
            index = self._queued.pop(0)
        self.dequeued += 1
        if self.short_first and self.dequeued == 1:
            return index, self._frames[index][:100]
        return index, self._frames[index]

    def requeue(self, index):
        with self._lock:
            self._queued.append(index)
        self.requeued += 1

    def close(self):
        self.closed = True


class NativeCapture(HDMICapture):
    RETRY_DELAY = 0.1

    def __init__(self, device, **kwargs):
        kwargs.setdefault("bridge", "csi")
        super().__init__(device=device, **kwargs)
        self.ffmpeg_opens = 0

    def _prepare_csi(self):
        self._input_width, self._input_height, self._input_fps = 640, 480, 30.0
        return True

    def _open_ffmpeg(self, generation):
        self.ffmpeg_opens += 1
        self._error = "ffmpeg stand-in"
        return False


@unittest.skipUnless(v4l2.HAS_NUMPY, "numpy required")
class NativeWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.device = Path(self.temp.name) / "video0"
        self.device.write_bytes(b"")
        FakeReader.instances.clear()

    def tearDown(self):
        self.temp.cleanup()

    def test_frames_flow_and_buffers_go_back_to_the_driver(self):
        with mock.patch.object(v4l2, "V4L2Reader", FakeReader):
            cap = NativeCapture(str(self.device), backend="auto")
            cap.start()
            try:
                self.assertTrue(wait_for(lambda: cap.get_frame() is not None), cap.get_telemetry())
                telem = cap.get_telemetry()
                self.assertEqual(telem["backend"], "v4l2")
                self.assertTrue(telem["signal"] and telem["connected"] and telem["preview"])
                self.assertEqual(telem["input_resolution"], "640x480")
                self.assertEqual(len(cap.get_preview()), PREVIEW_BYTES)
                frame = cap.get_frame()
                self.assertEqual(len(frame), FRAME_PIXELS)
                self.assertEqual(frame[12 * 32], 76, "left half red: luma 81 expands to 76")
                self.assertEqual(frame[12 * 32 + 31], 130)
                reader = FakeReader.instances[0]
                self.assertTrue(wait_for(lambda: reader.dequeued >= 12))
                time.sleep(0.1)
                self.assertGreaterEqual(reader.requeued, reader.dequeued - 1, "every buffer is handed back")
                self.assertEqual(cap.ffmpeg_opens, 0)
            finally:
                cap.stop()
            self.assertTrue(FakeReader.instances[0].closed, "stop() must release the device")
            self.assertIsNone(cap.get_frame())
            self.assertIsNone(cap.get_preview())

    def test_one_short_frame_is_skipped_without_reopening(self):
        class ShortFirstReader(FakeReader):
            short_first = True

        with mock.patch.object(v4l2, "V4L2Reader", ShortFirstReader):
            cap = NativeCapture(str(self.device), backend="v4l2")
            cap.start()
            try:
                self.assertTrue(wait_for(lambda: cap.get_frame() is not None), cap.get_telemetry())
                telem = cap.get_telemetry()
                self.assertEqual(telem["bad_frames"], 1)
                self.assertEqual(telem["error"], "")
                self.assertEqual(len(FakeReader.instances), 1, "a single short frame must not reopen the device")
            finally:
                cap.stop()

    def test_explicit_v4l2_reports_open_failure_instead_of_using_ffmpeg(self):
        def boom(device, buffers=4):
            raise OSError(25, "Inappropriate ioctl for device")

        with mock.patch.object(v4l2, "V4L2Reader", boom):
            cap = NativeCapture(str(self.device), backend="v4l2")
            cap.start()
            try:
                self.assertTrue(wait_for(lambda: "native V4L2 open failed" in cap.get_telemetry()["error"]))
                self.assertFalse(cap.get_telemetry()["connected"])
                self.assertEqual(cap.ffmpeg_opens, 0)
            finally:
                cap.stop()

    def test_auto_falls_back_to_ffmpeg_when_the_native_open_fails(self):
        def boom(device, buffers=4):
            raise OSError(25, "Inappropriate ioctl for device")

        with mock.patch.object(v4l2, "V4L2Reader", boom):
            cap = NativeCapture(str(self.device), backend="auto")
            cap.start()
            try:
                self.assertTrue(wait_for(lambda: cap.ffmpeg_opens >= 1))
            finally:
                cap.stop()

    def test_usb_dongle_never_tries_the_native_reader(self):
        with mock.patch.object(v4l2, "V4L2Reader", FakeReader):
            cap = NativeCapture(str(self.device), backend="auto", bridge="usb")
            cap.start()
            try:
                self.assertTrue(wait_for(lambda: cap.ffmpeg_opens >= 1))
                self.assertEqual(FakeReader.instances, [])
            finally:
                cap.stop()


if __name__ == "__main__":
    unittest.main()
