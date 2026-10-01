"""Optical index sensor driver for Nipkow disc synchronization.

Monitors the photoelectric interrupter once per revolution to determine
exact disc rotational period, RPM, jitter, and frame start alignment.
"""

from abc import ABC, abstractmethod
from collections import deque
import os
import threading
import time
from typing import Callable, Deque, Dict, List, Optional


class BaseOptoSensor(ABC):
    """Abstract interface for the photoelectric index sensor."""

    @abstractmethod
    def start(self, callback: Optional[Callable[[float, float], None]] = None) -> None:
        """Start listening for index pulses.

        callback(timestamp_seconds, period_seconds) is invoked on each pulse.
        """
        pass

    @abstractmethod
    def stop(self) -> None:
        pass

    @abstractmethod
    def set_target_fps(self, fps: float) -> None:
        """Update target frame rate for lock detection."""
        pass

    @abstractmethod
    def get_telemetry(self) -> Dict[str, object]:
        pass


class MockOptoSensor(BaseOptoSensor):
    """Simulated opto sensor for development and testing.

    Generates pulses based on a simulated or linked motor speed.
    """

    def __init__(self, pin: int = 4):
        self.pin = pin
        self._running = False
        self._callback: Optional[Callable[[float, float], None]] = None
        self._thread: Optional[threading.Thread] = None
        self.simulated_fps = 10.0
        self.pulse_count = 0
        self.last_pulse_time = 0.0
        self.measured_period = 0.1
        self.measured_fps = 10.0
        self.measured_rpm = 600.0
        self.jitter_ms = 0.0
        self.sync_locked = False
        self._lock = threading.Lock()
        self._period_history: Deque[float] = deque(maxlen=10)

    def set_target_fps(self, fps: float) -> None:
        self.set_simulated_fps(fps)

    def set_simulated_fps(self, fps: float) -> None:
        with self._lock:
            self.simulated_fps = max(0.5, float(fps))

    def trigger_pulse(self) -> None:
        """Manually trigger an index pulse (used by motor coupling or tests)."""
        now = time.perf_counter()
        with self._lock:
            if self.last_pulse_time > 0:
                period = now - self.last_pulse_time
                self.measured_period = period
                fps = 1.0 / period if period > 0 else 0.0
                self.measured_fps = fps
                self.measured_rpm = fps * 60.0
                if self._period_history:
                    avg_p = sum(self._period_history) / len(self._period_history)
                    self.jitter_ms = abs(period - avg_p) * 1000.0
                self._period_history.append(period)

                # Check lock: within 3% of simulated_fps and jitter < 2ms and at least 3 pulses
                if (
                    abs(fps - self.simulated_fps) / self.simulated_fps < 0.03
                    and self.jitter_ms < 2.0
                    and len(self._period_history) >= 3
                ):
                    self.sync_locked = True
                else:
                    self.sync_locked = False
            else:
                period = 1.0 / self.simulated_fps

            self.last_pulse_time = now
            self.pulse_count += 1
            cb = self._callback

        if cb:
            cb(now, period)

    def start(self, callback: Optional[Callable[[float, float], None]] = None) -> None:
        with self._lock:
            if self._running:
                return
            self._running = True
            self._callback = callback
            self.last_pulse_time = 0.0
            self._period_history.clear()
            self._thread = threading.Thread(target=self._loop, daemon=True, name="mock-opto-worker")
            self._thread.start()

    def stop(self) -> None:
        self._running = False
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None
        with self._lock:
            self.sync_locked = False

    def _loop(self) -> None:
        while self._running:
            with self._lock:
                fps = self.simulated_fps
            period = 1.0 / fps
            time.sleep(period)
            if self._running:
                self.trigger_pulse()

    def get_telemetry(self) -> Dict[str, object]:
        with self._lock:
            return {
                "driver": "MockOptoSensor",
                "pin": self.pin,
                "running": self._running,
                "pulse_count": self.pulse_count,
                "measured_fps": round(self.measured_fps, 2),
                "measured_rpm": round(self.measured_rpm, 1),
                "jitter_ms": round(self.jitter_ms, 2),
                "sync_locked": self.sync_locked,
                "connected": True,
            }


