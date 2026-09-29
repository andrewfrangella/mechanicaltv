"""Small authenticated local-network HTTP service; no third-party Python packages."""
import argparse
from collections import deque
import getpass
import hashlib
import hmac
from http import cookies
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit

from . import __version__
from .core import Library, Player, MAX_UPLOAD, RESERVE_BYTES

STATIC = Path(__file__).parent / 'static'


def password_hash(password, salt):
    return hashlib.scrypt(password.encode(), salt=bytes.fromhex(salt), n=16384, r=8, p=1).hex()


def initialize(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    path = root / 'auth.json'
    if path.exists():
        raise SystemExit('Authentication already configured. Use reset-password to change it.')
    set_password(root)


def set_password(root):
    password = getpass.getpass('Application password (at least 12 characters): ')
    if len(password) < 12 or password != getpass.getpass('Repeat password: '):
        raise SystemExit('Passwords must match and contain at least 12 characters.')
    salt = secrets.token_hex(16)
    path = Path(root) / 'auth.json'
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, 'w') as file:
        json.dump({'salt': salt, 'hash': password_hash(password, salt)}, file)
    os.chmod(path, 0o600)


class Application:
    def __init__(self, root):
        self.root = Path(root)
        self.auth = json.loads((self.root / 'auth.json').read_text())
        self.library = Library(self.root / 'media')
        self.player = Player(self.library)
        self.sessions = {}
        self.session_lock = threading.Lock()
        self.attempts = deque()
        self.upload_lock = threading.Lock()
        self.stopping = threading.Event()
        self.started = time.monotonic()
        self.worker = threading.Thread(target=self.work, daemon=True, name='conversion-worker')
        self.worker.start()

    def work(self):
        while not self.stopping.wait(0.5):
            try:
                # Do not begin new conversions during playback. Already-running jobs finish.
                if self.player.snapshot()['state'] == 'playing':
                    continue
                for item in self.library.items():
                    if item['status'] == 'queued':
                        self.library.prepare(item['id'])
                        break
            except Exception as exc:
                print(f'Worker error: {type(exc).__name__}', flush=True)

    def login(self, password):
        with self.session_lock:
            now = time.monotonic()
            while self.attempts and now - self.attempts[0] > 60:
                self.attempts.popleft()
            if len(self.attempts) >= 10:
                raise ValueError('Too many login attempts. Wait one minute.')
            self.attempts.append(now)
            if not hmac.compare_digest(password_hash(password, self.auth['salt']), self.auth['hash']):
                return None
            self.sessions = {k: v for k, v in self.sessions.items() if v > now}
            if len(self.sessions) >= 32:
                self.sessions.pop(next(iter(self.sessions)))
            token = secrets.token_urlsafe(32)
            self.sessions[token] = now + 12 * 3600
            return token

    def authenticated(self, token):
        with self.session_lock:
            return self.sessions.get(token, 0) > time.monotonic()

    def health(self):
        disk = shutil.disk_usage(self.root)
        temperature = None
        try:
            temperature = int(Path('/sys/class/thermal/thermal_zone0/temp').read_text()) / 1000
        except (OSError, ValueError):
            pass
        return {'version': __version__, 'mode': 'simulation', 'hardware_enabled': False,
                'uptime_seconds': int(time.monotonic() - self.started),
                'worker_alive': self.worker.is_alive(), 'disk_free_bytes': disk.free,
                'disk_total_bytes': disk.total, 'temperature_c': temperature,
                'load_average': os.getloadavg(), 'ffmpeg': bool(shutil.which('ffmpeg')),
                'ffprobe': bool(shutil.which('ffprobe')),
                'profile': {'width': 32, 'height': 25, 'fps': 10},
                'limitations': ['No GPIO output', 'No measured RPM or synchronization', 'No physical timing validation']}


