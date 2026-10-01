"""Comprehensive tests for real-time video ingest, motor control, opto sync, and LED modulation."""
import http.client
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

from mechanical_tv.capture import (
    FramePipeline,
    TestPatternGenerator,
    WIDTH,
    HEIGHT,
    FRAME_PIXELS,
    build_gamma_lut,
)
from mechanical_tv.hardware import (
    RealtimeController,
    MockMotorDriver,
    MotorController,
    MockOptoSensor,
    MockLEDModulator,
    TOTAL_PIXELS,
)
from mechanical_tv.server import Application, Handler, Server, password_hash


def make_auth(root):
    salt = 'b2' * 16
    (Path(root) / 'auth.json').write_text(
        json.dumps({'salt': salt, 'hash': password_hash('realtime-test-pass', salt)})
    )


class TestRealtimeCaptureAndPipeline(unittest.TestCase):
    def setUp(self):
        self.pipeline = FramePipeline()

    def tearDown(self):
        self.pipeline.close()

    def test_gamma_lut(self):
        lut_1 = build_gamma_lut(1.0)
        self.assertEqual(len(lut_1), 256)
        self.assertEqual(lut_1[0], 0)
        self.assertEqual(lut_1[255], 255)
        self.assertEqual(lut_1[128], 128)

        lut_2 = build_gamma_lut(2.0)
        self.assertEqual(lut_2[0], 0)
        self.assertEqual(lut_2[255], 255)
        # At gamma 2.0, mid-gray is darker (around 64)
        self.assertTrue(lut_2[128] < 128)

    def test_test_patterns(self):
        gen = TestPatternGenerator()
        for pat in TestPatternGenerator.PATTERNS:
            gen.set_pattern(pat)
            frame = gen.generate_frame(0.5)
            self.assertEqual(len(frame), FRAME_PIXELS)
            self.assertTrue(all(0 <= b <= 255 for b in frame))

    def test_nipkow_coordinate_mapping(self):
        # Create a test frame where pixel value = index % 256
        test_frame = bytes(i % 256 for i in range(FRAME_PIXELS))
        stream = self.pipeline.serialize_nipkow_stream(test_frame)
        self.assertEqual(len(stream), FRAME_PIXELS)

        # Invert X
        self.pipeline.set_calibration(invert_x=True)
        stream_inv_x = self.pipeline.serialize_nipkow_stream(test_frame)
        self.assertEqual(len(stream_inv_x), FRAME_PIXELS)
        self.assertNotEqual(stream, stream_inv_x)

        # Phase offset shift
        self.pipeline.set_calibration(invert_x=False, phase_offset=25)
        stream_shifted = self.pipeline.serialize_nipkow_stream(test_frame)
        self.assertEqual(len(stream_shifted), FRAME_PIXELS)
        self.assertNotEqual(stream, stream_shifted)

    def test_reconstruction(self):
        test_frame = bytes([100] * FRAME_PIXELS)
        self.pipeline.set_calibration(brightness=1.0, gamma=1.0, phase_offset=0, invert_x=False, invert_y=False)
        stream = self.pipeline.serialize_nipkow_stream(test_frame)
        reconstructed = self.pipeline.reconstruct_display_preview(stream)
        self.assertEqual(reconstructed, test_frame)


class TestMotorDriverAndControl(unittest.TestCase):
    def test_mock_motor_stepping(self):
        driver = MockMotorDriver(port=1)
        self.assertTrue(driver.released)
        driver.step(1)
        self.assertFalse(driver.released)
        self.assertEqual(driver.step_count, 1)
        driver.step(-1)
        self.assertEqual(driver.step_count, 0)
        driver.release()
        self.assertTrue(driver.released)

    def test_velocity_ramp(self):
        driver = MockMotorDriver(port=1)
        ctrl = MotorController(driver=driver, target_fps=10.0, ramp_rate_fps=20.0)
        self.assertFalse(ctrl.is_running)
        ctrl.start()
        self.assertTrue(ctrl.is_running)

        # Allow time to ramp up
        time.sleep(0.3)
        self.assertGreater(ctrl.current_fps, 1.0)
        self.assertGreater(driver.step_count, 10)

        telem = ctrl.get_telemetry()
        self.assertEqual(telem['target_fps'], 10.0)
        self.assertEqual(telem['target_rpm'], 600.0)
        ctrl.stop()
        self.assertFalse(ctrl.is_running)
        self.assertTrue(driver.released)