class GPIOOptoSensor(BaseOptoSensor):
    """Hardware opto sensor using Linux GPIO edge detection (gpiod / RPi.GPIO)."""

    def __init__(self, pin: int = 4, target_fps: float = 10.0):
        self.pin = pin
        self.target_fps = target_fps
        self._running = False
        self._callback: Optional[Callable[[float, float], None]] = None
        self._thread: Optional[threading.Thread] = None
        self.pulse_count = 0
        self.last_pulse_time = 0.0
        self.measured_period = 0.1
        self.measured_fps = 0.0
        self.measured_rpm = 0.0
        self.jitter_ms = 0.0
        self.sync_locked = False
        self._lock = threading.Lock()
        self._period_history: Deque[float] = deque(maxlen=10)
        self._backend = "none"

    def set_target_fps(self, fps: float) -> None:
        with self._lock:
            self.target_fps = max(1.0, min(20.0, float(fps)))

    def _handle_edge(self) -> None:
        now = time.perf_counter()
        with self._lock:
            if self.last_pulse_time > 0:
                period = now - self.last_pulse_time
                if period < 0.02:  # Debounce: max 50 rev/sec
                    return
                self.measured_period = period
                fps = 1.0 / period
                self.measured_fps = fps
                self.measured_rpm = fps * 60.0
                if self._period_history:
                    avg_p = sum(self._period_history) / len(self._period_history)
                    self.jitter_ms = abs(period - avg_p) * 1000.0
                self._period_history.append(period)

                # Check lock: within 3% of target_fps and jitter < 2ms
                if (
                    abs(fps - self.target_fps) / self.target_fps < 0.03
                    and self.jitter_ms < 2.0
                    and len(self._period_history) >= 5
                ):
                    self.sync_locked = True
                else:
                    self.sync_locked = False
            else:
                period = 1.0 / self.target_fps

            self.last_pulse_time = now
            self.pulse_count += 1
            cb = self._callback

        if cb:
            cb(now, period)

    def start(self, callback: Optional[Callable[[float, float], None]] = None) -> None:
        with self._lock:
            if self._running:
                return
            self._running = True
            self._callback = callback
            self.last_pulse_time = 0.0
            self._period_history.clear()

        # Try RPi.GPIO
        try:
            import RPi.GPIO as GPIO
            GPIO.setmode(GPIO.BCM)
            GPIO.setup(self.pin, GPIO.IN, pull_up_down=GPIO.PUD_DOWN)
            GPIO.add_event_detect(
                self.pin, GPIO.RISING, callback=lambda ch: self._handle_edge(), bouncetime=20
            )
            self._backend = "RPi.GPIO"
            return
        except Exception:
            pass

        # Try gpiod
        try:
            import gpiod
            self._backend = "gpiod"
            self._thread = threading.Thread(target=self._gpiod_loop, daemon=True, name="gpiod-opto-worker")
            self._thread.start()
            return
        except Exception:
            pass

        # Fallback to sysfs or mock
        self._backend = "mock-fallback"
        self._thread = threading.Thread(target=self._mock_loop, daemon=True, name="fallback-opto-worker")
        self._thread.start()

    def _gpiod_loop(self) -> None:
        try:
            import gpiod
            chip = gpiod.Chip("gpiochip0")
            line = chip.get_line(self.pin)
            line.request(consumer="mechanical-tv-opto", type=gpiod.LINE_REQ_EV_RISING_EDGE)
            while self._running:
                if line.event_wait(sec=1):
                    event = line.event_read()
                    if event:
                        self._handle_edge()
            line.release()
        except Exception:
            pass

    def _mock_loop(self) -> None:
        while self._running:
            period = 1.0 / self.target_fps
            time.sleep(period)
            if self._running:
                self._handle_edge()

    def stop(self) -> None:
        self._running = False
        if self._backend == "RPi.GPIO":
            try:
                import RPi.GPIO as GPIO
                GPIO.remove_event_detect(self.pin)
            except Exception:
                pass
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=1.0)
        self._thread = None
        with self._lock:
            self.sync_locked = False

    def get_telemetry(self) -> Dict[str, object]:
        with self._lock:
            return {
                "driver": "GPIOOptoSensor",
                "backend": self._backend,
                "pin": self.pin,
                "running": self._running,
                "pulse_count": self.pulse_count,
                "measured_fps": round(self.measured_fps, 2),
                "measured_rpm": round(self.measured_rpm, 1),
                "jitter_ms": round(self.jitter_ms, 2),
                "sync_locked": self.sync_locked,
                "connected": bool(self._backend != "none"),
            }


def create_opto_sensor(pin: int = 4, force_mock: bool = False) -> BaseOptoSensor:
    """Factory creating the appropriate opto sensor instance."""
    if force_mock or os.environ.get("MTV_MOCK_HARDWARE") == "1":
        return MockOptoSensor(pin=pin)

    try:
        import RPi.GPIO
        return GPIOOptoSensor(pin=pin)
    except ImportError:
        pass

    try:
        import gpiod
        return GPIOOptoSensor(pin=pin)
    except ImportError:
        pass

    return MockOptoSensor(pin=pin)
