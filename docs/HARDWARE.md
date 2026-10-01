# Hardware Specification and Realtime Wiring Guide

This document specifies the verified electrical connections, pinouts, timing calculations, and safety rules for the real-time Mechanical Television apparatus.

```
       +-----------------------------------------------------------+
       |                  Raspberry Pi 4 Model B                   |
       |                                                           |
       |   [USB 3.0 Port] <====== HDMI Capture Dongle (/dev/video0)|
       |                                                           |
       |   40-Pin Header:                                          |
       |     * I2C SDA (GPIO 2, Pin 3)  ===> Adafruit Motor HAT    |
       |     * I2C SCL (GPIO 3, Pin 5)  ===> Adafruit Motor HAT    |
       |     * Opto In (GPIO 4, Pin 7)  <=== Index Opto Sensor     |
       |     * LED Out (GPIO 21, Pin 40)===> L298N IN1 / ENA       |
       |     * (Alt SPI MOSI, Pin 19)   ===> Zero-Jitter LED DMA   |
       |     * 3.3V Power (Pin 1)       ===> Opto Sensor VCC       |
       |     * Ground (Pins 6, 9, 14)   ===> Common System GND     |
       +-----------------------------------------------------------+
                                  |
            +---------------------+---------------------+
            |                                           |
            v                                           v
+-------------------------------+           +---------------------------+
|  Adafruit Motor HAT (PCA9685) |           |   L298N LED Driver Board  |
|                               |           |                           |
|  * I2C Address: 0x60          |           |  * Logic IN1: Pi GPIO 21  |
|  * Ext Power: 9–12V DC        |           |  * Power Input: 9–12V DC  |
|  * Port M1/M2: NEMA 17 Stepper|           |  * Output: 3W High-Power  |
|    - Coil A (M1): Blk / Grn   |           |    White LED + Resistor   |
|    - Coil B (M2): Red / Blu   |           |    on Aluminum Heatsink   |
+-------------------------------+           +---------------------------+
            |                                           |
            v                                           v
+-------------------------------+           +---------------------------+
| STEPPERONLINE NEMA 17 Stepper |           | Photoelectric Opto Sensor |
| 1.8° Bipolar (200 steps/rev)  |           | Transmissive Interrupter  |
| Direct drive to Nipkow disk   |           | Reads disk notch 1x/rev   |
+-------------------------------+           +---------------------------+
```

---

## Verified Components

| Subsystem | Part | Specification |
| --- | --- | --- |
| **Main Computer** | Raspberry Pi 4 Model B | 64-bit OS, USB 3.0 for capture, I2C bus 1, hardware timers |
| **Motor HAT** | Adafruit DC & Stepper Motor HAT (Mini Kit) | PCA9685 16-channel 12-bit PWM controller (I2C 0x60), dual TB6612 H-bridges (1.2 A continuous, 3 A peak) |
| **Drive Motor** | STEPPERONLINE 17HE08-1004S NEMA 17 | 1.8° step angle (200 full steps/rev, 400 half-steps/rev), 1 A/phase, 3.6 Ω resistance, 4 mH inductance |
| **Index Sensor** | Slotted Photoelectric Interrupter | Transmissive optical switch reading synchronization notch on outer rim of Nipkow disk once per revolution |
| **LED Driver** | L298N Dual H-Bridge | Switched via Pi GPIO 21 (or SPI MOSI Pin 19) to modulate LED current |
| **Light Source** | 3W High-Power LED | White (or 3W RGB for later phase), mounted on finned aluminum heatsink |
| **Video Ingest** | USB HDMI Video Capture (UVC / V4L2) | MacroSilicon MS2109 or standard CamLink dongle presenting as `/dev/video0` |
| **Motor Supply** | Minidodoca Adjustable 3–24 V (Set to 9–12 V) | Dedicated power supply for motor HAT and LED power rails |

---

## Wiring Table

### 1. Raspberry Pi 4 GPIO Pinout

| Physical Pin | BCM GPIO | Function | Destination |
| --- | --- | --- | --- |
| **Pin 1** | — | 3.3 V DC Power | Photoelectric index sensor VCC |
| **Pin 3** | GPIO 2 | I2C1 SDA | Adafruit Motor HAT SDA |
| **Pin 5** | GPIO 3 | I2C1 SCL | Adafruit Motor HAT SCL |
| **Pin 7** | GPIO 4 | Discrete Input (Pulled Down) | Photoelectric index sensor Signal Output |
| **Pin 9** | — | Ground | Common Ground |
| **Pin 19** | GPIO 10 | SPI0 MOSI (Hardware DMA) | L298N ENA / IN1 (when SPI mode enabled) |
| **Pin 40** | GPIO 21 | Discrete Output | L298N IN1 (Standard GPIO PWM mode) |

### 2. Adafruit Motor HAT to NEMA 17 Stepper

