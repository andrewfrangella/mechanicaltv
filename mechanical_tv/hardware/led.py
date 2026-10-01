"""High-power LED pulse modulator and safety interlock for Nipkow disc output.

Modulates LED pulse widths for each of the 800 pixels per revolution based on
8-bit grayscale intensity and master brightness, matching bitluni's timing.
Includes a mandatory thermal/stall safety watchdog that shuts off the LED
if disc rotation stops or falls below safe speed.
"""

from abc import ABC, abstractmethod
import os
import threading
import time
from typing import Dict, List, Optional, Union


TOTAL_PIXELS = 800  # 32 columns * 25 rows


class BaseLEDModulator(ABC):
    """Abstract interface for LED pulse modulators."""

    @abstractmethod
    def set_brightness(self, brightness: float) -> None:
        """Set master brightness factor (0.0 to 1.0)."""
        pass

    @abstractmethod
    def output_frame(self, nipkow_stream: bytes, frame_period_s: float) -> None:
        """Modulate the 800 pixels across the given revolution period."""
        pass

    @abstractmethod
    def blank(self) -> None:
        """Force LED off immediately."""
        pass

    @abstractmethod
    def feed_watchdog(self) -> None:
        """Signal that disc is rotating safely."""
        pass

    @abstractmethod
    def get_telemetry(self) -> Dict[str, object]:
        pass

    def close(self) -> None:
        """Clean shutdown."""
        self.blank()


class MockLEDModulator(BaseLEDModulator):
    """Simulated LED modulator for testing and development."""

    def __init__(self, pin: int = 21):
        self.pin = pin
        self.brightness = 0.7
        self.is_blanked = True
        self.safety_tripped = False
        self.last_watchdog_time = time.monotonic()
        self.frames_rendered = 0
        self.average_duty_cycle = 0.0
        self.last_stream: bytes = bytes(TOTAL_PIXELS)
        self._lock = threading.Lock()

    def set_brightness(self, brightness: float) -> None:
        with self._lock:
            self.brightness = max(0.0, min(1.0, float(brightness)))

    def feed_watchdog(self) -> None:
        with self._lock:
            self.last_watchdog_time = time.monotonic()
            self.safety_tripped = False

    def blank(self) -> None:
        with self._lock:
            self.is_blanked = True

    def output_frame(self, nipkow_stream: bytes, frame_period_s: float) -> None:
        now = time.monotonic()
        with self._lock:
            # Check watchdog: if no feed in 350 ms, safety trip
            if now - self.last_watchdog_time > 0.35:
                self.safety_tripped = True
                self.is_blanked = True
                return

            if len(nipkow_stream) != TOTAL_PIXELS:
                return

            self.is_blanked = False
            self.last_stream = nipkow_stream
            self.frames_rendered += 1

            # Calculate average duty cycle
            avg_val = sum(nipkow_stream) / TOTAL_PIXELS
            self.average_duty_cycle = (avg_val / 255.0) * self.brightness

    def get_telemetry(self) -> Dict[str, object]:
        with self._lock:
            return {
                "driver": "MockLEDModulator",
                "pin": self.pin,
                "brightness": round(self.brightness, 2),
                "is_blanked": self.is_blanked,
                "safety_tripped": self.safety_tripped,
                "frames_rendered": self.frames_rendered,
                "average_duty_cycle": round(self.average_duty_cycle, 3),
                "connected": True,
            }


