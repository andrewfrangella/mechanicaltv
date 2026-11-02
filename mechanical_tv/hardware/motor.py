"""Adafruit DC & Stepper Motor HAT driver for Raspberry Pi and NEMA 17 motor.

Communicates with the PCA9685 PWM controller over I2C (address 0x60)
to drive the TB6612 H-bridges for Stepper 1 (M1/M2) or Stepper 2 (M3/M4).
Provides smooth velocity profiling / acceleration ramping to prevent motor stall.

Realtime note: on real hardware the stepping loop runs in its own process
(not a thread), so the web server, capture and sensor threads cannot delay
steps. At low speed a late step shifts the timing instead of firing a burst of
catch-up steps (which a stepper cannot follow); at speed only a short backlog is
caught up.
"""

from abc import ABC, abstractmethod
import glob
import multiprocessing as mp
import os
import signal
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
    """High-speed batched I2C driver for the Adafruit Motor HAT PCA9685 controller.

    Each step only rewrites the coil whose state changed (12 bytes instead of 24),
    halving I2C time per step so higher speeds fit in the bus bandwidth.
    """

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
        self._last_state = None  # (coil_a, coil_b) last written; None = unknown/released
        self._rebuild_tables()
        self._bus = None
        self._lock = threading.Lock()
        self._init_i2c()

    def _rebuild_tables(self) -> None:
        self.sequence = HALF_STEP_SEQUENCE if self.half_step_mode else FULL_STEP_SEQUENCE
        self.steps_per_rev = 400 if self.half_step_mode else 200
        self._step_table = [_build_step_block(ca, cb) for ca, cb in self.sequence]
        self._last_state = None

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
        """Advance one step, writing only the coil that changed (one I2C block write)."""
        with self._lock:
            if not self._bus:
                return
            self.released = False
            self.step_index = (self.step_index + direction) % len(self.sequence)
            new_state = self.sequence[self.step_index]
            block = self._step_table[self.step_index]
            last = self._last_state
            if last is None or (last[0] != new_state[0] and last[1] != new_state[1]):
                # First step after release / mode change: write both coils
                self._bus.write_i2c_block_data(self.address, self._base_reg, block)
            elif last[0] != new_state[0]:
                # Coil A channels (pwm_a, in2_a, in1_a)
                self._bus.write_i2c_block_data(self.address, self._base_reg, block[0:12])
            elif last[1] != new_state[1]:
                # Coil B channels (in1_b, in2_b, pwm_b)
                self._bus.write_i2c_block_data(self.address, self._base_reg + 12, block[12:24])
            self._last_state = new_state
            self.step_count += direction

    def release(self) -> None:
        """De-energize all coils in a single 24-byte I2C transaction."""
        with self._lock:
            if not self._bus:
                return
            self._bus.write_i2c_block_data(self.address, self._base_reg, self._release_block)
            self._last_state = None
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


