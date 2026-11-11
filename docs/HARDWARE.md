# Hardware and wiring

```
        +--------------------------------------------------------------+
        |                    Raspberry Pi 4 Model B                    |
        |  CAMERA (CSI-2) port  <== TC358743 HDMI-to-CSI adapter        |
        |  GPIO 18 (pin 12)     ==> A4988 STEP   (hardware PWM)         |
        |  GPIO 23 (pin 16)     ==> A4988 DIR                           |
        |  GPIO 24 (pin 18)     ==> A4988 ENABLE (low = on)             |
        |  GPIO 4  (pin 7)      <== index sensor output                 |
        |  GPIO 10 (pin 19)     ==> LED driver input (SPI output, best) |
        |   or GPIO 21 (pin 40) ==> LED driver input (GPIO output)      |
        |  3.3 V (pin 1/17)     ==> A4988 VDD, MS1-MS3, sensor VCC      |
        |  GND (pins 6, 9, 14)  ==> common ground                       |
        +--------------------------------------------------------------+
```

## Parts

| Subsystem | Part | Notes |
| --- | --- | --- |
| Computer | Raspberry Pi 4 Model B (4 GB) | Raspberry Pi OS 64-bit (Trixie or Bookworm), own USB-C supply |
| Motor | STEPPERONLINE 17HE08-1004S NEMA 17 | 1.8°, 1.0 A/phase, 3.6 Ω, 4 mH, 17 N·cm, rotor 22 g·cm² |
| Motor driver | A4988 STEP/DIR module (DRV8825 and TMC2209 also work; their current formulas differ, see below) | Default. The Adafruit DC & Stepper Motor HAT is still supported (`MTV_MOTOR_DRIVER=hat`) |
| Index sensor | Slotted photo-interrupter | Reads the disk's notch once per revolution |
| LED | 3 W white LED on a heatsink | Switched by an L298N channel (a logic-level MOSFET or constant-current driver is better) |
| Video input | GeeekPi/52Pi HDMI-to-CSI-2 adapter (Toshiba TC358743) | On the CAMERA ribbon port; raw UYVY, no encoding. A USB UVC dongle also works |
| Motor supply | Adjustable 3-24 V supply | 12 V minimum for the A4988; 24 V gives the most torque at 600 RPM |

## A4988 motor driver

| A4988 pin | Connect to |
| --- | --- |
| VMOT, GND | Motor supply (12-24 V) with a ≥ 47 µF electrolytic capacitor right at the pins |
| VDD, GND | Pi 3.3 V and ground (not 5 V) |
| STEP | GPIO 18, pin 12 (hardware PWM: `dtoverlay=pwm,pin=18,func=2`) |
| DIR | GPIO 23, pin 16 (`MTV_DIR_GPIO`) |
| ENABLE | GPIO 24, pin 18 (`MTV_ENABLE_GPIO`); held high from power-on by `gpio=24=op,dh` |
| MS1, MS2, MS3 | **All three to 3.3 V for 1/16 microstepping.** They have internal pull-downs: left open, the driver full-steps and every speed is 16× too fast |
| RESET, SLEEP | Tied together (and so high) |
| 1A/1B, 2A/2B | Motor coils: Black-Green to one pair, Red-Blue to the other (3.6 Ω each, open between pairs) |

**Current limit.** Set Vref by measurement, never by pot position, with I = Vref / (8 × Rs). For
0.7-0.8 A, use 0.56-0.64 V with 0.1 Ω sense resistors (marked R100), 0.38-0.44 V with 0.068 Ω
(R068), or 0.28-0.32 V with 0.05 Ω (R050). Fit a heatsink: above about 1 A a bare A4988 reaches
thermal shutdown within seconds. Check the microstep setting with **Check microstepping** in the
console; `MTV_MICROSTEPS` must match the jumpers.

DRV8825: 1/16 is MS1 low, MS2 low, MS3 high (M0, M1 low, M2 high), and 1/32 is all high. Its
current is I = 2 × Vref, so 0.35-0.40 V gives 0.7-0.8 A. **Do not use the A4988 values**: they
overdrive the motor. TMC2209 (standalone module): 1/16 is MS1 and MS2 high. The Vref formula
depends on the module (for example BIGTREETECH: I_rms ≈ Vref × 0.71); follow the module's sheet.
Set `MTV_MICROSTEPS` to match whichever mode is wired.

## Index sensor

| Sensor pin | Connect to |
| --- | --- |
| VCC | 3.3 V |
| GND | Ground |
| Signal | GPIO 4, pin 7 (`MTV_OPTO_PIN`). The app enables the internal pull-down (`MTV_OPTO_PULL=down`; `up`, or `none` for a sensor with its own pull resistor) and counts rising edges |

The rim passes through the slot. Measure the slot width: the 1.75 mm plate needs clearance on both
faces, and bitluni's printed holder leaves only 0.4 mm on the hub side. Turn the disk by hand
through a full revolution with the motor released and make sure it never touches.

## LED

Two outputs are available. Choose one with `MTV_LED_OUTPUT` (the installer writes it):