class GPIOLEDModulator(BaseLEDModulator):
    """Real-time GPIO LED pulse modulator using sysfs, gpiod, or RPi.GPIO.

    Decoupled into an asynchronous worker thread so the interrupt callback
    returns immediately without blocking the optical sensor detector.
    """

    def __init__(self, pin: int = 21, watchdog_timeout: float = 0.35):
        self.pin = pin
        self.watchdog_timeout = watchdog_timeout
        self.brightness = 0.7
        self.is_blanked = True
        self.safety_tripped = False
        self.last_watchdog_time = time.monotonic()
        self.frames_rendered = 0
        self.average_duty_cycle = 0.0
        self._lock = threading.Lock()
        self._backend = "none"
        self._gpio = None
        self._init_gpio()

        # Decoupled pulse worker
        self._running = True
        self._frame_ready = threading.Event()
        self._pending_stream: Optional[bytes] = None
        self._pending_period: float = 0.1
        self._worker_thread = threading.Thread(
            target=self._pulse_loop, daemon=True, name="gpio-led-pulse-worker"
        )
        self._worker_thread.start()

    def _init_gpio(self) -> None:
        # Try RPi.GPIO
        try:
            import RPi.GPIO as GPIO
            GPIO.setmode(GPIO.BCM)
            GPIO.setup(self.pin, GPIO.OUT, initial=GPIO.LOW)
            self._gpio = GPIO
            self._backend = "RPi.GPIO"
            return
        except Exception:
            pass

        # Try gpiod
        try:
            import gpiod
            chip = gpiod.Chip("gpiochip0")
            line = chip.get_line(self.pin)
            line.request(consumer="mechanical-tv-led", type=gpiod.LINE_REQ_DIR_OUT, default_val=0)
            self._gpio = line
            self._backend = "gpiod"
            return
        except Exception:
            pass

        self._backend = "mock-fallback"

    def _set_pin(self, val: int) -> None:
        if self._backend == "RPi.GPIO" and self._gpio:
            self._gpio.output(self.pin, val)
        elif self._backend == "gpiod" and self._gpio:
            self._gpio.set_value(val)

    def set_brightness(self, brightness: float) -> None:
        with self._lock:
            self.brightness = max(0.0, min(1.0, float(brightness)))

    def feed_watchdog(self) -> None:
        with self._lock:
            self.last_watchdog_time = time.monotonic()
            self.safety_tripped = False

    def blank(self) -> None:
        with self._lock:
            self.is_blanked = True
            self._set_pin(0)

    def output_frame(self, nipkow_stream: bytes, frame_period_s: float) -> None:
        """Hand off frame to the pulse worker in < 2 us."""
        now = time.monotonic()
        with self._lock:
            if now - self.last_watchdog_time > self.watchdog_timeout:
                self.safety_tripped = True
                self.blank()
                return

            if len(nipkow_stream) != TOTAL_PIXELS:
                return

            self.is_blanked = False
            self._pending_stream = nipkow_stream
            self._pending_period = frame_period_s
            self._frame_ready.set()

    def _pulse_loop(self) -> None:
        while self._running:
            if not self._frame_ready.wait(timeout=0.2):
                # Idle: enforce pin LOW
                self._set_pin(0)
                continue

            self._frame_ready.clear()
            with self._lock:
                stream = self._pending_stream
                period = self._pending_period
                b_scale = self.brightness
                blanked = self.is_blanked
                tripped = self.safety_tripped

            if blanked or tripped or not stream:
                self._set_pin(0)
                continue

            pixel_duration = period / TOTAL_PIXELS
            total_duty = 0.0

            t_next = time.perf_counter()
            for val in stream:
                if self.is_blanked or not self._running:
                    self._set_pin(0)
                    break

                duty = (val / 255.0) * b_scale
                total_duty += duty
                pulse_time = pixel_duration * duty

                if pulse_time > 0.000005:  # >= 5 microseconds
                    self._set_pin(1)
                    t_off = time.perf_counter() + pulse_time
                    while time.perf_counter() < t_off:
                        pass
                    self._set_pin(0)
                else:
                    self._set_pin(0)

                t_next += pixel_duration
                while time.perf_counter() < t_next:
                    pass

            with self._lock:
                self.frames_rendered += 1
                self.average_duty_cycle = total_duty / TOTAL_PIXELS

    def close(self) -> None:
        self._running = False
        self._frame_ready.set()
        self.blank()
        if self._worker_thread.is_alive():
            self._worker_thread.join(timeout=1.0)

    def get_telemetry(self) -> Dict[str, object]:
        with self._lock:
            return {
                "driver": "GPIOLEDModulator",
                "backend": self._backend,
                "pin": self.pin,
                "brightness": round(self.brightness, 2),
                "is_blanked": self.is_blanked,
                "safety_tripped": self.safety_tripped,
                "frames_rendered": self.frames_rendered,
                "average_duty_cycle": round(self.average_duty_cycle, 3),
                "connected": bool(self._backend != "none"),
            }


