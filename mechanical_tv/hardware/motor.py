"""Adafruit DC & Stepper Motor HAT driver for Raspberry Pi and NEMA 17 motor.

Communicates with the PCA9685 PWM controller over I2C (address 0x60)
to drive the TB6612 H-bridges for Stepper 1 (M1/M2) or Stepper 2 (M3/M4).
Provides smooth velocity profiling / acceleration ramping to prevent motor stall.
"""

from abc import ABC, abstractmethod
import os
import threading
import time
from typing import Dict, List, Optional


# PCA9685 Registers
PCA9685_ADDRESS = 0x60
MODE1 = 0x00
MODE2 = 0x01
SUBADR1 = 0x02
SUBADR2 = 0x03
SUBADR3 = 0x04
PRESCALE = 0xFE
LED0_ON_L = 0x06
LED0_ON_H = 0x07
LED0_OFF_L = 0x08
LED0_OFF_H = 0x09
ALL_LED_ON_L = 0xFA
ALL_LED_ON_H = 0xFB
ALL_LED_OFF_L = 0xFC
ALL_LED_OFF_H = 0xFD

# Stepper Pin Assignments on Adafruit Motor HAT
# Port 1: M1 (Coil A) & M2 (Coil B) - Contiguous channels 8 through 13
PORT1_PINS = {
    "pwm_a": 8, "in2_a": 9, "in1_a": 10,
    "in1_b": 11, "in2_b": 12, "pwm_b": 13
}
# Port 2: M3 (Coil A) & M4 (Coil B) - Contiguous channels 2 through 7
PORT2_PINS = {
    "pwm_a": 2, "in2_a": 3, "in1_a": 4,
    "in1_b": 5, "in2_b": 6, "pwm_b": 7
}

# 8-step half-step sequence (Interleave)
# (coil_a_dir, coil_b_dir) where 1=forward, -1=reverse, 0=off
HALF_STEP_SEQUENCE = [
    (1, 0),
    (1, 1),
    (0, 1),
    (-1, 1),
    (-1, 0),
    (-1, -1),
    (0, -1),
    (1, -1),
]

# 4-step full-step sequence (Double coil for maximum torque)
FULL_STEP_SEQUENCE = [
    (1, 1),
    (-1, 1),
    (-1, -1),
    (1, -1),
]

# Precomputed PCA9685 4-byte pin states:
# ON = Full ON (bit 4 of ON_H)
PIN_FULL_ON = [0x00, 0x10, 0x00, 0x00]
# OFF = Full OFF (bit 4 of OFF_H)
PIN_FULL_OFF = [0x00, 0x00, 0x00, 0x10]


def _build_step_block(coil_a: int, coil_b: int) -> List[int]:
    """Build the 24-byte payload covering the 6 contiguous channels:

    [pwm_a, in2_a, in1_a, in1_b, in2_b, pwm_b]
    """
    # Coil A
    if coil_a > 0:
        pwm_a = PIN_FULL_ON
        in2_a = PIN_FULL_OFF
        in1_a = PIN_FULL_ON
    elif coil_a < 0:
        pwm_a = PIN_FULL_ON
        in2_a = PIN_FULL_ON
        in1_a = PIN_FULL_OFF
    else:
        pwm_a = PIN_FULL_OFF
        in2_a = PIN_FULL_OFF
        in1_a = PIN_FULL_OFF

    # Coil B
    if coil_b > 0:
        in1_b = PIN_FULL_ON
        in2_b = PIN_FULL_OFF
        pwm_b = PIN_FULL_ON
    elif coil_b < 0:
        in1_b = PIN_FULL_OFF
        in2_b = PIN_FULL_ON
        pwm_b = PIN_FULL_ON
    else:
        in1_b = PIN_FULL_OFF
        in2_b = PIN_FULL_OFF
        pwm_b = PIN_FULL_OFF

    return pwm_a + in2_a + in1_a + in1_b + in2_b + pwm_b


class BaseMotorDriver(ABC):
    """Abstract interface for stepper motor drivers."""

    @abstractmethod
    def step(self, direction: int = 1) -> None:
        """Perform a single step or half-step."""
        pass

    @abstractmethod
    def release(self) -> None:
        """De-energize coils to prevent heating and save power."""
        pass

    @abstractmethod
    def get_telemetry(self) -> Dict[str, object]:
        pass


class MockMotorDriver(BaseMotorDriver):
    """Simulated motor driver for development and testing without physical I2C hardware."""

    def __init__(self, port: int = 1):
        self.port = port
        self.steps_per_rev = 400
        self.step_count = 0
        self.released = True
        self.last_step_time = time.monotonic()
        self.direction = 1
        self.steps_per_rev = 400
        self.half_step_mode = True

    def step(self, direction: int = 1) -> None:
        self.direction = direction
        self.step_count += direction
        self.released = False
        self.last_step_time = time.monotonic()

    def release(self) -> None:
        self.released = True

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "driver": "MockMotorDriver",
            "port": self.port,
            "step_count": self.step_count,
            "released": self.released,
            "connected": True,
        }