class A4988PWMDriver(BaseMotorDriver):
    """STEP/DIR stepper driver (A4988, DRV8825, TMC2209 ...) clocked by the Pi's hardware PWM.

    STEP pulses come from the PWM peripheral, so the step rate is exact and nothing else
    in the app can disturb it; software only changes the rate (ramping / PLL trim).
    Needs 'dtoverlay=pwm,pin=18,func=2' in /boot/firmware/config.txt (STEP on GPIO 18).
    DIR and ENABLE are ordinary GPIOs; ENABLE low = driver on, high = coils released.
    """

    def __init__(self, dir_gpio: int = 23, enable_gpio: int = 24, microsteps: int = 16,
                 direction: int = 1, pwm_chip: Optional[str] = None, pwm_channel: int = 0):
        import lgpio
        self._lg = lgpio
        self.port = 0
        self.address = None
        self.microsteps = int(microsteps)
        self.steps_per_rev = 200 * self.microsteps
        self.half_step_mode = False
        self.step_count = 0.0
        self.released = True
        self.direction = 1 if direction >= 0 else -1
        self._rate = 0.0
        self._rate_since = time.monotonic()
        self._lock = threading.Lock()
        self._dir_gpio = dir_gpio
        self._en_gpio = enable_gpio
        self._h = lgpio.gpiochip_open(int(os.environ.get("MTV_GPIOCHIP", "0")))
        lgpio.gpio_claim_output(self._h, enable_gpio, 1)  # start with the driver OFF
        lgpio.gpio_claim_output(self._h, dir_gpio, 0 if self.direction > 0 else 1)
        chips = [pwm_chip] if pwm_chip else sorted(glob.glob("/sys/class/pwm/pwmchip*"))
        if not chips:
            raise RuntimeError("No hardware PWM found: add 'dtoverlay=pwm,pin=18,func=2' to "
                               "/boot/firmware/config.txt and reboot")
        chip = chips[0]
        self._pwm = os.path.join(chip, f"pwm{pwm_channel}")
        if not os.path.isdir(self._pwm):
            self._w(os.path.join(chip, "export"), pwm_channel)
            for _ in range(50):
                if os.path.exists(os.path.join(self._pwm, "period")):
                    break
                time.sleep(0.02)
        self._f_period = os.path.join(self._pwm, "period")
        self._f_duty = os.path.join(self._pwm, "duty_cycle")
        self._f_enable = os.path.join(self._pwm, "enable")
        self._w(self._f_enable, 0)
        self._period = self._r(self._f_period)

    @staticmethod
    def _w(path: str, value) -> None:
        with open(path, "w") as f:
            f.write(str(value))

    @staticmethod
    def _r(path: str) -> int:
        try:
            with open(path) as f:
                return int(f.read().strip() or 0)
        except (OSError, ValueError):
            return 0

    def _account(self) -> None:
        now = time.monotonic()
        self.step_count += self.direction * self._rate * (now - self._rate_since)
        self._rate_since = now

    def set_rate(self, steps_per_sec: float) -> None:
        """Set the STEP pulse rate (micro-steps per second); 0 stops the pulses."""
        with self._lock:
            self._account()
            if steps_per_sec <= 0:
                if self._rate > 0:
                    self._w(self._f_enable, 0)
                self._rate = 0.0
                return
            period = max(2000, int(round(1e9 / steps_per_sec)))  # >= 2 us: A4988 needs 1 us high + 1 us low
            if self._period and period < self._period:
                # speeding up: the kernel needs duty <= period at every moment, so shrink duty first
                self._w(self._f_duty, period // 2)
                self._w(self._f_period, period)
            else:
                self._w(self._f_period, period)
                self._w(self._f_duty, period // 2)
            self._period = period
            if self._rate == 0.0:
                self._lg.gpio_write(self._h, self._en_gpio, 0)  # driver ON
                self._w(self._f_enable, 1)
            self.released = False
            self._rate = 1e9 / period

    def set_direction(self, direction: int) -> None:
        with self._lock:
            self._account()
            self.direction = 1 if direction >= 0 else -1
            self._lg.gpio_write(self._h, self._dir_gpio, 0 if self.direction > 0 else 1)

    def step(self, direction: int = 1) -> None:
        """Single software steps aren't used with hardware PWM (MotorController sets a rate)."""
        return

    def release(self) -> None:
        self.set_rate(0)
        with self._lock:
            self._lg.gpio_write(self._h, self._en_gpio, 1)  # driver OFF, coils de-energized
            self.released = True

    def get_telemetry(self) -> Dict[str, object]:
        with self._lock:
            self._account()
            return {
                "driver": "A4988PWMDriver",
                "microsteps": self.microsteps,
                "steps_per_rev": self.steps_per_rev,
                "step_rate_hz": round(self._rate, 1),
                "step_count": int(self.step_count),
                "released": self.released,
                "mode": f"1/{self.microsteps} microstep",
                "connected": True,
            }


def create_motor_driver(
    port: int = 1, force_mock: bool = False, address: int = PCA9685_ADDRESS
) -> BaseMotorDriver:
    """Factory creating the appropriate driver based on environment and availability."""
    if force_mock or os.environ.get("MTV_MOCK_HARDWARE") == "1":
        return MockMotorDriver(port=port)

    # STEP/DIR driver (A4988 etc.) on the Pi's hardware PWM, selected with MTV_MOTOR_DRIVER=a4988.
    # Errors are not swallowed: with this selected, the HAT has no motor attached.
    if os.environ.get("MTV_MOTOR_DRIVER", "").strip().lower() in ("a4988", "stepdir", "drv8825", "tmc2209"):
        return A4988PWMDriver(
            dir_gpio=int(os.environ.get("MTV_DIR_GPIO", "23")),
            enable_gpio=int(os.environ.get("MTV_ENABLE_GPIO", "24")),
            microsteps=int(os.environ.get("MTV_MICROSTEPS", "16")),
            direction=int(os.environ.get("MTV_MOTOR_DIRECTION", "1")),
            pwm_chip=os.environ.get("MTV_PWM_CHIP") or None,
        )

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


def _step_delay(cur_fps: float, steps_per_rev: int, trim_us: float) -> float:
    # Distribute total revolution trim evenly across all steps in the revolution
    per_step_trim = (trim_us * 1e-6) / steps_per_rev
    return max(0.00005, (1.0 / (cur_fps * steps_per_rev)) + per_step_trim)


def _resync(t_next: float, now: float, step_delay: float, cur_fps: float) -> float:
    """Decide what to do when a step goes out late.

    Below 3 FPS the rotor follows each step, so any backlog is dropped (no bursts).
    Above that the disk's momentum carries the rotor, so a short backlog (up to 3 ms)
    is caught up to keep the coils in phase with the spinning disk.
    """
    max_backlog = step_delay if cur_fps < 3.0 else max(step_delay, 0.003)
    if t_next < now - max_backlog:
        return now
    return t_next


def _wait_until(t_next: float) -> None:
    rem = t_next - time.perf_counter()
    if rem > 0.002:
        time.sleep(rem - 0.001)
    while time.perf_counter() < t_next:
        pass


def _stepper_process_main(port, address, half_step, v_target, v_current, v_trim, v_ramp,
                          v_steps, stop_evt, parent_pid) -> None:
    """Stepping loop that runs in its own process (no shared interpreter lock with the app)."""
    # Stop cleanly (coils released) on service stop; let the parent handle Ctrl-C
    signal.signal(signal.SIGTERM, lambda *_: stop_evt.set())
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    driver = None
    try:
        driver = AdafruitMotorHATDriver(port=port, address=address)
        driver.set_stepping_mode(bool(half_step))
        steps_per_rev = driver.steps_per_rev
        cur_fps = 1.0  # soft start
        v_current.value = cur_fps
        last_ramp_time = time.monotonic()
        t_next = time.perf_counter()
        while True:
            t_now = time.monotonic()
            dt_ramp = t_now - last_ramp_time
            if dt_ramp >= 0.05:
                if stop_evt.is_set() or os.getppid() != parent_pid:
                    break  # asked to stop, or the app died: never keep spinning unattended
                target = v_target.value
                ramp_step = v_ramp.value * dt_ramp
                if cur_fps < target:
                    cur_fps = min(target, cur_fps + ramp_step)
                elif cur_fps > target:
                    cur_fps = max(target, cur_fps - ramp_step)
                v_current.value = cur_fps
                last_ramp_time = t_now

            step_delay = _step_delay(cur_fps, steps_per_rev, v_trim.value)
            driver.step(1)
            v_steps.value += 1

            t_next = _resync(t_next + step_delay, time.perf_counter(), step_delay, cur_fps)
            _wait_until(t_next)
    finally:
        v_current.value = 0.0
        if driver is not None:
            try:
                driver.release()
            except Exception:
                pass


class MotorController:
    """High-level velocity and acceleration manager for the NEMA 17 stepper motor.

    Smoothly ramps rotational speed up and down to prevent inertia stalls.
    Supports closed-loop timing trimming from the opto sensor with zero-drift phase accumulation.

    With an A4988-style driver (MTV_MOTOR_DRIVER=a4988) the step pulses come from the
    Pi's hardware PWM and a light thread only updates the rate every 20 ms.
    With the Adafruit HAT driver, stepping runs in a separate process so other
    parts of the app cannot delay steps. Mock / CircuitPython drivers keep the
    original in-process thread. Environment overrides:
      MTV_MOTOR_PROCESS=0   use the old thread instead of a separate process
      MTV_STEP_MODE=full    full-step (200 steps/rev) instead of half-step (400)
    """

    STEPS_PER_REV = 400  # 400 half-steps per rev (NEMA 17 200 full steps * 2)

    def __init__(
        self,
        driver: Optional[BaseMotorDriver] = None,
        target_fps: float = 10.0,
        ramp_rate_fps: float = 2.0,
    ):
        self.driver = driver or MockMotorDriver()
        is_hat = isinstance(self.driver, AdafruitMotorHATDriver)
        if is_hat and os.environ.get("MTV_STEP_MODE", "half").strip().lower() == "full":
            self.driver.set_stepping_mode(False)
        self._use_process = (
            is_hat and os.name == "posix" and os.environ.get("MTV_MOTOR_PROCESS", "1") != "0"
        )
        self._use_pwm = isinstance(self.driver, A4988PWMDriver)
        self._ctx = mp.get_context("fork") if self._use_process else None
        # Shared numbers (plain shared memory, readable by the stepping process)
        self._v_target = mp.RawValue("d", float(target_fps))
        self._v_current = mp.RawValue("d", 0.0)
        self._v_trim = mp.RawValue("d", 0.0)
        self._v_ramp = mp.RawValue("d", float(ramp_rate_fps))
        self._v_steps = mp.RawValue("q", 0)
        self.is_running = False
        self._thread: Optional[threading.Thread] = None
        self._proc = None
        self._stop_mp = None
        self._lock = threading.Lock()
        self._stop_event = threading.Event()

    # --- shared-value properties (same attribute names as before) ---
    @property
    def target_fps(self) -> float:
        return self._v_target.value

    @target_fps.setter
    def target_fps(self, value: float) -> None:
        self._v_target.value = float(value)

    @property
    def current_fps(self) -> float:
        return self._v_current.value

    @current_fps.setter
    def current_fps(self, value: float) -> None:
        self._v_current.value = float(value)

    @property
    def ramp_rate_fps(self) -> float:
        return self._v_ramp.value

    @ramp_rate_fps.setter
    def ramp_rate_fps(self, value: float) -> None:
        self._v_ramp.value = float(value)

    @property
    def _trim_us(self) -> float:
        return self._v_trim.value

    @_trim_us.setter
    def _trim_us(self, value: float) -> None:
        self._v_trim.value = float(value)

    def start(self) -> None:
        with self._lock:
            if self.is_running:
                return
            self.is_running = True
            if self._use_process:
                self._stop_mp = self._ctx.Event()
                self._proc = self._ctx.Process(
                    target=_stepper_process_main,
                    args=(
                        self.driver.port, self.driver.address, self.driver.half_step_mode,
                        self._v_target, self._v_current, self._v_trim, self._v_ramp,
                        self._v_steps, self._stop_mp, os.getpid(),
                    ),
                    name="motor-stepper",
                    daemon=True,
                )
                self._proc.start()
            else:
                self._stop_event.clear()
                target = self._pwm_loop if self._use_pwm else self._run_loop
                self._thread = threading.Thread(target=target, daemon=True, name="motor-stepper-worker")
                self._thread.start()

    def stop(self) -> None:
        if self._use_process:
            if self._stop_mp is not None:
                self._stop_mp.set()
            if self._proc is not None:
                self._proc.join(timeout=2.0)
                if self._proc.is_alive():
                    self._proc.terminate()
                    self._proc.join(timeout=1.0)
            self._proc = None
        else:
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

    def _pwm_loop(self) -> None:
        """Rate manager for hardware-PWM drivers: ramps speed and applies the PLL trim.

        The PWM peripheral generates every STEP pulse; this thread only changes the
        rate 50 times a second, so other threads cannot disturb the motor.
        """
        cur_fps = 0.5  # gentle soft start
        self.current_fps = cur_fps
        last = time.monotonic()
        spr = self.driver.steps_per_rev
        applied = 0.0  # step rate actually sent to the driver

        def desired_rate(fps: float) -> float:
            # trim is microseconds added to each revolution (same meaning as the step loop)
            rev_period = max(1e-3, 1.0 / fps + self._trim_us * 1e-6)
            return spr / rev_period

        try:
            applied = desired_rate(cur_fps)
            self.driver.set_rate(applied)
            while not self._stop_event.wait(0.02):
                now = time.monotonic()
                dt = now - last
                last = now
                target = self.target_fps
                ramp_step = self.ramp_rate_fps * dt
                if cur_fps < target:
                    cur_fps = min(target, cur_fps + ramp_step)
                elif cur_fps > target:
                    cur_fps = max(target, cur_fps - ramp_step)
                self.current_fps = cur_fps
                # Slew-limit the final rate: speed ramp + sync trim together may change speed at
                # most 2x the ramp rate, so a sudden trim (e.g. from a missed index pulse) can't
                # jerk the motor and make it lose sync.
                max_change = 2.0 * self.ramp_rate_fps * spr * dt
                want = desired_rate(cur_fps)
                applied += max(-max_change, min(max_change, want - applied))
                self.driver.set_rate(applied)
        finally:
            self.driver.set_rate(0)

    def _run_loop(self) -> None:
        """In-process stepping thread (mock / CircuitPython drivers)."""
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

            step_delay = _step_delay(cur_fps, steps_per_rev, trim_us)

            # Step the motor
            self.driver.step(1)
            self._v_steps.value += 1

            # Incremental phase accumulator (see _resync for how late steps are handled)
            t_next = _resync(t_next + step_delay, time.perf_counter(), step_delay, cur_fps)
            _wait_until(t_next)

        self.driver.release()

    def get_telemetry(self) -> Dict[str, object]:
        driver_tel = self.driver.get_telemetry()
        if self._use_process:
            driver_tel["step_count"] = int(self._v_steps.value)
            driver_tel["process_alive"] = bool(self._proc is not None and self._proc.is_alive())
        return {
            "is_running": self.is_running,
            "target_fps": self.target_fps,
            "target_rpm": self.target_fps * 60.0,
            "current_fps": round(self.current_fps, 2),
            "current_rpm": round(self.current_fps * 60.0, 1),
            "trim_us": self._trim_us,
            "driver": driver_tel,
        }
