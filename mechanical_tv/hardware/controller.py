"""Master Real-time Hardware Synchronization Controller for Mechanical TV.

Coordinates HDMI video ingest, Adafruit Motor HAT stepper commutation,
optical index sensor synchronization, and LED pulse modulation in real time.
"""

from collections import deque
import threading
import time
from typing import Dict, List, Optional, Union

from ..capture import FramePipeline, WIDTH, HEIGHT, FRAME_PIXELS
from .motor import MotorController, create_motor_driver
from .sensor import BaseOptoSensor, create_opto_sensor, MockOptoSensor
from .led import BaseLEDModulator, create_led_modulator


class RealtimeController:
    """Master controller orchestrating video capture, motor rotation,

    index sensor feedback, and LED pulse modulation.
    """

    def __init__(
        self,
        pipeline: Optional[FramePipeline] = None,
        motor_port: int = 1,
        opto_pin: int = 4,
        led_pin: int = 21,
        use_spi_led: bool = False,
        force_mock: bool = False,
        target_fps: float = 10.0,
    ):
        self.pipeline = pipeline or FramePipeline()
        self.target_fps = target_fps

        # Hardware drivers
        motor_driver = create_motor_driver(port=motor_port, force_mock=force_mock)
        self.motor = MotorController(driver=motor_driver, target_fps=target_fps)
        self.sensor = create_opto_sensor(pin=opto_pin, force_mock=force_mock)
        self.led = create_led_modulator(pin=led_pin, use_spi=use_spi_led, force_mock=force_mock)

        # If mock sensor, link it to the motor's target/current speed
        if isinstance(self.sensor, MockOptoSensor):
            self.sensor.set_simulated_fps(target_fps)

        self.state = "idle"  # 'idle', 'ramping', 'running', 'locked', 'error'
        self.frame_counter = 0
        self.current_preview_frame: bytes = bytes(FRAME_PIXELS)
        self._lock = threading.RLock()
        self._sync_thread: Optional[threading.Thread] = None
        self._running = False
        self._last_rev_time = 0.0
        self._error_msg = ""

        # Closed-loop PLL parameters
        self._kp = 0.05  # Proportional gain for motor step trim

    def start(self) -> None:
        """Start motor rotation and optical synchronization."""
        with self._lock:
            if self._running:
                return
            self._running = True
            self.state = "ramping"
            self._error_msg = ""

            # Start opto sensor with revolution callback
            self.sensor.start(callback=self._on_revolution)

            # Start motor
            self.motor.set_target_fps(self.target_fps)
            self.motor.start()

            # Start background sync supervisor
            self._sync_thread = threading.Thread(
                target=self._supervise_loop, daemon=True, name="realtime-sync-supervisor"
            )
            self._sync_thread.start()

    def stop(self) -> None:
        """Stop motor and safely blank LED without lock inversion deadlocks."""
        with self._lock:
            self._running = False
            self.state = "idle"
            self.led.blank()

        # Stop hardware outside lock to prevent deadlock with sensor callbacks
        self.motor.stop()
        self.sensor.stop()

        if self._sync_thread and self._sync_thread.is_alive():
            self._sync_thread.join(timeout=1.5)
        self._sync_thread = None

    def emergency_blank(self) -> None:
        """Immediately cut power to LED and stop motor."""
        with self._lock:
            self.led.blank()
        self.stop()
        with self._lock:
            self.state = "idle"

    def set_target_fps(self, fps: float) -> None:
        with self._lock:
            self.target_fps = max(1.0, min(20.0, float(fps)))
            self.motor.set_target_fps(self.target_fps)
            self.sensor.set_target_fps(self.target_fps)

    def set_source_mode(self, mode: str, library_path: Optional[str] = None) -> None:
        self.pipeline.set_source_mode(mode, library_path=library_path)

    def set_pattern(self, pattern: str) -> None:
        self.pipeline.pattern_gen.set_pattern(pattern)

    def set_calibration(
        self,
        phase_offset: Optional[int] = None,
        invert_x: Optional[bool] = None,
        invert_y: Optional[bool] = None,
        gamma: Optional[float] = None,
        brightness: Optional[float] = None,
        contrast: Optional[float] = None,
    ) -> None:
        self.pipeline.set_calibration(
            phase_offset=phase_offset,
            invert_x=invert_x,
            invert_y=invert_y,
            gamma=gamma,
            brightness=brightness,
            contrast=contrast,
        )
        if brightness is not None:
            self.led.set_brightness(self.pipeline.brightness)

    def _on_revolution(self, timestamp: float, period: float) -> None:
        """Executed on each rising edge of the optical index sensor."""
        if not self._running:
            return

        # 1. Feed LED watchdog
        self.led.feed_watchdog()

        # 2. Closed loop PLL speed trim:
        # Only trim when motor is near operating speed (>= 80%) to avoid winding up during acceleration ramp
        if self.motor.current_fps >= self.target_fps * 0.8:
            target_period = 1.0 / self.target_fps
            period_error = target_period - period
            trim_us = self._kp * period_error * 1e6
            self.motor.apply_trim_us(trim_us)
        else:
            self.motor.apply_trim_us(0.0)

        # 3. Fetch latest video frame and serialize for Nipkow disk
        frame = self.pipeline.get_latest_frame()
        nipkow_stream = self.pipeline.serialize_nipkow_stream(frame)

        # 4. Output modulated pulses to LED
        self.led.output_frame(nipkow_stream, period)

        # 5. Store preview frame for UI
        with self._lock:
            self.frame_counter += 1
            self.current_preview_frame = frame
            self._last_rev_time = timestamp

    def _supervise_loop(self) -> None:
        """Background watchdog supervisor checking lock status and thermal safety."""
        while self._running:
            time.sleep(0.1)
            with self._lock:
                # If mock sensor, update simulated speed to match motor ramp
                if isinstance(self.sensor, MockOptoSensor):
                    self.sensor.set_simulated_fps(self.motor.current_fps)

                sensor_telem = self.sensor.get_telemetry()
                is_locked = bool(sensor_telem.get("sync_locked", False))

                if self.motor.current_fps >= self.target_fps * 0.95 and is_locked:
                    self.state = "locked"
                elif self.motor.is_running:
                    self.state = "ramping" if self.motor.current_fps < self.target_fps * 0.9 else "running"

                # Check watchdog timeout: if no revolution in 0.5s while running
                if time.perf_counter() - self._last_rev_time > 0.5 and self.frame_counter > 0:
                    self.led.blank()
                    if self.state == "locked":
                        self.state = "running"

    def get_status(self) -> Dict[str, object]:
        """Aggregate telemetry from all subsystems for UI and diagnostics."""
        with self._lock:
            sensor_telem = self.sensor.get_telemetry()
            motor_telem = self.motor.get_telemetry()
            led_telem = self.led.get_telemetry()
            pipe_telem = self.pipeline.get_telemetry()

            return {
                "state": self.state,
                "target_fps": self.target_fps,
                "target_rpm": self.target_fps * 60.0,
                "measured_fps": sensor_telem.get("measured_fps", 0.0),
                "measured_rpm": sensor_telem.get("measured_rpm", 0.0),
                "sync_locked": sensor_telem.get("sync_locked", False),
                "jitter_ms": sensor_telem.get("jitter_ms", 0.0),
                "frame_counter": self.frame_counter,
                "error": self._error_msg,
                "motor": motor_telem,
                "sensor": sensor_telem,
                "led": led_telem,
                "pipeline": pipe_telem,
            }

    def get_preview_frame(self) -> bytes:
        with self._lock:
            if self.frame_counter > 0 and self.current_preview_frame:
                return self.current_preview_frame
            return self.pipeline.get_latest_frame()

    def close(self) -> None:
        self.stop()
        self.led.close()
        self.pipeline.close()