class AdafruitMotorHATDriver(BaseMotorDriver):
    """High-speed batched I2C driver for the Adafruit Motor HAT PCA9685 controller."""

    def __init__(self, port: int = 1, address: int = PCA9685_ADDRESS, i2c_bus: int = 1):
        self.port = port
        self.address = address
        self.i2c_bus = i2c_bus
        self.pins = PORT1_PINS if port == 1 else PORT2_PINS
        self._base_reg = LED0_ON_L + 4 * (8 if port == 1 else 2)
        self.step_index = 0
        self.step_count = 0
        self.released = True
        self.half_step_mode = True
        self.steps_per_rev = 400 if self.half_step_mode else 200
        self.sequence = HALF_STEP_SEQUENCE if self.half_step_mode else FULL_STEP_SEQUENCE
        self._step_table: List[List[int]] = []
        self._release_block = PIN_FULL_OFF * 6
        self._rebuild_tables()
        self._bus = None
        self._lock = threading.Lock()
        self._init_i2c()

    def _rebuild_tables(self) -> None:
        self.sequence = HALF_STEP_SEQUENCE if self.half_step_mode else FULL_STEP_SEQUENCE
        self.steps_per_rev = 400 if self.half_step_mode else 200
        self._step_table = [_build_step_block(ca, cb) for ca, cb in self.sequence]

    def set_stepping_mode(self, half_step: bool) -> None:
        """Switch between half-step (400 steps/rev) and full-step (200 steps/rev)."""
        with self._lock:
            self.half_step_mode = half_step
            self._rebuild_tables()
            self.step_index = 0

    def _init_i2c(self) -> None:
        try:
            import smbus2
            self._bus = smbus2.SMBus(self.i2c_bus)
            self._write_byte(MODE1, 0x00)  # Normal mode
            self._write_byte(MODE2, 0x04)  # Totem pole outputs
            # Set PWM frequency to ~1600 Hz
            prescale_val = int(round(25000000.0 / (4096 * 1600.0)) - 1)
            old_mode = self._read_byte(MODE1)
            self._write_byte(MODE1, (old_mode & 0x7F) | 0x10)  # Sleep mode to set prescale
            self._write_byte(PRESCALE, prescale_val)
            self._write_byte(MODE1, old_mode)
            time.sleep(0.005)
            self._write_byte(MODE1, old_mode | 0xa1)  # Auto-increment + restart
            self.release()
        except Exception as exc:
            self._bus = None
            raise RuntimeError(f"Could not initialize I2C bus {self.i2c_bus} at 0x{self.address:02x}: {exc}")

    def _write_byte(self, reg: int, val: int) -> None:
        if self._bus:
            self._bus.write_byte_data(self.address, reg, val)

    def _read_byte(self, reg: int) -> int:
        if self._bus:
            return self._bus.read_byte_data(self.address, reg)
        return 0

    def _set_pwm(self, channel: int, on: int, off: int) -> None:
        if not self._bus:
            return
        base = LED0_ON_L + 4 * channel
        self._bus.write_i2c_block_data(
            self.address, base, [on & 0xFF, (on >> 8) & 0xFF, off & 0xFF, (off >> 8) & 0xFF]
        )

    def _set_pin(self, channel: int, value: int) -> None:
        if value:
            self._set_pwm(channel, 4096, 0)
        else:
            self._set_pwm(channel, 0, 4096)

    def step(self, direction: int = 1) -> None:
        """Advance one step via a single 24-byte batched I2C block transaction."""
        with self._lock:
            if not self._bus:
                return
            self.released = False
            self.step_index = (self.step_index + direction) % len(self.sequence)
            block = self._step_table[self.step_index]
            # Single I2C block write across all 6 channels simultaneously
            self._bus.write_i2c_block_data(self.address, self._base_reg, block)
            self.step_count += direction

    def release(self) -> None:
        """De-energize all coils in a single 24-byte I2C transaction."""
        with self._lock:
            if not self._bus:
                return
            self._bus.write_i2c_block_data(self.address, self._base_reg, self._release_block)
            self.released = True

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "driver": "AdafruitMotorHATDriver",
            "port": self.port,
            "address": hex(self.address),
            "step_count": self.step_count,
            "released": self.released,
            "mode": "half-step" if self.half_step_mode else "full-step",
            "steps_per_rev": self.steps_per_rev,
            "connected": bool(self._bus is not None),
        }


class CircuitPythonMotorKitDriver(BaseMotorDriver):
    """Wrapper around adafruit_motorkit.MotorKit if the CircuitPython stack is used."""

    def __init__(self, port: int = 1, address: int = PCA9685_ADDRESS):
        self.port = port
        self.steps_per_rev = 400
        self.step_count = 0
        self.released = True
        try:
            from adafruit_motorkit import MotorKit
            from adafruit_motor import stepper
            self._kit = MotorKit(address=address)
            self._stepper = self._kit.stepper1 if port == 1 else self._kit.stepper2
            self._stepper_mod = stepper
            self.release()
        except Exception as exc:
            raise RuntimeError(f"adafruit_motorkit initialization failed: {exc}")

    def step(self, direction: int = 1) -> None:
        dir_val = self._stepper_mod.FORWARD if direction >= 0 else self._stepper_mod.BACKWARD
        self._stepper.onestep(direction=dir_val, style=self._stepper_mod.INTERLEAVE)
        self.step_count += direction
        self.released = False

    def release(self) -> None:
        self._stepper.release()
        self.released = True

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "driver": "CircuitPythonMotorKitDriver",
            "port": self.port,
            "step_count": self.step_count,
            "released": self.released,
            "steps_per_rev": self.steps_per_rev,
            "connected": True,
        }