class TestOptoSensor(unittest.TestCase):
    def test_mock_opto_pulses_and_lock(self):
        sensor = MockOptoSensor(pin=4)
        sensor.set_simulated_fps(10.0)
        pulses = []

        def on_pulse(t, p):
            pulses.append((t, p))

        sensor.start(callback=on_pulse)
        time.sleep(0.4)
        sensor.stop()

        self.assertGreaterEqual(len(pulses), 3)
        telem = sensor.get_telemetry()
        self.assertGreater(telem['measured_fps'], 8.0)
        self.assertLess(telem['measured_fps'], 12.0)
        self.assertGreater(telem['measured_rpm'], 480.0)

    def test_set_target_fps_and_jitter_lock_condition(self):
        sensor = MockOptoSensor(pin=4)
        sensor.set_target_fps(15.0)
        self.assertEqual(sensor.simulated_fps, 15.0)


class TestLEDModulatorAndWatchdog(unittest.TestCase):
    def test_modulation_and_watchdog_cutoff(self):
        led = MockLEDModulator(pin=21)
        led.set_brightness(0.8)

        # Feed watchdog and output frame
        led.feed_watchdog()
        frame = bytes([200] * TOTAL_PIXELS)
        led.output_frame(frame, 0.1)

        telem = led.get_telemetry()
        self.assertFalse(telem['is_blanked'])
        self.assertFalse(telem['safety_tripped'])
        self.assertEqual(telem['frames_rendered'], 1)
        self.assertGreater(telem['average_duty_cycle'], 0.5)

        # Test watchdog cutoff: wait > 0.35s without feeding
        time.sleep(0.4)
        led.output_frame(frame, 0.1)
        telem_after = led.get_telemetry()
        self.assertTrue(telem_after['safety_tripped'])
        self.assertTrue(telem_after['is_blanked'])

    def test_gpio_led_modulator_decoupled(self):
        from mechanical_tv.hardware.led import GPIOLEDModulator
        mod = GPIOLEDModulator(pin=21)
        mod.feed_watchdog()
        t0 = time.perf_counter()
        mod.output_frame(bytes([100] * TOTAL_PIXELS), 0.1)
        elapsed = time.perf_counter() - t0
        # output_frame must return in less than 50ms because it is decoupled into worker thread
        self.assertLess(elapsed, 0.05)
        mod.close()


class TestRealtimeControllerIntegration(unittest.TestCase):
    def setUp(self):
        self.pipeline = FramePipeline()
        self.ctrl = RealtimeController(
            pipeline=self.pipeline,
            force_mock=True,
            target_fps=10.0,
        )

    def tearDown(self):
        self.ctrl.close()

    def test_controller_lifecycle(self):
        self.assertEqual(self.ctrl.state, 'idle')
        self.ctrl.start()
        self.assertIn(self.ctrl.state, ('ramping', 'running', 'locked'))

        # Wait for frames to be rendered
        time.sleep(0.35)
        status = self.ctrl.get_status()
        self.assertGreater(status['frame_counter'], 0)
        self.assertEqual(len(self.ctrl.get_preview_frame()), FRAME_PIXELS)

        # Test emergency blank
        self.ctrl.emergency_blank()
        self.assertEqual(self.ctrl.state, 'idle')
        self.assertTrue(self.ctrl.led.is_blanked)

    def test_idle_preview_fallback(self):
        # Even when motor is idle (frame_counter == 0), preview frame returns active pattern
        frame = self.ctrl.get_preview_frame()
        self.assertEqual(len(frame), FRAME_PIXELS)
        self.assertTrue(any(frame))