class SPILEDModulator(BaseLEDModulator):
    """Hardware SPI MOSI bitstream generator for zero-jitter LED pulses via DMA.

    Uses vectorized byte slice assembly and chunked transfers <= 4096 bytes
    to satisfy Linux spidev kernel buffer limits.
    """

    CHUNK_SIZE = 4096  # Standard Linux spidev default buffer limit

    def __init__(self, spidev_path: str = "/dev/spidev0.0", speed_hz: int = 2000000):
        self.spidev_path = spidev_path
        self.speed_hz = speed_hz
        self.brightness = 0.7
        self.is_blanked = True
        self.safety_tripped = False
        self.last_watchdog_time = time.monotonic()
        self.frames_rendered = 0
        self.average_duty_cycle = 0.0
        self._lock = threading.Lock()
        self._spi = None
        self._init_spi()

        # Decoupled SPI worker thread
        self._running = True
        self._frame_ready = threading.Event()
        self._pending_stream: Optional[bytes] = None
        self._pending_period: float = 0.1
        self._worker_thread = threading.Thread(
            target=self._spi_loop, daemon=True, name="spi-led-worker"
        )
        self._worker_thread.start()

    def _init_spi(self) -> None:
        try:
            import spidev
            self._spi = spidev.SpiDev()
            self._spi.open(0, 0)
            self._spi.max_speed_hz = self.speed_hz
            self._spi.mode = 0
        except Exception:
            self._spi = None

    def set_brightness(self, brightness: float) -> None:
        with self._lock:
            self.brightness = max(0.0, min(1.0, float(brightness)))

    def feed_watchdog(self) -> None:
        with self._lock:
            self.last_watchdog_time = time.monotonic()
            self.safety_tripped = False

    def blank(self) -> None:
        with self._lock:
            self.is_blanked = True
            if self._spi:
                try:
                    self._spi.writebytes([0] * 64)
                except Exception:
                    pass

    def output_frame(self, nipkow_stream: bytes, frame_period_s: float) -> None:
        now = time.monotonic()
        with self._lock:
            if now - self.last_watchdog_time > 0.35:
                self.safety_tripped = True
                self.blank()
                return

            if not self._spi or len(nipkow_stream) != TOTAL_PIXELS:
                return

            self.is_blanked = False
            self._pending_stream = nipkow_stream
            self._pending_period = frame_period_s
            self._frame_ready.set()

    def _spi_loop(self) -> None:
        while self._running:
            if not self._frame_ready.wait(timeout=0.2):
                continue
            self._frame_ready.clear()

            with self._lock:
                stream = self._pending_stream
                period = self._pending_period
                b_scale = self.brightness
                blanked = self.is_blanked
                tripped = self.safety_tripped

            if blanked or tripped or not stream or not self._spi:
                continue

            # Vectorized bitstream assembly:
            bits_per_pix = max(1, int(round((self.speed_hz * period) / TOTAL_PIXELS)))
            bytes_per_pix = (bits_per_pix + 7) // 8

            buffer = bytearray(TOTAL_PIXELS * bytes_per_pix)
            total_duty = 0.0

            for p_idx, val in enumerate(stream):
                duty = (val / 255.0) * b_scale
                total_duty += duty
                on_bits = int(round(bits_per_pix * duty))
                buf_offset = p_idx * bytes_per_pix

                full_bytes = on_bits // 8
                rem_bits = on_bits % 8

                # Fast slice assignment
                if full_bytes > 0:
                    buffer[buf_offset : buf_offset + full_bytes] = b"\xff" * full_bytes
                if rem_bits > 0:
                    buffer[buf_offset + full_bytes] = (0xFF << (8 - rem_bits)) & 0xFF

            # Chunked DMA transmission <= 4096 bytes per ioctl
            chunk_size = self.CHUNK_SIZE
            try:
                for i in range(0, len(buffer), chunk_size):
                    if self.is_blanked or not self._running:
                        break
                    chunk = buffer[i : i + chunk_size]
                    if hasattr(self._spi, "writebytes2"):
                        self._spi.writebytes2(chunk)
                    else:
                        self._spi.writebytes(list(chunk))
            except Exception:
                pass

            with self._lock:
                self.frames_rendered += 1
                self.average_duty_cycle = total_duty / TOTAL_PIXELS

    def close(self) -> None:
        self._running = False
        self._frame_ready.set()
        self.blank()
        if self._worker_thread.is_alive():
            self._worker_thread.join(timeout=1.0)
        if self._spi:
            try:
                self._spi.close()
            except Exception:
                pass
            self._spi = None

    def get_telemetry(self) -> Dict[str, object]:
        with self._lock:
            return {
                "driver": "SPILEDModulator",
                "spidev": self.spidev_path,
                "speed_hz": self.speed_hz,
                "brightness": round(self.brightness, 2),
                "is_blanked": self.is_blanked,
                "safety_tripped": self.safety_tripped,
                "frames_rendered": self.frames_rendered,
                "average_duty_cycle": round(self.average_duty_cycle, 3),
                "connected": bool(self._spi is not None),
            }


def create_led_modulator(
    pin: int = 21, use_spi: bool = False, force_mock: bool = False
) -> BaseLEDModulator:
    """Factory creating the LED modulator."""
    if force_mock or os.environ.get("MTV_MOCK_HARDWARE") == "1":
        return MockLEDModulator(pin=pin)

    if use_spi and os.path.exists("/dev/spidev0.0"):
        try:
            return SPILEDModulator()
        except Exception:
            pass

    try:
        return GPIOLEDModulator(pin=pin)
    except Exception:
        pass

    return MockLEDModulator(pin=pin)
