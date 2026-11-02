"""Authenticated local-network HTTP service with real-time hardware synchronization."""
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
from .capture import HDMICapture, WIDTH, HEIGHT
from .core import MAX_UPLOAD, RESERVE_BYTES
from .logs import setup_logging
from .studio import Studio, load_env_file

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


class Application(Studio):
    """The Studio engine plus password sessions for the browser interface."""

    def __init__(self, root, **studio_options):
        self.auth = json.loads((Path(root) / 'auth.json').read_text())
        self.sessions = {}
        self.session_lock = threading.Lock()
        self.attempts = deque()
        super().__init__(root, **studio_options)

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
                self.reply({'status': 'ok', 'mode': self.app.health()['mode']})
                return
            if not self.require_auth():
                return
            if path == '/api/state':
                player_snap = self.app.player.snapshot()
                rt_status = self.app.realtime.get_status()
                preview_frame = self.app.realtime.get_preview_frame()
                if preview_frame and any(preview_frame):
                    player_snap['pixels'] = list(preview_frame)
                if rt_status['state'] in ('running', 'ramping', 'locked'):
                    player_snap['state'] = rt_status['state']
                self.reply({
                    'player': player_snap,
                    'library': self.app.library.items(),
                    'realtime': rt_status
                })
            elif path == '/api/realtime/status':
                self.reply(self.app.realtime.get_status())
            elif path == '/api/realtime/frame':
                frame = self.app.realtime.get_preview_frame()
                self.reply({
                    'width': WIDTH,
                    'height': HEIGHT,
                    'pixels': list(frame),
                    'status': self.app.realtime.get_status()
                })
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

            if path == '/api/realtime/control':
                action = body.get('action')
                if action == 'start':
                    self.app.realtime.start()
                elif action == 'stop':
                    self.app.realtime.stop()
                elif action == 'emergency_blank':
                    self.app.realtime.emergency_blank()
                elif action == 'free_run':
                    enabled = body.get('enabled')
                    if not isinstance(enabled, bool):
                        raise ValueError('free_run needs "enabled": true or false')
                    self.app.realtime.set_free_run(enabled)
                elif action == 'speed':
                    val = body.get('fps', body.get('value', 10.0))
                    if not isinstance(val, (int, float)) or not math.isfinite(val):
                        raise ValueError('Invalid speed value')
                    self.app.realtime.set_target_fps(float(val))
                elif action == 'source':
                    mode = body.get('mode', 'hdmi')
                    clip_id = body.get('id')
                    path_str = None
                    if clip_id:
                        path_str = self.app.clip_frames_path(clip_id)
                    self.app.realtime.set_source_mode(mode, library_path=path_str)
                elif action == 'pattern':
                    pattern = body.get('pattern', 'smpte_bars')
                    self.app.realtime.set_pattern(pattern)
                elif action == 'calibration':
                    self.app.realtime.set_calibration(
                        phase_offset=body.get('phase_offset'),
                        invert_x=body.get('invert_x'),
                        invert_y=body.get('invert_y'),
                        gamma=body.get('gamma'),
                        brightness=body.get('brightness'),
                        contrast=body.get('contrast'),
                    )
                else:
                    raise ValueError(f'Unknown realtime action: {action}')
                self.reply({'ok': True, 'status': self.app.realtime.get_status()})
                return

            if path == '/api/player':
                action = body.get('action')
                value = body.get('value')
                if action in ('seek', 'brightness'):
                    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                        raise ValueError('Expected a finite number')
                self.app.player.command(action, value)
                if action == 'select' and value:
                    clip_path = str(self.app.library.root / value / 'frames.raw')
                    self.app.realtime.set_source_mode('library', library_path=clip_path)
                elif action == 'play':
                    if self.app.realtime.state == 'idle':
                        self.app.realtime.start()
                elif action == 'stop':
                    self.app.realtime.stop()
                elif action == 'brightness':
                    self.app.realtime.set_calibration(brightness=float(value))
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
    pre = argparse.ArgumentParser(add_help=False)
    pre.add_argument('--env-file')
    pre_args, _ = pre.parse_known_args()
    if pre_args.env_file:
        load_env_file(pre_args.env_file)
    parser = argparse.ArgumentParser(description='Mechanical TV realtime hardware appliance')
    parser.add_argument('--env-file', help='read MTV_* defaults from a KEY=value file, e.g. /etc/mechanical-tv/environment')
    parser.add_argument('command', choices=['init', 'serve', 'reset-password', 'doctor'], nargs='?', default='serve')
    parser.add_argument('--data', default=os.environ.get('MTV_DATA', './data'))
    parser.add_argument('--host', default=os.environ.get('MTV_HOST', '127.0.0.1'))
    parser.add_argument('--port', type=int, default=int(os.environ.get('MTV_PORT', '8080')))
    parser.add_argument('--mock-hardware', action='store_true', default=bool(os.environ.get('MTV_MOCK_HARDWARE') == '1'))
    parser.add_argument('--hdmi-device', default=os.environ.get('MTV_HDMI_DEVICE', '/dev/video0'))
    parser.add_argument('--hdmi-bridge', choices=['auto', 'csi', 'usb'], default=os.environ.get('MTV_HDMI_BRIDGE', 'auto'),
                        help='csi: TC358743 HDMI-to-CSI-2 adapter (needs EDID + timings); usb: UVC dongle')
    parser.add_argument('--hdmi-format', default=os.environ.get('MTV_HDMI_FORMAT', 'auto'),
                        help='V4L2 input format for ffmpeg: auto (uyvy422 for csi, mjpeg for usb), none, or a name')
    parser.add_argument('--hdmi-edid', default=os.environ.get('MTV_HDMI_EDID', 'auto'),
                        help='EDID hex file loaded into a CSI bridge: auto (packaged 1080p30), none, or a path')
    parser.add_argument('--hdmi-fit', choices=['fit', 'crop'], default=os.environ.get('MTV_HDMI_FIT', 'fit'))
    parser.add_argument('--hdmi-backend', choices=['auto', 'v4l2', 'ffmpeg', 'opencv'], default=os.environ.get('MTV_HDMI_BACKEND', 'auto'))
    parser.add_argument('--default-source', choices=['pattern', 'hdmi'], default=os.environ.get('MTV_DEFAULT_SOURCE', 'pattern'),
                        help='video input selected at service start; hdmi starts capture and loads the EDID without a browser')
    parser.add_argument('--motor-port', type=int, default=int(os.environ.get('MTV_MOTOR_PORT', '1')))
    parser.add_argument('--opto-pin', type=int, default=int(os.environ.get('MTV_OPTO_PIN', '4')))
    parser.add_argument('--led-pin', type=int, default=int(os.environ.get('MTV_LED_PIN', '21')))
    parser.add_argument('--spi-led', action='store_true', default=bool(os.environ.get('MTV_SPI_LED') == '1'))
    args = parser.parse_args()

    if args.command == 'init':
        initialize(args.data)
    elif args.command == 'reset-password':
        set_password(args.data)
        print('Restart the service to invalidate existing sessions and load the new password.')
    elif args.command == 'doctor':
        probe = HDMICapture(device=args.hdmi_device, fit=args.hdmi_fit, input_format=args.hdmi_format,
                            edid=args.hdmi_edid, backend=args.hdmi_backend, bridge=args.hdmi_bridge)
        try:
            auth_configured = (Path(args.data) / 'auth.json').exists()
            data_exists = Path(args.data).is_dir()
            data_access = 'ok'
        except OSError as exc:  # e.g. PermissionError on the 0700 appliance data directory without sudo
            auth_configured = data_exists = False
            data_access = f'{type(exc).__name__}: run with sudo'
        results = {
            'python': os.sys.version.split()[0],
            'ffmpeg': shutil.which('ffmpeg'),
            'ffprobe': shutil.which('ffprobe'),
            'data': str(args.data),
            'data_access': data_access,
            'auth_configured': auth_configured,
            'data_exists': data_exists,
            'i2c_available': os.path.exists('/dev/i2c-1'),
            'v4l2_ctl': shutil.which('v4l2-ctl'),
            'hdmi_device_exists': os.path.exists(args.hdmi_device),
            'hdmi_bridge': probe.bridge if os.path.exists(args.hdmi_device) else 'device missing',
            'hdmi_input_format': probe.input_format,
            'hdmi_edid': probe.edid_path or None,
            'hdmi_fit': probe.fit,
            'hdmi_backend': probe.backend,
            'default_source': args.default_source,
        }
        print(json.dumps(results, indent=2))
        raise SystemExit(0 if results['auth_configured'] and results['data_exists'] else 1)
    else:
        if not (Path(args.data) / 'auth.json').exists():
            raise SystemExit('Run python3 -m mechanical_tv.server init first.')
        try:
            # Same rotating log file as the local console, next to the library; journald still gets stdout.
            setup_logging(Path(args.data) / 'logs')
        except OSError as exc:
            print(f'Log file unavailable: {exc}', flush=True)
        app = Application(
            args.data,
            hdmi_device=args.hdmi_device,
            hdmi_options={
                'hdmi_bridge': args.hdmi_bridge,
                'hdmi_format': args.hdmi_format,
                'hdmi_edid': args.hdmi_edid,
                'hdmi_fit': args.hdmi_fit,
                'hdmi_backend': args.hdmi_backend,
            },
            motor_port=args.motor_port,
            opto_pin=args.opto_pin,
            led_pin=args.led_pin,
            use_spi_led=args.spi_led,
            force_mock=args.mock_hardware,
            default_source=args.default_source,
        )
        server = Server((args.host, args.port), Handler)
        server.app = app
        mode_str = "REALTIME HARDWARE" if not args.mock_hardware and os.path.exists('/dev/i2c-1') else "REALTIME EMULATION"
        print(f'Mechanical TV {__version__}: http://{args.host}:{args.port} — {mode_str}', flush=True)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            app.close()
            server.server_close()


if __name__ == '__main__':
    main()
