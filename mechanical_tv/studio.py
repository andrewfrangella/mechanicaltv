"""The Mechanical TV engine shared by every user interface.

A Studio owns the media library, the frame pipeline, the realtime hardware controller and
the background conversion worker. The web server (server.py) adds HTTP, sessions and
uploads on top of it; the local desktop app (app.py) drives it directly in-process, so
nothing an operator does passes through a socket, a browser or a login.
"""
import os
from pathlib import Path
import shutil
import threading
import time

from . import __version__
from .capture import FramePipeline, WIDTH, HEIGHT
from .core import Library, Player, MAX_UPLOAD, RESERVE_BYTES
from .hardware import RealtimeController, MockMotorDriver

# Hardware settings read from the environment (install.sh writes /etc/mechanical-tv/environment).
HDMI_OPTION_ENV = {
    'hdmi_bridge': ('MTV_HDMI_BRIDGE', 'auto'),
    'hdmi_format': ('MTV_HDMI_FORMAT', 'auto'),
    'hdmi_edid': ('MTV_HDMI_EDID', 'auto'),
    'hdmi_fit': ('MTV_HDMI_FIT', 'fit'),
    'hdmi_backend': ('MTV_HDMI_BACKEND', 'auto'),
}


def hdmi_options_from_env():
    return {key: os.environ.get(name, default) for key, (name, default) in HDMI_OPTION_ENV.items()}


def load_env_file(path):
    """Read KEY=value lines (install.sh format) into os.environ without overriding existing values."""
    try:
        lines = Path(path).read_text().splitlines()
    except OSError:
        return
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        key, value = line.split('=', 1)
        os.environ.setdefault(key.strip(), value.strip())


class Studio:
    def __init__(
        self,
        root,
        hdmi_device='/dev/video0',
        motor_port=1,
        opto_pin=4,
        led_pin=21,
        use_spi_led=False,
        force_mock=False,
        hdmi_options=None,
        default_source='pattern',
    ):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.library = Library(self.root / 'media')
        self.player = Player(self.library)
        self.pipeline = FramePipeline(hdmi_device=hdmi_device, **(hdmi_options or {}))
        self.realtime = RealtimeController(
            pipeline=self.pipeline,
            motor_port=motor_port,
            opto_pin=opto_pin,
            led_pin=led_pin,
            use_spi_led=use_spi_led,
            force_mock=force_mock,
        )
        if default_source == 'hdmi':
            # Start capture (and the EDID load an HDMI source depends on) at boot instead of waiting for an operator.
            self.realtime.set_source_mode('hdmi')
        self.upload_lock = threading.Lock()
        self.stopping = threading.Event()
        self.started = time.monotonic()
        self.worker = threading.Thread(target=self.work, daemon=True, name='conversion-worker')
        self.worker.start()

    # -- background conversion ---------------------------------------------

    def work(self):
        while not self.stopping.wait(0.5):
            try:
                # Do not begin new conversions during playback or active motor scanning.
                if self.player.snapshot()['state'] == 'playing' or self.realtime.state in ('running', 'locked', 'ramping'):
                    continue
                for item in self.library.items():
                    if item['status'] == 'queued':
                        self.library.prepare(item['id'])
                        break
            except Exception as exc:
                print(f'Worker error: {type(exc).__name__}', flush=True)

    def close(self):
        self.stopping.set()
        self.realtime.close()

    # -- library -----------------------------------------------------------

    def pending_jobs(self):
        return sum(x['status'] in ('uploading', 'queued', 'preparing') for x in self.library.items())

    def import_file(self, source, fit='fit', name=None):
        """Copy a video file from local storage into the library and queue its conversion.

        Mirrors the web upload's checks (size, free space, queue depth) for files the operator
        picks on the Pi itself. Returns the new library identifier.
        """
        source = Path(source)
        if not source.is_file():
            raise ValueError(f'{source.name}: not a readable file')
        size = source.stat().st_size
        if not 0 < size <= MAX_UPLOAD:
            raise ValueError('Video must be between 1 byte and 256 MiB')
        if shutil.disk_usage(self.root).free < size + RESERVE_BYTES + 64 * 1024 * 1024:
            raise ValueError('Not enough free space; keep at least 512 MiB available')
        if self.pending_jobs() >= 3:
            raise ValueError('Preparation queue is full. Wait for a job to finish.')
        if not self.upload_lock.acquire(blocking=False):
            raise ValueError('Another import is in progress. Try again shortly.')
        ident = None
        try:
            ident = self.library.create(name or source.name, fit)
            folder = self.library.root / ident
            shutil.copyfile(source, folder / 'upload.part')
            os.replace(folder / 'upload.part', folder / 'original')
            self.library.update(ident, status='queued')
            return ident
        except Exception:
            if ident:
                (self.library.root / ident / 'upload.part').unlink(missing_ok=True)
                self.library.update(ident, status='failed', error='Import interrupted. Delete and import again.')
            raise
        finally:
            self.upload_lock.release()

    def clip_frames_path(self, ident):
        """Validated path of a ready clip's prepared frames (raises ValueError otherwise)."""
        item = self.library.get(ident)
        if item['status'] != 'ready':
            raise ValueError('Selected video is not ready')
        return str(self.library.root / ident / 'frames.raw')

    # -- diagnostics -------------------------------------------------------

    @property
    def hardware_enabled(self):
        return not isinstance(self.realtime.motor.driver, MockMotorDriver)

    @staticmethod
    def temperature_c():
        try:
            return int(Path('/sys/class/thermal/thermal_zone0/temp').read_text()) / 1000
        except (OSError, ValueError):
            return None

    def health(self):
        disk = shutil.disk_usage(self.root)
        rt_status = self.realtime.get_status()
        hw_enabled = self.hardware_enabled
        motor = rt_status.get('motor', {}).get('driver', '')
        return {
            'version': __version__,
            'mode': 'realtime' if hw_enabled else 'realtime-simulation',
            'hardware_enabled': hw_enabled,
            'uptime_seconds': int(time.monotonic() - self.started),
            'worker_alive': self.worker.is_alive(),
            'disk_free_bytes': disk.free,
            'disk_total_bytes': disk.total,
            'temperature_c': self.temperature_c(),
            'load_average': os.getloadavg(),
            'ffmpeg': bool(shutil.which('ffmpeg')),
            'ffprobe': bool(shutil.which('ffprobe')),
            'v4l2_ctl': bool(shutil.which('v4l2-ctl')),
            'profile': {'width': WIDTH, 'height': HEIGHT, 'fps': self.realtime.target_fps},
            'realtime': rt_status,
            'hardware': {
                'computer': 'Raspberry Pi 4 Model B',
                'motor_driver': motor or 'unknown',
                'motor_hat': 'Adafruit DC & Stepper Motor HAT (PCA9685 @ 0x60)' if 'HAT' in motor else 'A4988 STEP/DIR on hardware PWM (GPIO 18/23/24)' if 'A4988' in motor else motor,
                'motor': 'NEMA 17 Stepper (200 steps/rev)',
                'opto_sensor': f'Photoelectric index on GPIO {self.realtime.sensor.pin}',
                'led_modulator': f'High-power LED on GPIO {self.realtime.led.pin}',
                'hdmi_input': f'{self.realtime.pipeline.hdmi.device} ({self.realtime.pipeline.hdmi.bridge} adapter)',
            }
        }
