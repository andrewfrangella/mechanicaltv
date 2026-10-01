"""Standalone HDMI/V4L2 conversion. The sink is a preview, never GPIO."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import select
import shutil
import signal
import subprocess
import threading
import time

from .core import FRAME_BYTES, WIDTH, HEIGHT, FPS, frame_filter


class PreviewSink:
    """Latest-frame output boundary; replace only after hardware timing is validated."""
    def __init__(self, root, source='hdmi'):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.count = 0
        self.source = source

    def atomic(self, name, data):
        temporary = self.root / (name + '.part')
        temporary.write_bytes(data)
        os.replace(temporary, self.root / name)

    def status(self, state, error=None):
        self.atomic('capture.json', json.dumps({
            'source': self.source, 'state': state, 'hardware_enabled': False,
            'width': WIDTH, 'height': HEIGHT, 'fps': FPS,
            'format': 'gray8-row-major', 'frames_received': self.count,
            'updated_at': time.time(), 'error': error,
        }, indent=2).encode())

    def frame(self, data):
        if len(data) != FRAME_BYTES:
            raise ValueError('Incomplete grayscale frame')
        self.atomic('latest.pgm', f'P5\n{WIDTH} {HEIGHT}\n255\n'.encode() + data)
        self.count += 1
        self.status('receiving')

    def blank(self, state, error=None):
        self.atomic('latest.pgm', f'P5\n{WIDTH} {HEIGHT}\n255\n'.encode() + bytes(FRAME_BYTES))
        self.status(state, error)


def command(args):
    base = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-threads', '1']
    if args.test_pattern:
        base += ['-re', '-f', 'lavfi', '-i', 'testsrc2=size=320x240:rate=30']
    else:
        base += ['-f', 'v4l2', '-framerate', args.input_fps, '-video_size', args.input_size]
        if args.input_format:
            base += ['-input_format', args.input_format]
        base += ['-i', args.device]
    return base + ['-an', '-sn', '-dn', '-vf', frame_filter(args.fit),
                   '-filter_threads', '1', '-threads', '1', '-pix_fmt', 'gray',
                   '-f', 'rawvideo', 'pipe:1']


def read_frame(stream, stopping, timeout):
    """Handle partial pipe reads and bound both buffering and stall time."""
    data = bytearray()
    deadline = time.monotonic() + timeout
    while len(data) < FRAME_BYTES:
        if stopping.is_set():
            return None
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Capture stalled; no complete frame received')
        ready, _, _ = select.select([stream], [], [], min(.2, remaining))
        if not ready:
            continue
        chunk = os.read(stream.fileno(), FRAME_BYTES - len(data))
        if not chunk:
            raise EOFError('Capture ended or device disconnected')
        data.extend(chunk)
    return bytes(data)


def run(args, sink, stopping):
    sink.blank('starting')
    received = 0
    try:
        while not stopping.is_set():
            process = None
            try:
                # Inherit stderr for journald; never retain an unbounded error pipe.
                process = subprocess.Popen(command(args), stdout=subprocess.PIPE, bufsize=0)
                while not stopping.is_set():
                    frame = read_frame(process.stdout, stopping, args.stall_timeout)
                    if frame is None:
                        break
                    sink.frame(frame)
                    received += 1
                    if args.frames and received >= args.frames:
                        return
            except (OSError, EOFError, TimeoutError) as exc:
                sink.blank('waiting', str(exc))
                print(f'HDMI capture waiting: {exc}', flush=True)
            finally:
                if process is not None:
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait()
                    process.stdout.close()
            stopping.wait(args.retry_seconds)
    finally:
        sink.blank('stopped')


def positive(value):
    number = float(value)
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError('Must be a finite positive number')
    return number


def arguments(argv=None):
    parser = argparse.ArgumentParser(description='HDMI capture appliance (preview output; no GPIO)')
    parser.add_argument('--device', default=os.environ.get('MTV_CAPTURE_DEVICE', '/dev/video0'))
    parser.add_argument('--input-size', default=os.environ.get('MTV_CAPTURE_SIZE', '640x480'))
    parser.add_argument('--input-fps', type=positive, default=os.environ.get('MTV_CAPTURE_FPS', '30'))
    parser.add_argument('--input-format', default=os.environ.get('MTV_CAPTURE_FORMAT', ''))
    parser.add_argument('--fit', choices=['fit', 'crop'], default=os.environ.get('MTV_CAPTURE_FIT', 'fit'))
    parser.add_argument('--data', default=os.environ.get('MTV_DATA', './data'))
    parser.add_argument('--stall-timeout', type=positive, default=5)
    parser.add_argument('--retry-seconds', type=positive, default=2)
    parser.add_argument('--test-pattern', action='store_true', help='Explicit development source; never automatic fallback')
    parser.add_argument('--frames', type=int, default=0, help='Stop after N frames; zero runs continuously')
    args = parser.parse_args(argv)
    args.input_fps = str(args.input_fps)
    if not re.fullmatch(r'[1-9][0-9]{0,3}x[1-9][0-9]{0,3}', args.input_size):
        parser.error('Input size must be WIDTHxHEIGHT (1–9999 each)')
    if args.frames < 0:
        parser.error('Frame count cannot be negative')
    if not args.device.startswith('/dev/'):
        parser.error('Capture device must be a /dev/ path')
    return args


def main():
    args = arguments()
    if not shutil.which('ffmpeg'):
        raise SystemExit('FFmpeg is required')
    stopping = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stopping.set())
    print('HDMI conversion starting — preview output only; no motor or LED control', flush=True)
    run(args, PreviewSink(Path(args.data) / 'capture', 'test-pattern' if args.test_pattern else 'hdmi'), stopping)


if __name__ == '__main__':
    main()
