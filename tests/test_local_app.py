"""Local console: engine glue, logging, imports, and (when a display exists) the Tk window itself."""
import json
import logging
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from mechanical_tv import app as local_app
from mechanical_tv.capture import FRAME_PIXELS, WIDTH, HEIGHT
from mechanical_tv.logs import LOGGER, setup_logging, log_exception
from mechanical_tv.studio import Studio, load_env_file


def parse(argv):
    return local_app.build_parser().parse_args(argv)


class HelperTests(unittest.TestCase):
    def test_pgm_header_and_padding(self):
        data = local_app.pgm(bytes(range(256)) * 3 + bytes(32))
        self.assertTrue(data.startswith(b'P5 32 25 255\n'))
        self.assertEqual(len(data), len(b'P5 32 25 255\n') + FRAME_PIXELS)
        self.assertEqual(len(local_app.pgm(b'short')), len(b'P5 32 25 255\n') + FRAME_PIXELS)

    def test_default_source_auto(self):
        args = parse(['--hdmi-device', '/nonexistent/video9'])
        self.assertEqual(local_app.resolve_default_source(args), 'pattern')
        args = parse(['--hdmi-device', __file__])
        self.assertEqual(local_app.resolve_default_source(args), 'hdmi')
        args = parse(['--hdmi-device', __file__, '--mock-hardware'])
        self.assertEqual(local_app.resolve_default_source(args), 'pattern', 'simulation must not open a real device')
        args = parse(['--hdmi-device', __file__, '--mock-hardware', '--default-source', 'hdmi'])
        self.assertEqual(local_app.resolve_default_source(args), 'hdmi')

    def test_check_report(self):
        args = parse(['--check', '--hdmi-device', '/nonexistent/video9', '--data', '/tmp/x'])
        report = local_app.check_report(args)
        self.assertEqual(report['hdmi_bridge'], 'device missing')
        self.assertEqual(report['default_source'], 'pattern')
        self.assertIn('tk', report)

    def test_check_command_exits_cleanly(self):
        result = subprocess.run([sys.executable, '-m', 'mechanical_tv.app', '--check', '--hdmi-device', '/nonexistent/video9'],
                                capture_output=True, text=True, timeout=60, cwd=str(Path(__file__).resolve().parent.parent))
        report = json.loads(result.stdout)
        self.assertEqual(report['version'], local_app.__version__)
        self.assertIn(result.returncode, (0, 1))

    def test_usable_data_dir_falls_back(self):
        with tempfile.TemporaryDirectory() as temp:
            ok = local_app.usable_data_dir(os.path.join(temp, 'data'), log=lambda *a, **k: None)
            self.assertTrue(Path(ok).is_dir())
            Path(temp, 'data', 'file.txt').write_text('not a directory')
            fallback = local_app.usable_data_dir(os.path.join(temp, 'data', 'file.txt', 'child'), log=lambda *a, **k: None)
            self.assertEqual(Path(fallback), local_app.DEFAULT_DATA)

    def test_speed_slider_ceiling_matches_engine(self):
        from mechanical_tv.hardware.controller import MAX_FPS
        from mechanical_tv.hardware.motor import MAX_FPS as motor_max
        from mechanical_tv.hardware.sensor import MAX_FPS as sensor_max
        self.assertEqual(MAX_FPS, 24.0)
        self.assertEqual(motor_max, 24.0)
        self.assertEqual(sensor_max, 24.0)
        html = (Path(__file__).resolve().parent.parent / 'mechanical_tv' / 'static' / 'index.html').read_text()
        self.assertIn('id="rt-speed" min="1.0" max="24.0"', html)


class StudioTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.studio = Studio(self.temp.name, force_mock=True, hdmi_device='/nonexistent/video9')
        self.addCleanup(self.studio.close)

    def test_import_file_queues_conversion(self):
        src = Path(self.temp.name) / 'clip.mp4'
        src.write_bytes(b'\x00' * 1024)
        ident = self.studio.import_file(src, fit='crop')
        item = self.studio.library.get(ident)
        self.assertEqual(item['status'], 'queued')
        self.assertEqual(item['fit'], 'crop')
        self.assertEqual(item['name'], 'clip.mp4')
        self.assertTrue((self.studio.library.root / ident / 'original').exists())
        with self.assertRaises(ValueError):
            self.studio.clip_frames_path(ident)
        with self.assertRaises(ValueError):
            self.studio.import_file(Path(self.temp.name) / 'missing.mp4')
        with self.assertRaises(ValueError):
            self.studio.import_file(src, fit='stretch')

    def test_health_and_env(self):
        health = self.studio.health()
        self.assertEqual(health['mode'], 'realtime-simulation')
        self.assertIn('A4988', health['hardware']['motor_hat']) if 'A4988' in health['hardware']['motor_driver'] else None
        env = Path(self.temp.name) / 'env'
        env.write_text('# comment\nMTV_TEST_KEY = value with spaces\nBROKEN\n')
        os.environ.pop('MTV_TEST_KEY', None)
        load_env_file(env)
        self.assertEqual(os.environ.get('MTV_TEST_KEY'), 'value with spaces')
        os.environ.pop('MTV_TEST_KEY', None)


class LoggingTests(unittest.TestCase):
    def test_file_queue_and_print_capture(self):
        with tempfile.TemporaryDirectory() as temp:
            listener = queue.Queue()
            saved_out, saved_err = sys.stdout, sys.stderr
            try:
                path = setup_logging(Path(temp) / 'logs', listener=listener)
                print('hello from print', flush=True)
                try:
                    raise RuntimeError('boom')
                except RuntimeError as exc:
                    log_exception('unit test', exc)
                LOGGER.warning('a warning')
            finally:
                # restore streams; handlers stay on the shared logger but point at the temp file until it is removed
                sys.stdout, sys.stderr = saved_out, saved_err
            levels = []
            while not listener.empty():
                levels.append(listener.get_nowait().levelno)
            self.assertIn(logging.INFO, levels)
            self.assertIn(logging.ERROR, levels)
            self.assertIn(logging.WARNING, levels)
            for handler in list(LOGGER.handlers):
                handler.flush()
            text = Path(path).read_text()
            self.assertIn('hello from print', text)
            self.assertIn('RuntimeError: boom', text)
            self.assertIn('Traceback', text)
            for handler in list(LOGGER.handlers):
                LOGGER.removeHandler(handler)
                handler.close()


@unittest.skipUnless(os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY'), 'needs a display')
class ConsoleWindowTests(unittest.TestCase):
    """Builds the real window in simulation, exercises the refresh loop and the debug window."""

    def test_window_runs_and_updates(self):
        import tkinter as tk
        try:
            root = tk.Tk()
        except tk.TclError as exc:
            self.skipTest(f'no usable display: {exc}')
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        listener = queue.Queue()
        studio = Studio(temp.name, force_mock=True, hdmi_device='/nonexistent/video9')
        console = local_app.Console(root, studio, log_queue=listener, log_path=Path(temp.name) / 'x.log')
        try:
            root.update()
            for _ in range(8):
                root.update()
                time.sleep(0.12)
            console.action('free_run', enabled=True)
            console.action('speed', fps=24.0)
            console.jobs.join()
            for _ in range(6):
                root.update()
                time.sleep(0.12)
            self.assertTrue(studio.realtime.free_run)
            self.assertEqual(studio.realtime.target_fps, 24.0)
            self.assertEqual(console.pill_sync.cget('text'), 'SYNC: FREE-RUN')
            self.assertGreater(studio.realtime.get_status()['frame_counter'], 0)
            self.assertEqual(console.img_src_small.width(), WIDTH)
            self.assertEqual(console.img_src_small.height(), HEIGHT)
            self.assertEqual(console.img_src.width(), WIDTH * console._zoom, 'preview must be the zoomed frame')
            self.assertEqual(console.img_out.height(), HEIGHT * console._zoom)
            # an error record reaches the debug window and the pill
            record = logging.LogRecord('mechanical_tv', logging.ERROR, __file__, 1, 'synthetic failure', None, None)
            listener.put(record)
            console.debug.show()
            root.update()
            time.sleep(0.15)
            root.update()
            self.assertEqual(console.error_count, 1)
            self.assertIn('ERROR', console.pill_log.cget('text'))
            self.assertIn('synthetic failure', console.debug.text.get('1.0', 'end'))
            console.debug.errors_only.set(True)
            console.debug._refilter()
            self.assertIn('synthetic failure', console.debug.text.get('1.0', 'end'))
        finally:
            console.quit()


if __name__ == '__main__':
    unittest.main()
