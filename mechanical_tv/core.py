"""Persistent media library and deterministic, hardware-free player."""
import json
from contextlib import contextmanager
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import threading
import time
import uuid

WIDTH, HEIGHT, FPS = 32, 25, 10
FRAME_BYTES = WIDTH * HEIGHT
MAX_UPLOAD = 256 * 1024 * 1024
MAX_DURATION = 600
RESERVE_BYTES = 512 * 1024 * 1024


class Library:
    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.db = self.root / "library.sqlite3"
        self.guard = threading.Lock()
        with self.connect() as db:
            db.execute("""CREATE TABLE IF NOT EXISTS media (
                id TEXT PRIMARY KEY, name TEXT, status TEXT, duration REAL DEFAULT 0,
                frames INTEGER DEFAULT 0, fit TEXT, error TEXT DEFAULT '', created REAL)""")
            interrupted = list(db.execute("SELECT id FROM media WHERE status IN ('uploading','preparing')"))
            db.execute("UPDATE media SET status='failed', error='Interrupted by restart. Delete and upload again.' WHERE status IN ('uploading','preparing')")
        for row in interrupted:
            (self.root / row[0] / "upload.part").unlink(missing_ok=True)

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.db, timeout=15)
        db.row_factory = sqlite3.Row
        try:
            with db:
                yield db
        finally:
            db.close()

    def items(self):
        with self.connect() as db:
            return [dict(x) for x in db.execute("SELECT * FROM media ORDER BY created DESC")]

    def get(self, ident):
        if not isinstance(ident, str) or len(ident) != 32 or any(c not in '0123456789abcdef' for c in ident):
            raise ValueError("Invalid video identifier")
        with self.connect() as db:
            row = db.execute("SELECT * FROM media WHERE id=?", (ident,)).fetchone()
        if row is None:
            raise ValueError("Video not found")
        return dict(row)

    def update(self, ident, **values):
        allowed = {'status', 'duration', 'frames', 'error'}
        if not values or not set(values) <= allowed:
            raise ValueError("Invalid update")
        with self.connect() as db:
            db.execute("UPDATE media SET " + ','.join(f'{k}=?' for k in values) + " WHERE id=?", (*values.values(), ident))

    def create(self, name, fit):
        if fit not in ('fit', 'crop'):
            raise ValueError("Choose Fit or Crop")
        ident = uuid.uuid4().hex
        (self.root / ident).mkdir()
        with self.connect() as db:
            db.execute("INSERT INTO media(id,name,status,fit,created) VALUES(?,?,?,?,?)", (ident, name[:160], 'uploading', fit, time.time()))
        return ident

    def delete(self, ident):
        item = self.get(ident)
        if item['status'] in ('uploading', 'queued', 'preparing'):
            raise ValueError("Wait for preparation before deleting")
        shutil.rmtree(self.root / ident, ignore_errors=True)
        with self.connect() as db:
            db.execute("DELETE FROM media WHERE id=?", (ident,))

    def prepare(self, ident):
        item = self.get(ident)
        folder = self.root / ident
        self.update(ident, status='preparing', error='')
        try:
            # Restrict containers and protocols: uploads cannot be playlists or network inputs.
            input_flags = ['-protocol_whitelist', 'file', '-format_whitelist', 'mov,matroska,avi']
            probe = subprocess.run(['ffprobe', '-v', 'error', *input_flags, '-show_streams', '-show_format', '-of', 'json', str(folder / 'original')], capture_output=True, timeout=30, check=True)
            metadata = json.loads(probe.stdout)
            streams = [s for s in metadata['streams'] if s.get('codec_type') == 'video']
            if not streams:
                raise ValueError("No video stream found")
            stream = streams[0]
            duration = float(metadata['format'].get('duration', 0))
            if not 0 < duration <= MAX_DURATION:
                raise ValueError("Video must have a known duration of 10 minutes or less")
            if not 0 < stream.get('width', 0) <= 1920 or not 0 < stream.get('height', 0) <= 1920:
                raise ValueError("Maximum input dimensions are 1920 pixels on either side")
            if shutil.disk_usage(self.root).free < RESERVE_BYTES + 64 * 1024 * 1024:
                raise ValueError("Not enough free space to prepare this video")
            geometry = (f'scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=decrease,pad={WIDTH}:{HEIGHT}:(ow-iw)/2:(oh-ih)/2' if item['fit'] == 'fit' else f'scale={WIDTH}:{HEIGHT}:force_original_aspect_ratio=increase,crop={WIDTH}:{HEIGHT}')
            common = ['ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-y', '-threads', '1', *input_flags, '-i', str(folder / 'original'), '-t', str(MAX_DURATION), '-map', '0:v:0', '-an', '-sn', '-dn', '-filter_threads', '1']
            subprocess.run([*common, '-vf', f'fps={FPS},{geometry},setsar=1,format=gray', '-pix_fmt', 'gray', '-f', 'rawvideo', str(folder / 'frames.part')], capture_output=True, timeout=900, check=True)
            count, remainder = divmod((folder / 'frames.part').stat().st_size, FRAME_BYTES)
            if remainder or not 0 < count <= MAX_DURATION * FPS:
                raise ValueError("Invalid prepared frame data")
            # Browser-compatible proxy, not dependent on the original upload's codec.
            subprocess.run([*common, '-vf', 'fps=10,scale=480:480:force_original_aspect_ratio=decrease:force_divisible_by=2,setsar=1', '-c:v', 'libx264', '-threads', '1', '-preset', 'ultrafast', '-crf', '28', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', '-f', 'mp4', str(folder / 'preview.part')], capture_output=True, timeout=900, check=True)
            os.replace(folder / 'frames.part', folder / 'frames.raw')
            os.replace(folder / 'preview.part', folder / 'preview.mp4')
            (folder / 'profile.json').write_text(json.dumps({'version': 1, 'width': WIDTH, 'height': HEIGHT, 'fps': FPS, 'format': 'gray8-row-major', 'fit': item['fit'], 'frames': count}, indent=2))
            self.update(ident, status='ready', duration=count / FPS, frames=count)
        except (ValueError, KeyError, OSError, subprocess.SubprocessError, json.JSONDecodeError) as exc:
            message = str(exc) if isinstance(exc, ValueError) else 'Video preparation failed. Use an MP4/H.264 file or inspect service logs.'
            self.update(ident, status='failed', error=message[:300])
            print(f'Preparation failed for {ident}: {type(exc).__name__}', flush=True)
        finally:
            for name in ('frames.part', 'preview.part'):
                (folder / name).unlink(missing_ok=True)


class Player:
    def __init__(self, library, clock=time.monotonic):
        self.library, self.clock = library, clock
        self.lock = threading.RLock()
        self.ident = None
        self.frames = b''
        self.state = 'idle'
        self.position = 0.0
        self.started = clock()
        self.loop = True
        self.brightness = 0.7

    def _advance(self):
        now = self.clock()
        if self.state == 'playing':
            self.position += now - self.started
            duration = len(self.frames) / FRAME_BYTES / FPS
            if duration and self.position >= duration:
                if self.loop:
                    self.position %= duration
                else:
                    self.position = 0
                    self.state = 'stopped'
        self.started = now

    def command(self, action, value=None):
        with self.lock:
            self._advance()
            if action == 'select':
                item = self.library.get(value)
                if item['status'] != 'ready':
                    raise ValueError("Video is not ready")
                frames = (self.library.root / value / 'frames.raw').read_bytes()
                if len(frames) != item['frames'] * FRAME_BYTES or not frames:
                    raise ValueError("Prepared file is damaged; upload again")
                self.frames, self.ident = frames, value
                self.position, self.state = 0, 'ready'
            elif action == 'play':
                if not self.frames:
                    raise ValueError("Select a ready video first")
                self.state = 'playing'
            elif action == 'pause':
                if self.state == 'playing':
                    self.state = 'paused'
            elif action == 'stop':
                self.state, self.position = 'stopped', 0
            elif action == 'seek':
                self.position = max(0, min(float(value), max(0, len(self.frames) / FRAME_BYTES / FPS - 1 / FPS)))
            elif action == 'brightness':
                self.brightness = max(0, min(1, float(value)))
            elif action == 'loop':
                if not isinstance(value, bool):
                    raise ValueError("Loop must be true or false")
                self.loop = value
            else:
                raise ValueError("Unknown player command")

    def snapshot(self):
        with self.lock:
            self._advance()
            index = int(self.position * FPS)
            data = self.frames[index * FRAME_BYTES:(index + 1) * FRAME_BYTES]
            if self.state in ('idle', 'stopped'):
                data = bytes(FRAME_BYTES)
            return {'id': self.ident, 'state': self.state, 'position': self.position,
                    'duration': len(self.frames) / FRAME_BYTES / FPS, 'loop': self.loop,
                    'brightness': self.brightness, 'pixels': list(data),
                    'width': WIDTH, 'height': HEIGHT, 'fps': FPS, 'mode': 'simulation'}
