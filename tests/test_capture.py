"""Tests for live HDMI capture: adapter detection, FFmpeg command, failure reporting, EDID files."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from mechanical_tv.server import Application, password_hash
from mechanical_tv.capture import (
    FRAME_PIXELS,
    FramePipeline,
    HDMICapture,
    PACKAGED_EDID,
    build_ffmpeg_command,
    detect_csi_bridge,
    parse_dv_timings,
)

REPO = Path(__file__).resolve().parent.parent


def wait_for(predicate, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.02)
    return predicate()


class FakeFFmpegCapture(HDMICapture):
    """HDMICapture whose 'ffmpeg' is a Python script, so failure paths run without hardware."""

    RETRY_DELAY = 0.1

    def __init__(self, script, device, bridge="usb", **kwargs):
        super().__init__(device=device, bridge=bridge, **kwargs)
        self.script = script
        self.spawned = []

    def _ffmpeg_argv(self):
        return [sys.executable, "-c", self.script]

    def _open_ffmpeg(self, generation):
        ok = super()._open_ffmpeg(generation)
        if ok:
            self.spawned.append(self._ffmpeg_proc)
        return ok


class CommandTests(unittest.TestCase):
    def test_input_format_precedes_input_and_defaults_are_sane(self):
        args = build_ffmpeg_command("/dev/v4l/by-id/capture", "crop", "mjpeg")
        self.assertLess(args.index("-input_format"), args.index("-i"))
        self.assertEqual(args[args.index("-input_format") + 1], "mjpeg")
        self.assertIn("fps=10,scale=32:25:force_original_aspect_ratio=increase,crop=32:25,setsar=1,format=gray", args)
        self.assertEqual(args[-1], "pipe:1")

    def test_csi_bridge_uses_uyvy_and_none_lets_ffmpeg_negotiate(self):
        args = build_ffmpeg_command("/dev/video0", "fit", "uyvy422")
        self.assertEqual(args[args.index("-input_format") + 1], "uyvy422")
        self.assertIn("pad=32:25:(ow-iw)/2:(oh-ih)/2,setsar=1,format=gray", " ".join(args))
        self.assertNotIn("-input_format", build_ffmpeg_command("/dev/video0", "fit", "none"))

    def test_parse_dv_timings(self):
        text = "\tActive width: 1920\n\tActive height: 1080\n\tPixelclock: 74250000 Hz (30.00 frames per second)\n"
        self.assertEqual(parse_dv_timings(text), (1920, 1080, 30.0))
        self.assertEqual(parse_dv_timings(""), (0, 0, 0.0))


class BridgeDetectionTests(unittest.TestCase):
    def test_detects_pi_csi_receiver_but_not_usb_dongle(self):
        with tempfile.TemporaryDirectory() as root:
            sysfs = Path(root)
            (sysfs / "video0").mkdir()
            (sysfs / "video0" / "name").write_text("unicam-image\n")
            (sysfs / "video1").mkdir()
            (sysfs / "video1" / "name").write_text("USB Video: USB Video\n")
            self.assertTrue(detect_csi_bridge("/dev/video0", sysfs))
            self.assertFalse(detect_csi_bridge("/dev/video1", sysfs))
            self.assertFalse(detect_csi_bridge("/dev/video7", sysfs))
            self.assertFalse(detect_csi_bridge(3, sysfs))

    def test_auto_format_and_edid_follow_adapter_type(self):
        csi = HDMICapture(device="/dev/video0", bridge="csi")
        self.assertEqual(csi.input_format, "uyvy422")
        self.assertEqual(csi.edid_path, str(PACKAGED_EDID))
        self.assertTrue(Path(csi.edid_path).is_file(), "packaged EDID must ship inside mechanical_tv/")
        usb = HDMICapture(device="/dev/video0", bridge="usb")
        self.assertEqual(usb.input_format, "mjpeg")
        self.assertEqual(usb.edid_path, "")
        self.assertEqual(HDMICapture(device=2, bridge="csi", input_format="mjpeg", edid="none").device, "/dev/video2")
        self.assertEqual(HDMICapture(device="/dev/video0", bridge="csi", edid="none").edid_path, "")
        self.assertEqual(HDMICapture(device="/dev/video0", bridge="usb", input_format="yuyv422").input_format, "yuyv422")

    def test_csi_bridge_without_v4l2_ctl_reports_why(self):
        with tempfile.TemporaryDirectory() as root:
            device = Path(root) / "video0"
            device.write_bytes(b"")
            cap = HDMICapture(device=str(device), bridge="csi")
            cap._v4l2ctl = None
            self.assertFalse(cap._open_device(cap._generation))
            self.assertIn("v4l2-ctl", cap.get_telemetry()["error"])
            self.assertFalse(cap.get_telemetry()["connected"])


class FailureReportingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.device = Path(self.temp.name) / "video0"
        self.device.write_bytes(b"")

    def tearDown(self):
        self.temp.cleanup()

    def test_fragmented_frame_then_exit_is_reported_and_frame_cleared(self):
        script = (
            "import os, sys, time\n"
            "os.write(1, b'a' * 123); time.sleep(0.05); os.write(1, b'a' * 677)\n"
            "time.sleep(0.6); sys.stderr.write('boom: bad input format\\n'); sys.exit(3)\n"
        )
        cap = FakeFFmpegCapture(script, str(self.device))
        cap.start()
        try:
            self.assertTrue(wait_for(lambda: cap.get_frame() == b"a" * FRAME_PIXELS), cap.get_telemetry())
            telem = cap.get_telemetry()
            self.assertTrue(telem["connected"])
            self.assertTrue(telem["signal"])
            self.assertEqual(telem["backend"], "ffmpeg")
            self.assertTrue(wait_for(lambda: "boom" in cap.get_telemetry()["error"]), cap.get_telemetry())
            telem = cap.get_telemetry()
            self.assertIn("exited (3)", telem["error"])
            self.assertIsNone(cap.get_frame(), "a dead capture must not leave a frozen frame on the disk")
            self.assertFalse(telem["signal"])
            # The worker keeps retrying rather than sleeping forever.
            self.assertTrue(wait_for(lambda: len(cap.spawned) >= 2))
        finally:
            cap.stop()
        self.assertFalse(cap.get_telemetry()["running"])
        self.assertTrue(all(p.poll() is not None for p in cap.spawned), "stop() must reap every child")

    def test_stalled_stream_is_terminated_and_reopened(self):
        cap = FakeFFmpegCapture("import time; time.sleep(30)", str(self.device))
        cap.STALL_TIMEOUT = 0.4
        cap.start()
        try:
            self.assertTrue(wait_for(lambda: "no frames" in cap.get_telemetry()["error"]), cap.get_telemetry())
            first = cap.spawned[0]
            self.assertTrue(wait_for(lambda: first.poll() is not None), "stalled child must be killed")
            self.assertTrue(wait_for(lambda: len(cap.spawned) >= 2), "capture must reopen after a stall")
        finally:
            cap.stop()

    def test_missing_device_is_reported_not_masked(self):
        cap = HDMICapture(device=str(self.device.with_name("absent")), bridge="usb")
        cap.RETRY_DELAY = 0.1
        cap.start()
        try:
            self.assertTrue(wait_for(lambda: "not found" in cap.get_telemetry()["error"]))
            self.assertFalse(cap.get_telemetry()["connected"])
        finally:
            cap.stop()

    def test_hdmi_mode_without_signal_shows_black_not_a_pattern(self):
        pipeline = FramePipeline(hdmi_device=str(self.device.with_name("absent")), hdmi_bridge="usb")
        try:
            pipeline.set_source_mode("hdmi")
            frame = pipeline.get_latest_frame()
            self.assertEqual(len(frame), FRAME_PIXELS)
            self.assertFalse(any(frame))
            telem = pipeline.get_telemetry()
            self.assertEqual(telem["source_mode"], "hdmi")
            self.assertEqual(telem["source_details"]["type"], "hdmi")
            pipeline.set_source_mode("pattern")
            self.assertTrue(any(pipeline.get_latest_frame()))
        finally:
            pipeline.close()


FRAME_SCRIPT = "import os, time\nwhile True:\n    os.write(1, b'b' * 800); time.sleep(0.05)\n"

FAKE_V4L2_CTL = """#!/usr/bin/env python3
import os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_V4L2_LOG"], "a") as log:
    log.write(" ".join(args) + "\\n")