The STEPPERONLINE 17HE08-1004S has 4 leads arranged into two independent phase coils:
- **Phase A:** Black wire & Green wire -> Connect to **M1** terminal block.
- **Phase B:** Red wire & Blue wire -> Connect to **M2** terminal block.
- **Power Terminal Block:** Connect `+` and `-` to the Minidodoca power supply adjusted to **9.0 V – 12.0 V DC**.

> [!CAUTION]
> **DO NOT connect the motor supply above 12 V.** The TB6612 drivers on the Adafruit HAT cannot withstand 24 V, and the low-resistance NEMA 17 winding (3.6 Ω) will draw excessive current at higher voltages.
> **DO NOT install the logic power jumper** that ties Pi 5V to the motor power rail. The Pi must run exclusively on its dedicated USB-C power supply.

### 3. Photoelectric Index Sensor

- **VCC:** Connect to Pi Pin 1 (3.3 V).
- **GND:** Connect to Pi Pin 9 (GND).
- **Signal:** Connect to Pi Pin 7 (BCM GPIO 4).
- The software configures an internal pull-down resistor (`GPIO.PUD_DOWN`). When the optical slot passes between the emitter and detector, the signal transitions HIGH.

### 4. L298N LED Modulation Board

- **VCC (12V Input):** Connected to external 9–12 V rail.
- **GND:** Connected to external supply GND **AND** Raspberry Pi Pin 6/9/14 (Common Ground).
- **ENA / IN1:** Connected to Raspberry Pi Pin 40 (BCM GPIO 21) or Pin 19 (SPI MOSI).
- **OUT1 & OUT2:** Connected in series with a 3W LED and a 2.7 Ω – 3.3 Ω, 5 W current-limiting power resistor (or a dedicated 700 mA buck constant-current LED driver).
- The 3W LED generates significant heat: **mount it to a finned aluminum star heatsink with thermal paste.**

---

## Timing and Synchronization Mechanics

### Nipkow Disk Geometry (bitluni Reference)

- **Total Holes:** 32 spiraled holes arranged in an Archimedean spiral.
- **Vertical Resolution per Hole:** 25 lines.
- **Total Pixels per Revolution:** $32 \times 25 = 800\text{ pixels}$.
- **Target Frame Rate:** 10.0 frames per second (FPS).
- **Target Motor Speed:** $10.0\text{ rev/s} \times 60 = 600.0\text{ RPM}$.
- **Revolution Duration ($T_{rev}$):** $100.0\text{ ms} = 0.1\text{ s}$.
- **Pixel Slot Duration ($T_{pixel}$):** $\frac{100.0\text{ ms}}{800} = 125.0\ \mu\text{s}$ (8.0 kHz pixel clock).

### Motor Stepping Rate

- The NEMA 17 motor has a 1.8° step angle (200 full steps per revolution).
- In half-step mode (interleave 8-step commutation):
  $$\text{Steps per rev} = 400\text{ half-steps/rev}$$
- At 10.0 FPS (600 RPM):
  $$\text{Stepping frequency} = 400 \times 10.0 = 4,000\text{ steps/second}$$
  $$\text{Step interval} = \frac{1}{4,000} = 250\ \mu\text{s}$$

### Coordinate Mapping Formula

Matching bitluni's hardware disk layout:
$$\text{For pixel index } i \in [0, 799]:$$
$$c = i // 25 \quad (c \in [0, 31] \text{ column/hole index})$$
$$r = i \% 25 \quad (r \in [0, 24] \text{ line index})$$
$$x = 31 - c \quad (\text{or } c \text{ if horizontal inverted})$$
$$y = 24 - r \quad (\text{or } r \text{ if vertical inverted})$$
$$\text{Shifted index } i' = (i + \text{phase\_offset}) \pmod{800}$$

### LED Pulse Modulation

Within each $125\ \mu\text{s}$ pixel slot:
- The 8-bit grayscale intensity $V \in [0, 255]$ maps to pulse width:
  $$\text{Duty Cycle} = \left(\frac{V}{255}\right) \times \text{Master Brightness}$$
  $$\text{Pulse Duration } t_{on} = 125\ \mu\text{s} \times \text{Duty Cycle}$$
- During $t_{on}$, the GPIO pin is HIGH (LED energized).
- During the remainder ($125\ \mu\text{s} - t_{on}$), the GPIO pin is LOW (LED blanked).

---

## Safety Features

1. **Velocity Ramping (Soft Start):** Stepper motors cannot instantaneously accelerate from 0 to 600 RPM without stalling. The software controller accelerates smoothly from 1.0 FPS up to target 10.0 FPS over 2 seconds.
2. **Thermal & Stall Watchdog:** If the motor stalls, if the disk is stopped, or if no optical index pulse arrives within 350 ms, the software automatically cuts power to the LED pin (`blank()`).
3. **Coil De-Energizing:** When the motor is commanded to stop, all TB6612 H-bridge outputs are disabled (`release()`), preventing the motor from drawing holding current and overheating while stationary.
4. **Emergency Blank Button:** A dedicated prominent button in the web UI immediately shuts down LED modulation and halts the drive motor.
