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

MAX_FPS = 24.0  # speed ceiling: 1440 RPM; above ~20 FPS the stepper may stall and the LED pixel slot drops below 55 us


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

        # Free-run: an internal clock at target_fps drives the LED and preview instead of the
        # index sensor, for bench tests with no disk (or no sensor) fitted. The watchdog is fed
        # by that clock, so the LED stays on without a spinning disk: see set_free_run().
        self.free_run = False
        self._free_run_thread: Optional[threading.Thread] = None
        self._free_run_stop = threading.Event()
        self.free_run_frames = 0

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
        with self._lock:
            if not self._running:
                self.state = "idle"  # the supervisor may have written 'ramping' while the motor wound down

    def emergency_blank(self) -> None:
        """Immediately cut power to LED and stop motor. Also leaves free-run, which would relight the LED."""
        with self._lock:
            self.led.blank()
        self.set_free_run(False)
        self.stop()
        with self._lock:
            self.led.blank()
            self.state = "idle"

    # -- free-run output clock ----------------------------------------------

    def set_free_run(self, enabled: bool) -> None:
        """Drive the LED and preview from an internal clock at target_fps, ignoring the index sensor.

        While enabled the LED fires whether or not the disk spins or is even fitted: the operator
        is choosing to bypass the stall interlock, for bench checks of the video path. Index
        pulses still trim the motor speed; they no longer clock frames. Disabling blanks the LED
        and hands output back to the sensor, which relights it on the next index pulse.
        """
        enabled = bool(enabled)
        with self._lock:
            if enabled == self.free_run:
                return
            self.free_run = enabled
            if enabled:
                self._free_run_stop.clear()
                self._free_run_thread = threading.Thread(
                    target=self._free_run_loop, daemon=True, name="realtime-free-run"
                )
                self._free_run_thread.start()
                return
            self._free_run_stop.set()
            thread, self._free_run_thread = self._free_run_thread, None
        if thread and thread.is_alive() and thread is not threading.current_thread():
            thread.join(timeout=1.5)
        with self._lock:
            self.led.blank()

    def _free_run_loop(self) -> None:
        stop = self._free_run_stop
        t_next = time.perf_counter()
        while not stop.is_set():
            period = 1.0 / max(1.0, self.target_fps)
            self.led.feed_watchdog()
            self._drive_frame(time.perf_counter(), period)
            with self._lock:
                self.free_run_frames += 1
            t_next += period
            delay = t_next - time.perf_counter()
            if delay <= 0:
                t_next = time.perf_counter()  # fell behind (busy CPU): resume from now, never burst
                continue
            stop.wait(delay)

    def set_target_fps(self, fps: float) -> None:
        with self._lock:
            self.target_fps = max(1.0, min(MAX_FPS, float(fps)))
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

        # Closed loop PLL speed trim:
        # Only trim when motor is near operating speed (>= 80%) to avoid winding up during acceleration ramp
        if self.motor.current_fps >= self.target_fps * 0.8:
            target_period = 1.0 / self.target_fps
            period_error = target_period - period
            trim_us = self._kp * period_error * 1e6
            self.motor.apply_trim_us(trim_us)
        else:
            self.motor.apply_trim_us(0.0)

        if self.free_run:
            return  # the free-run clock owns the LED, watchdog and preview

        self.led.feed_watchdog()
        self._drive_frame(timestamp, period)

    def _drive_frame(self, timestamp: float, period: float) -> None:
        """Serialize the newest frame for the disk, pulse the LED across `period`, store the preview."""
        # 1. Fetch latest video frame and serialize for Nipkow disk
        frame = self.pipeline.get_latest_frame()
        nipkow_stream = self.pipeline.serialize_nipkow_stream(frame)

        # 2. Output modulated pulses to LED
        self.led.output_frame(nipkow_stream, period)

        # 3. Store preview frame for UI
        with self._lock:
            self.frame_counter += 1
            self.current_preview_frame = frame
            self._last_rev_time = timestamp

    def _supervise_loop(self) -> None:
        """Background watchdog supervisor checking lock status and thermal safety."""
        while self._running:
            time.sleep(0.1)
            with self._lock:
                if not self._running:
                    break  # stop() ran during the sleep: leave its 'idle' state alone
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
                "free_run": self.free_run,
                "free_run_frames": self.free_run_frames,
                "output_clock": "free-run" if self.free_run else "index-sensor",
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
        self.set_free_run(False)
        self.stop()
        self.led.close()
        self.pipeline.close()