| Output | Wiring | Timing |
| --- | --- | --- |
| `spi` (recommended) | L298N IN1 from **GPIO 10 / SPI0 MOSI, pin 19** | DMA bitstream at 2 MHz: 0.5 µs pulse resolution, no software timing inside a revolution. Needs `dtparam=spi=on`, `core_freq_fixed=1` and `spidev.bufsiz=65536`, which the installer adds |
| `gpio` | L298N IN1 from **GPIO 21, pin 40** (`MTV_LED_PIN`) | Bit-banged by the realtime thread through `/dev/gpiomem`. A few µs of jitter; under heavy load a pixel can be skipped |

**L298N:**

- 9-12 V on its supply input, GND common with the Pi.
- IN1 takes the signal, IN2 goes to GND, and the ENA jumper stays fitted.
- The LED and a series resistor go across OUT1/OUT2.

Size the resistor for about 0.7 A. The L298N drops about 2 V and a white 3 W LED about 3.4 V:

| Supply | Resistor for 0.7 A | Resistor rating |
| --- | --- | --- |
| 12 V | 10 Ω | 10 W (5 W at full duty) |
| 9 V | 5.1 Ω | 5 W |

The 2.7-3.3 Ω value in older notes passes about 2 A at 12 V, more than the LED or an L298N channel
should carry. A 700 mA constant-current LED driver, or a logic-level MOSFET with a resistor, is better
than the L298N. The L298N's own switching delays (1-2 µs) limit the real pulse resolution, not the
SPI's 0.5 µs. Heatsink the LED.

## HDMI input (TC358743)

- **Connector:** the Pi 4 **CAMERA** socket, between the micro-HDMI ports and the audio jack. The
  DISPLAY socket does not work, and the adapter's red LED only shows 3.3 V from the ribbon.
- **Cable:** 15-pin, 1 mm pitch, two lanes on the Pi 4.
- **Bandwidth:** 1080p30 uses about half the two-lane link; 720p60 fits; 1080p60 does not. The
  shipped EDID advertises only 1080p30 and 720p60.
- **No encoding:** the bridge outputs raw pixels. H.264 in vendor examples is the Pi's encoder
  saving a file; this app never encodes or decodes.
- **Setup:** `camera_auto_detect=0` and `dtoverlay=tc358743` in `/boot/firmware/config.txt`. The app
  loads the EDID and locks the timings itself (see [INSTALL.md](INSTALL.md#hdmi-input)).

## Adafruit Motor HAT (alternative)

The HAT is selected with `MTV_MOTOR_DRIVER=hat`. It sits on the header (I2C SDA GPIO 2, SCL
GPIO 3, address 0x60), with motor coils on M1/M2 and a 9-12 V supply on its power terminals. Do not
exceed 12 V: the TB6612 bridges are rated for 13.5 V and 1.2 A. Steps are sent by I2C from a
realtime thread in half-step mode (400 per revolution; `MTV_STEP_MODE=full` for 200). Each step is a
12-24 byte I2C write. At the Pi's default 100 kHz bus that is only about 750 steps per second, about
2 FPS in half-step, so the installer sets `dtparam=i2c_arm_baudrate=400000` for the HAT. That gives
an estimated 3,000 steps per second, about 7 FPS. The A4988 has no such limit.

## Timing

| Quantity | Value at 10 FPS (600 RPM) |
| --- | --- |
| Pixels per revolution | 32 holes × 25 lines = 800 |
| Revolution | 100 ms |
| Pixel slot | 125 µs (8 kHz) |
| A4988 STEP rate at 1/16 | 3,200 × 10 = 32,000 steps/s |
| PWM period | 31,250 ns, quantised to the 50 MHz PWM clock: 31,260 ns, −0.03 % |
| SPI bits per revolution | 200,000 at 2 MHz (250 per pixel, 25 KB) |

Pixel `i` maps to hole `c = i / 25` and line `r = i % 25`, shown at `x = 31 − c`, `y = 24 − r`
(bitluni's spiral). The phase offset rotates the pixel index, and the flips mirror x or y.

The printed disk is bitluni's geometry at a 1.75 mm plate (about 81 g): 32 square 1.0 mm holes at
r = 84.5 + i mm, 11.25° apart, with the notch at −168.75°. The parametric model `nipkow_disk.scad`
is kept with the 3D model files.

## Safety

1. **Interlock:** the LED is blanked when no index pulse arrives for 350 ms, or 1.5 revolutions if
   longer. It is never lit below 0.9 measured FPS.
2. **Stall stop:** no pulses while commanded to spin, or the wrong number of steps per revolution,
   stops the motor and blanks the LED with a message.
3. **Emergency blank** turns off the LED, stops the motor and leaves free-run.
4. **Free-run bypasses the interlock** on purpose for bench tests. Never leave it on with a stopped
   disk under the 3 W LED.
5. **Coils are released when stopped** (A4988 ENABLE high, HAT outputs off), and ENABLE is held high
   from power-on until the app starts the motor.
6. **Exits and crashes turn everything off.** Logout, shutdown and `kill` take the same path as
   Ctrl+Q. A crash stops the PWM, raises ENABLE and drops the LED line from the signal handler.
   `mechanical-tv-hw.service` does the same again at system shutdown. GPIO outputs otherwise keep
   their last level after a program ends.
