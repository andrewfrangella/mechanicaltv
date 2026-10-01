"""Hardware abstraction and real-time controller package for Mechanical TV."""

from .motor import (
    BaseMotorDriver,
    MockMotorDriver,
    AdafruitMotorHATDriver,
    CircuitPythonMotorKitDriver,
    create_motor_driver,
    MotorController,
)
from .sensor import (
    BaseOptoSensor,
    MockOptoSensor,
    GPIOOptoSensor,
    create_opto_sensor,
)
from .led import (
    BaseLEDModulator,
    MockLEDModulator,
    GPIOLEDModulator,
    SPILEDModulator,
    create_led_modulator,
    TOTAL_PIXELS,
)
from .controller import RealtimeController

__all__ = [
    "BaseMotorDriver",
    "MockMotorDriver",
    "AdafruitMotorHATDriver",
    "CircuitPythonMotorKitDriver",
    "create_motor_driver",
    "MotorController",
    "BaseOptoSensor",
    "MockOptoSensor",
    "GPIOOptoSensor",
    "create_opto_sensor",
    "BaseLEDModulator",
    "MockLEDModulator",
    "GPIOLEDModulator",
    "SPILEDModulator",
    "create_led_modulator",
    "TOTAL_PIXELS",
    "RealtimeController",
]
