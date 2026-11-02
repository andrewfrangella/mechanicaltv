"""Free-run output clock: LED and preview driven without index pulses, and its API/engine wiring."""
import http.client
import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

from mechanical_tv.capture import FramePipeline, FRAME_PIXELS
from mechanical_tv.hardware import RealtimeController
from mechanical_tv.hardware.controller import MAX_FPS
from mechanical_tv.server import Application, Handler, Server, password_hash


class FreeRunControllerTests(unittest.TestCase):
    def setUp(self):
        self.pipeline = FramePipeline()
        self.ctrl = RealtimeController(pipeline=self.pipeline, force_mock=True, target_fps=10.0)

    def tearDown(self):
        self.ctrl.close()

    def test_free_run_drives_led_and_preview_while_idle(self):
        self.assertEqual(self.ctrl.state, 'idle')
        self.assertFalse(self.ctrl.free_run)
        self.ctrl.set_free_run(True)
        time.sleep(0.45)
        status = self.ctrl.get_status()
        self.assertTrue(status['free_run'])
        self.assertEqual(status['output_clock'], 'free-run')
        self.assertGreaterEqual(status['free_run_frames'], 3)
        self.assertGreaterEqual(status['frame_counter'], 3)
        self.assertGreaterEqual(self.ctrl.led.frames_rendered, 3)
        self.assertFalse(self.ctrl.led.is_blanked)
        self.assertFalse(self.ctrl.led.safety_tripped)
        self.assertEqual(self.ctrl.state, 'idle')  # motor untouched
        self.assertTrue(any(self.ctrl.get_preview_frame()))
        self.ctrl.set_free_run(False)
        self.assertFalse(self.ctrl.free_run)
        self.assertTrue(self.ctrl.led.is_blanked)
        rendered = self.ctrl.led.frames_rendered
        time.sleep(0.25)
        self.assertEqual(self.ctrl.led.frames_rendered, rendered, 'clock must stop with free-run')

    def test_free_run_paces_at_target_fps(self):
        self.ctrl.set_target_fps(20.0)
        self.ctrl.set_free_run(True)
        time.sleep(0.52)
        frames = self.ctrl.get_status()['free_run_frames']
        self.assertGreaterEqual(frames, 8)
        self.assertLessEqual(frames, 13)

    def test_sensor_pulses_do_not_double_clock_in_free_run(self):
        self.ctrl.set_free_run(True)
        self.ctrl.start()  # mock sensor now pulses at ~10 Hz too
        time.sleep(0.5)
        status = self.ctrl.get_status()
        self.assertGreater(status['sensor']['pulse_count'], 0)
        # LED frames come from the free-run clock only: about 5 in 0.5 s, never ~10
        self.assertLessEqual(self.ctrl.led.frames_rendered, status['free_run_frames'] + 1)

    def test_emergency_blank_and_close_leave_free_run(self):
        self.ctrl.set_free_run(True)
        time.sleep(0.15)
        self.ctrl.emergency_blank()
        self.assertFalse(self.ctrl.free_run)
        self.assertTrue(self.ctrl.led.is_blanked)
        self.assertEqual(self.ctrl.state, 'idle')
        self.ctrl.set_free_run(True)
        self.ctrl.close()
        self.assertFalse(self.ctrl.free_run)

    def test_set_free_run_is_idempotent(self):
        self.ctrl.set_free_run(True)
        thread = self.ctrl._free_run_thread
        self.ctrl.set_free_run(True)
        self.assertIs(self.ctrl._free_run_thread, thread)
        self.ctrl.set_free_run(False)
        self.ctrl.set_free_run(False)
        self.assertIsNone(self.ctrl._free_run_thread)

    def test_speed_ceiling_is_24_fps(self):
        self.assertEqual(MAX_FPS, 24.0)
        self.ctrl.set_target_fps(30.0)
        self.assertEqual(self.ctrl.target_fps, 24.0)
        self.assertEqual(self.ctrl.motor.target_fps, 24.0)
        self.assertEqual(self.ctrl.get_status()['target_rpm'], 1440.0)
        self.ctrl.set_target_fps(0.1)
        self.assertEqual(self.ctrl.target_fps, 1.0)


class FreeRunAPITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        root = Path(cls.temp.name)
        salt = 'c3' * 16
        (root / 'auth.json').write_text(json.dumps({'salt': salt, 'hash': password_hash('free-run-test-pass', salt)}))
        cls.app = Application(root, force_mock=True)
        cls.server = Server(('127.0.0.1', 0), Handler)
        cls.server.app = cls.app
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.app.close()
        cls.server.shutdown()
        cls.server.server_close()
        cls.temp.cleanup()

    def call(self, method, path, body=None, cookie=None):
        headers = {'X-MTV-Request': '1', 'Content-Type': 'application/json'}
        if cookie:
            headers['Cookie'] = cookie
        conn = http.client.HTTPConnection('127.0.0.1', self.server.server_port, timeout=10)
        conn.request(method, path, body=json.dumps(body).encode() if body is not None else None, headers=headers)
        resp = conn.getresponse()
        data = json.loads(resp.read() or b'{}')
        cookie_out = resp.getheader('Set-Cookie')
        conn.close()
        return resp.status, data, cookie_out

    def test_free_run_action(self):
        status, _, cookie = self.call('POST', '/api/login', {'password': 'free-run-test-pass'})
        self.assertEqual(status, 200)
        cookie = cookie.split(';')[0]
        status, data, _ = self.call('POST', '/api/realtime/control', {'action': 'free_run', 'enabled': 'yes'}, cookie)
        self.assertEqual(status, 400)
        status, data, _ = self.call('POST', '/api/realtime/control', {'action': 'free_run', 'enabled': True}, cookie)
        self.assertEqual(status, 200)
        self.assertTrue(data['status']['free_run'])
        time.sleep(0.3)
        status, data, _ = self.call('GET', '/api/state', cookie=cookie)
        self.assertEqual(status, 200)
        self.assertTrue(data['realtime']['free_run'])
        self.assertGreater(data['realtime']['free_run_frames'], 0)
        self.assertEqual(len(data['player']['pixels']), FRAME_PIXELS)
        status, data, _ = self.call('POST', '/api/realtime/control', {'action': 'speed', 'fps': 24.0}, cookie)
        self.assertEqual(data['status']['target_fps'], 24.0)
        status, data, _ = self.call('POST', '/api/realtime/control', {'action': 'free_run', 'enabled': False}, cookie)
        self.assertEqual(status, 200)
        self.assertFalse(data['status']['free_run'])
        self.assertEqual(data['status']['output_clock'], 'index-sensor')


if __name__ == '__main__':
    unittest.main()