def create_motor_driver(
    port: int = 1, force_mock: bool = False, address: int = PCA9685_ADDRESS
) -> BaseMotorDriver:
    """Factory creating the appropriate driver based on environment and availability."""
    if force_mock or os.environ.get("MTV_MOCK_HARDWARE") == "1":
        return MockMotorDriver(port=port)

    # Try native I2C smbus2
    try:
        return AdafruitMotorHATDriver(port=port, address=address)
    except Exception:
        pass

    # Try CircuitPython MotorKit
    try:
        return CircuitPythonMotorKitDriver(port=port, address=address)
    except Exception:
        pass

    # Fallback to Mock
    return MockMotorDriver(port=port)


class MotorController:
    """High-level velocity and acceleration manager for the NEMA 17 stepper motor.

    Smoothly ramps rotational speed up and down to prevent inertia stalls.
    Supports closed-loop timing trimming from the opto sensor with zero-drift phase accumulation.
    """

    STEPS_PER_REV = 400  # 400 half-steps per rev (NEMA 17 200 full steps * 2)

    def __init__(
        self,
        driver: Optional[BaseMotorDriver] = None,
        target_fps: float = 10.0,
        ramp_rate_fps: float = 2.0,
    ):
        self.driver = driver or MockMotorDriver()
        self.target_fps = target_fps
        self.current_fps = 0.0
        self.ramp_rate_fps = ramp_rate_fps  # FPS increase per second
        self.is_running = False
        self._trim_us = 0.0  # Total revolution timing trim in microseconds from PLL
        self._thread: Optional[threading.Thread] = None
        self._lock = threading.Lock()
        self._stop_event = threading.Event()

    def start(self) -> None:
        with self._lock:
            if self.is_running:
                return
            self.is_running = True
            self._stop_event.clear()
            self._thread = threading.Thread(target=self._run_loop, daemon=True, name="motor-stepper-worker")
            self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)
        self._thread = None
        with self._lock:
            self.is_running = False
            self.current_fps = 0.0
            self.driver.release()

    def set_target_fps(self, fps: float) -> None:
        with self._lock:
            self.target_fps = max(1.0, min(20.0, float(fps)))

    def apply_trim_us(self, trim_us: float) -> None:
        """Apply closed-loop timing adjustment (in microseconds per revolution) from index sensor PLL."""
        with self._lock:
            self._trim_us = max(-5000.0, min(5000.0, float(trim_us)))

    def _run_loop(self) -> None:
        last_ramp_time = time.monotonic()
        # Start at 1.0 FPS for soft start
        with self._lock:
            self.current_fps = 1.0

        t_next = time.perf_counter()

        while not self._stop_event.is_set():
            t_now = time.monotonic()
            dt_ramp = t_now - last_ramp_time

            # Velocity Ramp
            if dt_ramp >= 0.05:
                ramp_step = self.ramp_rate_fps * dt_ramp
                with self._lock:
                    if self.current_fps < self.target_fps:
                        self.current_fps = min(self.target_fps, self.current_fps + ramp_step)
                    elif self.current_fps > self.target_fps:
                        self.current_fps = max(self.target_fps, self.current_fps - ramp_step)
                last_ramp_time = t_now

            steps_per_rev = getattr(self.driver, "steps_per_rev", self.STEPS_PER_REV)
            with self._lock:
                trim_us = self._trim_us
                cur_fps = self.current_fps

            # Distribute total revolution trim evenly across all steps in the revolution
            per_step_trim = (trim_us * 1e-6) / steps_per_rev
            step_delay = (1.0 / (cur_fps * steps_per_rev)) + per_step_trim
            step_delay = max(0.00005, step_delay)

            # Step the motor
            self.driver.step(1)

            # Incremental phase accumulator to eliminate cumulative timing drift
            t_next += step_delay
            now = time.perf_counter()
            # If accumulator fell significantly behind (e.g. initial start or heavy I2C stall), resync
            if t_next < now - 0.05:
                t_next = now + step_delay

            rem = t_next - time.perf_counter()
            if rem > 0.002:
                time.sleep(rem - 0.001)
            while time.perf_counter() < t_next:
                pass

        self.driver.release()

    def get_telemetry(self) -> Dict[str, object]:
        return {
            "is_running": self.is_running,
            "target_fps": self.target_fps,
            "target_rpm": self.target_fps * 60.0,
            "current_fps": round(self.current_fps, 2),
            "current_rpm": round(self.current_fps * 60.0, 1),
            "trim_us": self._trim_us,
            "driver": self.driver.get_telemetry(),
        }
