"""Unattended HDMI-to-gray8 capture. No HTTP, networking, or GPIO output."""
import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import threading
import time

from .core import WIDTH, HEIGHT, FPS, FRAME_BYTES


def command(device, fit='fit', input_format='', size='', rate=''):
    geometry = (f'scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2'
                if fit == 'fit' else f'scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase,crop={WIDTH}:{HEIGHT}')
    args = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-threads', '1', '-f', 'v4l2']
    for option, value in (('-input_format', input_format), ('-video_size', size), ('-framerate', rate)):
        if value:
            args.extend([option, value])
    return [*args, '-i', device, '-map', '0:v:0', '-an', '-sn', '-dn', '-filter_threads', '1',
            '-vf', f'fps={FPS},{geometry},setsar=1,format=gray', '-pix_fmt', 'gray', '-f', 'rawvideo', 'pipe:1']


class PreviewSink:
    """Single latest-frame slot on disk, not a recording or physical output driver."""
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.count = 0

    def write(self, name, data):
        temporary = self.root / (name + '.part')
        temporary.write_bytes(data)
        os.replace(temporary, self.root / name)

    def status(self, state, error=''):
        self.write('capture.json', json.dumps({'source': 'hdmi', 'state': state,
                   'hardware_enabled': False, 'frames_received': self.count,
                   'width': WIDTH, 'height': HEIGHT, 'fps': FPS,
                   'updated_at': time.time(), 'error': error}).encode())

    def frame(self, data):
        if len(data) != FRAME_BYTES:
            raise ValueError('Incomplete grayscale frame')
        self.write('latest.pgm', f'P5\n{WIDTH} {HEIGHT}\n255\n'.encode() + data)
        self.count += 1
        self.status('capturing')

    def blank(self, state, error=''):
        self.write('latest.pgm', f'P5\n{WIDTH} {HEIGHT}\n255\n'.encode() + bytes(FRAME_BYTES))
        self.status(state, error)


def consume(args, sink, stopping, timeout=5):
    """Drain complete frames; blank and reconnect on EOF or a stalled capture."""
    process = subprocess.Popen(args, stdout=subprocess.PIPE, bufsize=0)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(process.stdout, selectors.EVENT_READ)
            pending = bytearray()
            last_frame = time.monotonic()
            while not stopping.is_set():
                if time.monotonic() - last_frame > timeout:
                    raise RuntimeError('Capture stalled; check HDMI source and device mode')
                if not selector.select(.2):
                    continue
                chunk = os.read(process.stdout.fileno(), FRAME_BYTES * 64)
                if not chunk:
                    raise RuntimeError('Capture ended; check device and HDMI signal')
                pending.extend(chunk)
                # Deliver only the newest complete frame from each read.
                complete = len(pending) // FRAME_BYTES
                if complete:
                    end = complete * FRAME_BYTES
                    sink.frame(bytes(pending[end - FRAME_BYTES:end]))
                    del pending[:end]
                    last_frame = time.monotonic()
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        process.stdout.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', default=os.environ.get('MTV_CAPTURE_DEVICE', '/dev/video0'))
    parser.add_argument('--fit', choices=['fit', 'crop'], default=os.environ.get('MTV_CAPTURE_FIT', 'fit'))
    parser.add_argument('--input-format', default=os.environ.get('MTV_CAPTURE_FORMAT', ''))
    parser.add_argument('--size', default=os.environ.get('MTV_CAPTURE_SIZE', ''))
    parser.add_argument('--rate', default=os.environ.get('MTV_CAPTURE_RATE', ''))
    parser.add_argument('--data', default=os.environ.get('MTV_DATA', './data'))
    args = parser.parse_args()
    stopping = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stopping.set())
    sink = PreviewSink(Path(args.data) / 'live')
    capture_command = command(args.device, args.fit, args.input_format, args.size, args.rate)
    print(f'HDMI capture: {args.device}; preview only, hardware output disabled', flush=True)
    try:
        while not stopping.is_set():
            sink.blank('connecting')
            try:
                consume(capture_command, sink, stopping)
            except (OSError, RuntimeError) as exc:
                sink.blank('waiting', str(exc))
                print(str(exc), flush=True)
                stopping.wait(2)
    finally:
        sink.blank('stopped')


if __name__ == '__main__':
    main()