if "--set-edid" in args:
    sys.exit(0)
if "--set-dv-bt-timings" in args:
    if os.path.exists(os.environ["FAKE_V4L2_SIGNAL"]):
        print("BT timings set"); sys.exit(0)
    print("VIDIOC_QUERY_DV_TIMINGS: failed: Link has been severed", file=sys.stderr); sys.exit(255)
if "--query-dv-timings" in args:
    print("\\tActive width: 1920\\n\\tActive height: 1080\\n\\tPixelclock: 74250000 Hz (30.00 frames per second)")
    sys.exit(0)
sys.exit(1)
"""


class LifecycleTests(unittest.TestCase):
    """Audit regressions: worker ownership of the capture process and recovery from faults."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.device = self.root / "video0"
        self.device.write_bytes(b"")

    def tearDown(self):
        self.temp.cleanup()

    def test_csi_bridge_loads_edid_once_and_locks_timings_per_open(self):
        script = self.root / "v4l2-ctl"
        script.write_text(FAKE_V4L2_CTL)
        script.chmod(0o755)
        log = self.root / "calls.log"
        signal = self.root / "signal"
        os.environ["FAKE_V4L2_LOG"] = str(log)
        os.environ["FAKE_V4L2_SIGNAL"] = str(signal)
        cap = FakeFFmpegCapture(FRAME_SCRIPT, str(self.device), bridge="csi")
        cap._v4l2ctl = str(script)
        cap.start()
        try:
            self.assertTrue(wait_for(lambda: "No HDMI signal" in cap.get_telemetry()["error"]), cap.get_telemetry())
            self.assertTrue(wait_for(lambda: log.exists() and log.read_text().count("--set-dv-bt-timings query") >= 2))
            telem = cap.get_telemetry()
            self.assertTrue(telem["edid_loaded"])
            self.assertIn("Link has been severed", telem["error"])
            self.assertFalse(telem["connected"])
            self.assertEqual(cap.spawned, [], "ffmpeg must not start before timings are locked")
            signal.write_bytes(b"")
            self.assertTrue(wait_for(lambda: cap.get_frame() == b"b" * FRAME_PIXELS), cap.get_telemetry())
            telem = cap.get_telemetry()
            self.assertEqual((telem["bridge"], telem["input_format"], telem["input_resolution"], telem["input_fps"]),
                             ("csi", "uyvy422", "1920x1080", 30.0))
            self.assertTrue(telem["signal"])
            self.assertEqual(telem["error"], "")
            calls = log.read_text()
            self.assertEqual(calls.count("--set-edid"), 1, calls)
            self.assertIn(f"file={PACKAGED_EDID}", calls)
            self.assertLess(calls.index("--set-edid"), calls.index("--set-dv-bt-timings"))
        finally:
            cap.stop()
            os.environ.pop("FAKE_V4L2_LOG", None)
            os.environ.pop("FAKE_V4L2_SIGNAL", None)

    def test_stop_during_slow_open_does_not_orphan_a_process(self):
        class SlowPrepareCapture(FakeFFmpegCapture):
            STOP_JOIN_TIMEOUT = 0.1

            def _prepare_csi(self):
                time.sleep(0.8)
                return True

        cap = SlowPrepareCapture(FRAME_SCRIPT, str(self.device), bridge="csi")
        cap.start()
        time.sleep(0.15)                 # worker is inside the slow preparation
        t0 = time.monotonic()
        cap.stop()
        self.assertLess(time.monotonic() - t0, 0.6, "stop() must not block on a slow v4l2-ctl")
        cap.start()                      # second generation while the stale worker is still preparing
        try:
            self.assertTrue(wait_for(lambda: cap.get_frame() == b"b" * FRAME_PIXELS, timeout=5), cap.get_telemetry())
            time.sleep(0.5)              # give the stale worker time to misbehave if it were going to
            self.assertEqual(len(cap.spawned), 1, "the stale worker must not open a device nobody owns")
            self.assertIs(cap.spawned[0], cap._ffmpeg_proc)
            self.assertTrue(cap.get_telemetry()["signal"])
        finally:
            cap.stop()
        self.assertTrue(all(p.poll() is not None for p in cap.spawned))

    def test_worker_survives_an_unexpected_exception(self):
        class FlakyCapture(FakeFFmpegCapture):
            def __init__(self, *a, **k):
                super().__init__(*a, **k)
                self.calls = 0
                self.logged = []

            def _open_device(self, generation):
                self.calls += 1
                if self.calls == 1:
                    raise RuntimeError("transient sysfs hiccup")
                return super()._open_device(generation)

            def _log(self, message):
                self.logged.append(message)

        cap = FlakyCapture(FRAME_SCRIPT, str(self.device))
        cap.start()
        try:
            self.assertTrue(wait_for(lambda: cap.get_frame() == b"b" * FRAME_PIXELS), cap.get_telemetry())
            self.assertTrue(cap.get_telemetry()["running"])
            self.assertTrue(any("capture worker error: RuntimeError: transient" in m for m in cap.logged), cap.logged)
        finally:
            cap.stop()
        self.assertFalse(cap.get_telemetry()["running"])
        cap.start()                      # restart after a stop still works
        try:
            self.assertTrue(wait_for(lambda: cap.get_frame() == b"b" * FRAME_PIXELS))
        finally:
            cap.stop()

    def test_default_source_hdmi_starts_capture_at_boot(self):
        salt = "c3" * 16
        (self.root / "auth.json").write_text(json.dumps({"salt": salt, "hash": password_hash("boot-test-password", salt)}))
        app = Application(self.root, hdmi_device=str(self.root / "absent"), force_mock=True,
                          hdmi_options={"hdmi_bridge": "usb"}, default_source="hdmi")
        try:
            self.assertEqual(app.realtime.pipeline.source_mode, "hdmi")
            self.assertTrue(app.realtime.pipeline.hdmi.get_telemetry()["running"])
            self.assertTrue(wait_for(lambda: "not found" in app.realtime.pipeline.hdmi.get_telemetry()["error"]))
            self.assertEqual(app.health()["version"], "0.2.0")
        finally:
            app.close()

    def test_doctor_reads_env_file_and_reports_the_adapter(self):
        env_file = self.root / "environment"
        env_file.write_text("# comment\nMTV_HDMI_FORMAT=yuyv422\nMTV_DEFAULT_SOURCE=hdmi\n")
        result = subprocess.run(
            [sys.executable, "-m", "mechanical_tv.server", "doctor", "--data", str(self.root / "no-such-data"),
             "--hdmi-device", str(self.device), "--hdmi-bridge", "csi", "--env-file", str(env_file)],
            capture_output=True, text=True, cwd=str(REPO), timeout=30,
        )
        report = json.loads(result.stdout)
        self.assertEqual(report["hdmi_bridge"], "csi")
        self.assertEqual(report["hdmi_input_format"], "yuyv422")
        self.assertEqual(report["default_source"], "hdmi")
        self.assertTrue(report["hdmi_edid"].endswith("1080p30.txt"))
        self.assertFalse(report["data_exists"])
        self.assertEqual(result.returncode, 1)


