import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest

from mechanical_tv.capture import PreviewSink, command, consume
from mechanical_tv.core import FRAME_BYTES


class CaptureTests(unittest.TestCase):
    def test_device_options_are_input_options(self):
        args = command('/dev/v4l/by-id/capture', 'crop', 'mjpeg', '640x480', '30')
        self.assertLess(args.index('-input_format'), args.index('-i'))
        self.assertIn('fps=10,scale=32:25:force_original_aspect_ratio=increase,crop=32:25,setsar=1,format=gray', args)
        self.assertEqual(args[-1], 'pipe:1')

    def test_fragmented_frames_and_eof(self):
        with tempfile.TemporaryDirectory() as root:
            sink = PreviewSink(root)
            # Exercise pipe framing with writes smaller than one frame.
            script = 'import os,time; os.write(1,b"a"*123); time.sleep(.05); os.write(1,b"a"*677)'
            with self.assertRaisesRegex(RuntimeError, 'ended'):
                consume([sys.executable, '-c', script], sink, threading.Event())
            self.assertEqual(Path(root, 'latest.pgm').read_bytes().split(b'255\n')[1], b'a' * FRAME_BYTES)
            sink.blank('waiting', 'disconnected')
            self.assertEqual(set(Path(root, 'latest.pgm').read_bytes().split(b'255\n')[1]), {0})
            self.assertFalse(json.loads(Path(root, 'capture.json').read_text())['hardware_enabled'])

    def test_stall_terminates_child(self):
        with tempfile.TemporaryDirectory() as root:
            with self.assertRaisesRegex(RuntimeError, 'stalled'):
                consume([sys.executable, '-c', 'import time; time.sleep(30)'], PreviewSink(root), threading.Event(), timeout=.1)

    @unittest.skipUnless(shutil.which('ffmpeg'), 'FFmpeg required')
    def test_real_live_filter_without_capture_hardware(self):
        args = command('/dev/video0')
        start, end = args.index('-f'), args.index('-map')
        args[start:end] = ['-f', 'lavfi', '-i', 'testsrc2=size=160x120:rate=30', '-t', '0.5']
        result = subprocess.run(args, capture_output=True, check=True, timeout=20)
        self.assertEqual(len(result.stdout), FRAME_BYTES * 5)
        self.assertGreater(len(set(result.stdout)), 10)


if __name__ == '__main__':
    unittest.main()