class TestRealtimeAPIEndpoints(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temp.name)
        make_auth(cls.root)
        cls.app = Application(cls.root, force_mock=True)
        cls.server = Server(('127.0.0.1', 0), Handler)
        cls.server.app = cls.app
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.app.close()
        cls.server.shutdown()
        cls.server.server_close()
        cls.temp.cleanup()

    def request(self, method, path, body=None, cookie=None):
        headers = {'X-MTV-Request': '1'}
        if cookie:
            headers['Cookie'] = cookie
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            headers['Content-Type'] = 'application/json'
        conn = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=10)
        conn.request(method, path, body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        conn.close()
        return resp.status, data

    def login(self):
        status, headers, _ = self.app_login()
        self.assertEqual(status, 200)
        return headers['Set-Cookie'].split(';')[0]

    def app_login(self):
        headers = {'X-MTV-Request': '1', 'Content-Type': 'application/json'}
        body = json.dumps({'password': 'realtime-test-pass'}).encode()
        conn = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=10)
        conn.request('POST', '/api/login', body=body, headers=headers)
        resp = conn.getresponse()
        data = resp.read()
        headers_dict = dict(resp.getheaders())
        conn.close()
        return resp.status, headers_dict, data

    def test_realtime_api_flow(self):
        cookie = self.login()

        # 1. Check status
        status, body = self.request('GET', '/api/realtime/status', cookie=cookie)
        self.assertEqual(status, 200)
        st = json.loads(body)
        self.assertIn('state', st)
        self.assertIn('target_rpm', st)
        self.assertIn('sync_locked', st)

        # 2. Control: Set source to test pattern
        status, body = self.request(
            'POST', '/api/realtime/control',
            {'action': 'pattern', 'pattern': 'grid'},
            cookie=cookie
        )
        self.assertEqual(status, 200)

        # 3. Control: Set speed
        status, body = self.request(
            'POST', '/api/realtime/control',
            {'action': 'speed', 'fps': 12.0},
            cookie=cookie
        )
        self.assertEqual(status, 200)
        st = json.loads(body)['status']
        self.assertEqual(st['target_fps'], 12.0)
        self.assertEqual(st['target_rpm'], 720.0)

        # 4. Control: Calibration
        status, body = self.request(
            'POST', '/api/realtime/control',
            {'action': 'calibration', 'phase_offset': 50, 'gamma': 2.0, 'invert_x': True},
            cookie=cookie
        )
        self.assertEqual(status, 200)
        st = json.loads(body)['status']
        self.assertEqual(st['pipeline']['phase_offset'], 50)
        self.assertEqual(st['pipeline']['gamma'], 2.0)
        self.assertTrue(st['pipeline']['invert_x'])

        # 5. Control: Start motor
        status, body = self.request(
            'POST', '/api/realtime/control',
            {'action': 'start'},
            cookie=cookie
        )
        self.assertEqual(status, 200)

        # 6. Fetch live frame
        time.sleep(0.3)
        status, body = self.request('GET', '/api/realtime/frame', cookie=cookie)
        self.assertEqual(status, 200)
        frame_data = json.loads(body)
        self.assertEqual(frame_data['width'], WIDTH)
        self.assertEqual(frame_data['height'], HEIGHT)
        self.assertEqual(len(frame_data['pixels']), FRAME_PIXELS)

        # 7. Control: Stop motor
        status, body = self.request(
            'POST', '/api/realtime/control',
            {'action': 'stop'},
            cookie=cookie
        )
        self.assertEqual(status, 200)
        st = json.loads(body)['status']
        self.assertEqual(st['state'], 'idle')

        # 8. Rejection of invalid clip ID / path traversal
        status, body = self.request(
            'POST', '/api/realtime/control',
            {'action': 'source', 'mode': 'library', 'id': '../../etc/passwd'},
            cookie=cookie
        )
        self.assertEqual(status, 400)


if __name__ == '__main__':
    unittest.main()