class EdidTests(unittest.TestCase):
    def test_shipped_edids_match_generator_and_have_valid_checksums(self):
        result = subprocess.run([sys.executable, str(REPO / "packaging" / "make-edid.py"), "--check"],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("1080p30", "720p60"):
            data = bytes.fromhex("".join((REPO / "mechanical_tv" / "edid" / f"{name}.txt").read_text().split()))
            self.assertEqual(len(data), 256)
            self.assertEqual(data[:8], b"\x00\xff\xff\xff\xff\xff\xff\x00")
            self.assertEqual(sum(data[:128]) % 256, 0)
            self.assertEqual(sum(data[128:]) % 256, 0)
            self.assertEqual(data[126], 1)
            # Max pixel clock 80 MHz: a Mac cannot pick 1080p60 (148.5 MHz) over the 2-lane CSI port.
            self.assertEqual(data[81], 8)

    def test_preferred_timing_is_1080p30(self):
        data = bytes.fromhex("".join(PACKAGED_EDID.read_text().split()))
        dtd = data[54:72]
        pixel_clock_khz = (dtd[0] | dtd[1] << 8) * 10
        h_active = dtd[2] | ((dtd[4] >> 4) << 8)
        v_active = dtd[5] | ((dtd[7] >> 4) << 8)
        self.assertEqual((pixel_clock_khz, h_active, v_active), (74250, 1920, 1080))


@unittest.skipUnless(shutil.which("ffmpeg"), "FFmpeg required")
class RealFFmpegTests(unittest.TestCase):
    def test_live_filter_graph_without_capture_hardware(self):
        args = build_ffmpeg_command("/dev/video0", "fit", "mjpeg")
        start, end = args.index("-f"), args.index("-vf")
        args[start:end] = ["-f", "lavfi", "-i", "testsrc2=size=160x120:rate=30", "-t", "0.5"]
        result = subprocess.run(args, capture_output=True, check=True, timeout=30)
        self.assertEqual(len(result.stdout), FRAME_PIXELS * 5)
        self.assertGreater(len(set(result.stdout)), 10)

    def test_crop_filter_graph(self):
        args = build_ffmpeg_command("/dev/video0", "crop", "uyvy422")
        start, end = args.index("-f"), args.index("-vf")
        args[start:end] = ["-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=30", "-t", "0.3"]
        result = subprocess.run(args, capture_output=True, check=True, timeout=30)
        self.assertEqual(len(result.stdout) % FRAME_PIXELS, 0)
        self.assertGreaterEqual(len(result.stdout) // FRAME_PIXELS, 2)


if __name__ == "__main__":
    unittest.main()