class Server(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 16


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.0'

    @property
    def app(self):
        return self.server.app

    def setup(self):
        super().setup()
        self.connection.settimeout(60)

    def log_message(self, format, *args):
        # Do not log filenames, request bodies, credentials, or query strings.
        print(f'{self.client_address[0]} {self.command} {urlsplit(self.path).path}', flush=True)

    def token(self):
        jar = cookies.SimpleCookie()
        try:
            jar.load(self.headers.get('Cookie', ''))
            return jar['mtv_session'].value if 'mtv_session' in jar else ''
        except cookies.CookieError:
            return ''

    def headers_out(self, status, kind, length, extra=None):
        self.send_response(status)
        self.send_header('Content-Type', kind)
        self.send_header('Content-Length', str(length))
        self.send_header('Cache-Control', 'no-store')
        self.send_header('X-Content-Type-Options', 'nosniff')
        self.send_header('Referrer-Policy', 'no-referrer')
        self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; media-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")
        for key, value in (extra or {}).items():
            self.send_header(key, value)
        self.end_headers()

    def reply(self, data, status=200, extra=None):
        body = json.dumps(data, allow_nan=False).encode()
        self.headers_out(status, 'application/json', len(body), extra)
        self.wfile.write(body)

    def require_auth(self):
        if not self.app.authenticated(self.token()):
            self.reply({'error': 'Please sign in'}, 401)
            return False
        return True

    def do_GET(self):
        try:
            path = urlsplit(self.path).path
            assets = {'/': ('index.html', 'text/html; charset=utf-8'), '/app.js': ('app.js', 'text/javascript'), '/style.css': ('style.css', 'text/css')}
            if path in assets:
                name, kind = assets[path]
                body = (STATIC / name).read_bytes()
                self.headers_out(200, kind, len(body))
                self.wfile.write(body)
                return
            if path == '/healthz':
                self.reply({'status': 'ok', 'mode': 'simulation'})
                return
            if not self.require_auth():
                return
            if path == '/api/state':
                self.reply({'player': self.app.player.snapshot(), 'library': self.app.library.items()})
            elif path == '/api/health':
                self.reply(self.app.health())
            elif path == '/api/diagnostics':
                self.reply(self.app.health(), extra={'Content-Disposition': 'attachment; filename="mechanical-tv-diagnostics.json"'})
            elif path.startswith('/api/preview/'):
                self.preview(path.rsplit('/', 1)[-1])
            else:
                self.reply({'error': 'Not found'}, 404)
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        except (ValueError, OSError) as exc:
            self.reply({'error': str(exc)}, 400)

    def preview(self, ident):
        item = self.app.library.get(ident)
        if item['status'] != 'ready':
            raise ValueError('Preview not ready')
        path = self.app.library.root / ident / 'preview.mp4'
        with path.open('rb') as file:
            size = os.fstat(file.fileno()).st_size
            start, end, status = 0, size - 1, 200
            range_header = self.headers.get('Range')
            if range_header:
                try:
                    unit, span = range_header.split('=', 1)
                    left, right = span.split('-', 1)
                    if unit != 'bytes' or ',' in span:
                        raise ValueError()
                    if left:
                        start = int(left)
                        end = min(int(right), end) if right else end
                    else:
                        suffix = int(right)
                        if suffix <= 0:
                            raise ValueError()
                        start = max(0, size - suffix)
                    if not 0 <= start <= end < size:
                        raise ValueError()
                    status = 206
                except ValueError:
                    self.headers_out(416, 'video/mp4', 0, {'Content-Range': f'bytes */{size}'})
                    return
            extra = {'Accept-Ranges': 'bytes'}
            if status == 206:
                extra['Content-Range'] = f'bytes {start}-{end}/{size}'
            self.headers_out(status, 'video/mp4', end - start + 1, extra)
            file.seek(start)
            remaining = end - start + 1
            while remaining:
                chunk = file.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def json_body(self):
        size = int(self.headers.get('Content-Length', '0'))
        if not 0 < size <= 4096:
            raise ValueError('Invalid request size')
        raw = self.rfile.read(size)
        if len(raw) != size:
            raise ValueError('Incomplete request')
        value = json.loads(raw)
        if not isinstance(value, dict):
            raise ValueError('Expected a JSON object')
        return value

    def do_POST(self):
        try:
            # Custom header prevents cross-origin form submissions; no CORS is enabled.
            origin = self.headers.get('Origin')
            if self.headers.get('X-MTV-Request') != '1' or (origin and origin != 'http://' + self.headers.get('Host', '')):
                self.reply({'error': 'Request must come from this interface'}, 403)
                return
            path = urlsplit(self.path).path
            if path == '/api/login':
                body = self.json_body()
                password = body.get('password', '')
                if not isinstance(password, str) or len(password) > 1024:
                    raise ValueError('Invalid password')
                token = self.app.login(password)
                if not token:
                    self.reply({'error': 'Incorrect password'}, 401)
                else:
                    self.reply({'ok': True}, extra={'Set-Cookie': f'mtv_session={token}; HttpOnly; SameSite=Strict; Path=/; Max-Age=43200'})
                return
            if not self.require_auth():
                return
            if path == '/api/upload':
                self.upload()
                return
            body = self.json_body()
            if path == '/api/player':
                value = body.get('value')
                if body.get('action') in ('seek', 'brightness'):
                    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                        raise ValueError('Expected a finite number')
                self.app.player.command(body.get('action'), value)
                self.reply({'ok': True})
            elif path == '/api/delete':
                with self.app.player.lock:
                    if self.app.player.ident == body.get('id'):
                        if self.app.player.state == 'playing':
                            raise ValueError('Stop playback before deleting the selected video')
                        self.app.player.command('stop')
                        self.app.player.ident = None
                        self.app.player.frames = b''
                    self.app.library.delete(body.get('id'))
                self.reply({'ok': True})
            elif path == '/api/logout':
                with self.app.session_lock:
                    self.app.sessions.pop(self.token(), None)
                self.reply({'ok': True}, extra={'Set-Cookie': 'mtv_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0'})
            else:
                self.reply({'error': 'Not found'}, 404)
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            pass
        except (ValueError, TypeError, OSError) as exc:
            self.reply({'error': str(exc)}, 400)

    def upload(self):
        if not self.app.upload_lock.acquire(blocking=False):
            self.reply({'error': 'Another upload is in progress. Try again shortly.'}, 409)
            return
        ident = None
        try:
            if self.headers.get('Transfer-Encoding'):
                raise ValueError('A fixed upload length is required')
            size = int(self.headers.get('Content-Length', '0'))
            if not 0 < size <= MAX_UPLOAD:
                raise ValueError('Upload must be between 1 byte and 256 MiB')
            if shutil.disk_usage(self.app.root).free < size + RESERVE_BYTES + 64 * 1024 * 1024:
                raise ValueError('Not enough free space; keep at least 512 MiB available')
            query = parse_qs(urlsplit(self.path).query)
            name = Path(query.get('name', ['video'])[0]).name
            fit = query.get('fit', ['fit'])[0]
            pending = sum(x['status'] in ('uploading', 'queued', 'preparing') for x in self.app.library.items())
            if pending >= 3:
                raise ValueError('Preparation queue is full. Wait for a job to finish.')
            ident = self.app.library.create(name, fit)
            folder = self.app.library.root / ident
            remaining = size
            with (folder / 'upload.part').open('wb') as file:
                while remaining:
                    chunk = self.rfile.read(min(65536, remaining))
                    if not chunk:
                        raise ValueError('Upload interrupted; please upload again')
                    file.write(chunk)
                    remaining -= len(chunk)
            os.replace(folder / 'upload.part', folder / 'original')
            self.app.library.update(ident, status='queued')
            self.reply({'id': ident, 'status': 'queued'}, 201)
        except Exception:
            if ident:
                (self.app.library.root / ident / 'upload.part').unlink(missing_ok=True)
                self.app.library.update(ident, status='failed', error='Upload interrupted. Delete and upload again.')
            raise
        finally:
            self.app.upload_lock.release()


def main():
    parser = argparse.ArgumentParser(description='Mechanical TV simulation appliance')
    parser.add_argument('command', choices=['init', 'serve', 'reset-password', 'doctor'], nargs='?', default='serve')
    parser.add_argument('--data', default=os.environ.get('MTV_DATA', './data'))
    parser.add_argument('--host', default=os.environ.get('MTV_HOST', '127.0.0.1'))
    parser.add_argument('--port', type=int, default=int(os.environ.get('MTV_PORT', '8080')))
    args = parser.parse_args()
    if args.command == 'init':
        initialize(args.data)
    elif args.command == 'reset-password':
        set_password(args.data)
        print('Restart the service to invalidate existing sessions and load the new password.')
    elif args.command == 'doctor':
        results = {'python': os.sys.version.split()[0], 'ffmpeg': shutil.which('ffmpeg'), 'ffprobe': shutil.which('ffprobe'), 'auth_configured': (Path(args.data) / 'auth.json').exists(), 'data_exists': Path(args.data).is_dir()}
        print(json.dumps(results, indent=2))
        raise SystemExit(0 if all(results.values()) else 1)
    else:
        if not (Path(args.data) / 'auth.json').exists():
            raise SystemExit('Run python3 -m mechanical_tv.server init first.')
        app = Application(args.data)
        server = Server((args.host, args.port), Handler)
        server.app = app
        print(f'Mechanical TV {__version__}: http://{args.host}:{args.port} — SIMULATION ONLY', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            app.stopping.set()
            server.server_close()


if __name__ == '__main__':
    main()
