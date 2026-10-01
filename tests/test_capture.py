import json
import os
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from unittest.mock import patch

from mechanical_tv.capture import PreviewSink, arguments, command, read_frame, run
from mechanical_tv.core import FRAME_BYTES, frame_filter


class CaptureTests(unittest.TestCase):
    def test_capture_command_has_no_network_or_upload_input(self):
        args = arguments(['--device', '/dev/v4l/by-id/usb-capture-video-index0', '--fit', 'crop'])
        cmd = command(args)
        self.assertIn('v4l2', cmd)
        self.assertIn(args.device, cmd)
        self.assertIn(frame_filter('crop'), cmd)
        self.assertEqual(cmd[-1], 'pipe:1')
        self.assertNotIn('lavfi', cmd)

    def test_partial_reads_and_truncated_frame(self):
        read_fd, write_fd = os.pipe()
        with os.fdopen(read_fd, 'rb', buffering=0) as stream:
            def write():
                os.write(write_fd, b'a' * 300)
                os.write(write_fd, b'b' * 500)
                os.write(write_fd, b'partial')
                os.close(write_fd)
            thread = threading.Thread(target=write)
            thread.start()
            self.assertEqual(read_frame(stream, threading.Event(), 1), b'a' * 300 + b'b' * 500)
            with self.assertRaises(EOFError):
                read_frame(stream, threading.Event(), 1)
            thread.join()

    def test_stall_and_shutdown_interrupt_reads(self):
        read_fd, write_fd = os.pipe()
        try:
            with os.fdopen(read_fd, 'rb', buffering=0) as stream:
                with self.assertRaises(TimeoutError):
                    read_frame(stream, threading.Event(), .01)
                stopping = threading.Event()
                stopping.set()
                self.assertIsNone(read_frame(stream, stopping, 1))
        finally:
            os.close(write_fd)

    def test_missing_device_blanks_and_retries(self):
        with tempfile.TemporaryDirectory() as root:
            sink = PreviewSink(root)
            sink.frame(bytes([255]) * FRAME_BYTES)
            stopping = threading.Event()
            args = arguments([])
            # Stop on retry wait; prove process startup failure reaches a blank sink.
            def wait(_):
                status = json.loads((Path(root) / 'capture.json').read_text())
                self.assertEqual(status['state'], 'waiting')
                self.assertEqual((Path(root) / 'latest.pgm').read_bytes()[-FRAME_BYTES:], bytes(FRAME_BYTES))
                stopping.set()
            with patch('mechanical_tv.capture.subprocess.Popen', side_effect=OSError('device missing')), patch.object(stopping, 'wait', side_effect=wait):
                run(args, sink, stopping)
            self.assertEqual(json.loads((Path(root) / 'capture.json').read_text())['state'], 'stopped')

    @unittest.skipUnless(shutil.which('ffmpeg'), 'FFmpeg required')
    def test_real_conversion_without_hardware_and_shutdown_blanking(self):
        for fit in ('fit', 'crop'):
            with self.subTest(fit=fit), tempfile.TemporaryDirectory() as root:
                class RecordingSink(PreviewSink):
                    def frame(self, data):
                        super().frame(data)
                        self.last = data
                sink = RecordingSink(root)
                run(arguments(['--test-pattern', '--frames', '3', '--fit', fit]), sink, threading.Event())
                self.assertEqual(sink.count, 3)
                self.assertEqual(len(sink.last), FRAME_BYTES)
                self.assertGreater(len(set(sink.last)), 10)
                self.assertEqual((Path(root) / 'latest.pgm').read_bytes()[-FRAME_BYTES:], bytes(FRAME_BYTES))
                self.assertFalse(list(Path(root).glob('*.part')))

    def test_cli_test_source_is_explicit(self):
        self.assertFalse(arguments([]).test_pattern)
        self.assertIn('lavfi', command(arguments(['--test-pattern'])))
        with self.assertRaises(ValueError):
            frame_filter('invalid')


if __name__ == '__main__':
    unittest.main()
