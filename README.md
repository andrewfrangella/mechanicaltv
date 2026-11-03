# Mechanical TV

A real-time display and synchronization appliance for a Raspberry Pi mechanical television based on [bitluni's design](https://github.com/bitluni/MechanicalTV).

**Version 0.2.1 runs as a local desktop console on the Pi** (`python3 -m mechanical_tv.app`, see [LOCAL_APP.md](docs/LOCAL_APP.md)); the password-protected web interface remains available with `MTV_UI=web`. **Version 0.2.0 is a Realtime First release.** It captures live HDMI video or procedural test patterns, controls a NEMA 17 stepper motor via the Adafruit Motor HAT, tracks disk synchronization using a photoelectric index sensor, and modulates 800 pixels per revolution onto a 3W LED with thermal safety watchdog protection.

![Mechanical TV Studio showing live test pattern and 32 by 25 optical preview](docs/images/player-desktop.png)

## What works

- **Live HDMI Video Ingest:** Real-time V4L2 capture from an HDMI-to-CSI-2 adapter (Toshiba TC358743: GeeekPi/52Pi, Geekworm, Waveshare) or a USB UVC dongle, downscaled on the fly to 32 × 25 8-bit grayscale, with a colour preview of the full input picture in the local console. The CSI bridge is read in-process (V4L2 + numpy, one frame of delay); FFmpeg is the fallback and the USB path. For CSI adapters the service loads the EDID and locks DV timings itself; failures (no signal, dead capture, stalled stream) show in the UI and blank the disk instead of freezing.
- **Procedural Test Patterns:** SMPTE grayscale bars, horizontal/vertical ramps, aperture alignment grid, rotating phase sync bar, 1 Hz photodiode pulse test, and sweeping single column.
- **Motor Control & Velocity Profiling:** Drives a NEMA 17 stepper motor using the Adafruit DC & Stepper Motor HAT (PCA9685 @ I2C 0x60, TB6612 dual H-bridge). Soft-start acceleration ramping (1.0 to 10.0+ FPS / 60 to 600+ RPM) prevents motor inertia stall.
- **Closed-Loop Optical Sync:** Photoelectric index sensor (BCM GPIO 4) detects the once-per-revolution disk notch, measuring rotation period, instantaneous RPM, jitter, and phase lock. A closed-loop PLL trims stepper intervals to eliminate drift.
- **LED Pulse Modulation:** Modulates a 3W high-power LED (BCM GPIO 21 or SPI MOSI) across 800 pixels per frame matching bitluni's Nipkow disc geometry ($x = 31 - c$, $y = 24 - r$).
- **Thermal & Stall Watchdog:** Hardware safety interlock cuts LED power immediately if motor stops, disc speed drops below 1.0 FPS, or opto pulses cease, protecting the 3D-printed disk from heat damage.
- **Free-run output:** a checkbox clocks the LED and preview from an internal timer at the target speed instead of the index sensor, for bench tests with no disk or sensor fitted. The stall interlock is bypassed while it is on. Speed range is 1 to 24 FPS (60 to 1440 RPM).
- **Debug log:** a separate log window (F12) and a rotating log file keep every capture state change, action and error with tracebacks.
- **Optical Calibration:** Web-based phase offset slider ($0^\circ$ to $359^\circ$ / 0 to 799 px framing hold), horizontal/vertical scan inversion, optical gamma adjustment, and contrast tuning.
- **Stored Media Library:** Upload MP4/WebM/MOV videos for real-time looping or offline preview.
- **Universal Operation:** Runs on Raspberry Pi 4 Model B with physical hardware, and automatically provides high-fidelity hardware emulation when developed locally on macOS or non-Pi systems.

## Hardware setup

| Component | Specification | Connection / Notes |
| --- | --- | --- |
| **Main Computer** | Raspberry Pi 4 Model B | Raspberry Pi OS 64-bit |
| **Motor HAT** | Adafruit DC & Stepper Motor HAT | Placed on 40-pin GPIO header (I2C SDA GPIO 2, SCL GPIO 3) |
| **Stepper Motor** | STEPPERONLINE NEMA 17 (1.8°, 200 steps/rev) | Connected to Stepper 1 (terminals M1 & M2) |
| **Index Sensor** | Slotted photoelectric opto-interrupter | Signal to BCM GPIO 4 (Pin 7), GND, 3.3V |
| **LED Driver** | L298N or logic MOSFET driver | Logic input to BCM GPIO 21 (Pin 40) or SPI MOSI (Pin 19) |
| **Light Source** | 3W High-power White or RGB LED | Heat-sink mounted, driven via L298N from dedicated supply |
| **Power Supply** | Adjustable 9–12 V, 3 A supply | Connected to Adafruit Motor HAT external power terminals |
| **Video Input** | HDMI-to-CSI-2 adapter (TC358743, e.g. GeeekPi/52Pi) or USB UVC capture dongle | CSI adapter on the CAMERA ribbon port with `dtoverlay=tc358743`; USB dongle on a USB 3.0 port. Both appear as `/dev/video0`. See [INSTALL.md](docs/INSTALL.md#hdmi-input) |

> [!WARNING]
> Keep the Raspberry Pi on its dedicated official 5V/3A USB-C supply. Do not power the Pi from the motor supply. Always heatsink the 3W LED.

## Start on a development computer

Requires Python **3.11+**, FFmpeg and FFprobe. Runs in emulation mode without physical I2C/GPIO:

```bash
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
python3 -m mechanical_tv.server init --data ./data
python3 -m mechanical_tv.server serve --data ./data --mock-hardware
```

Open **http://127.0.0.1:8080** and sign in using the application password you just created.

## Install on Raspberry Pi OS

Use Raspberry Pi OS 64-bit (Debian Bookworm or newer; the installer requires Python 3.11+):

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

Run it from the desktop account that will operate the TV. The installer installs hardware packages (`i2c-tools`, `python3-smbus`, `python3-tk`, `v4l-utils`), adds that account to the `i2c`, `gpio`, `video` and `spi` groups, enables I2C, and installs the console as a desktop application that opens full screen with the desktop (desktop icon, and menu: Sound & Video > Mechanical TV). Log out and in once after the first install so the group memberships apply.

For the web interface instead, run `sudo MTV_UI=web bash install.sh` and open `http://<Pi-IP-address>:8080` or `http://<pi-hostname>.local:8080`.

## Documentation

| Document | Contents |
| --- | --- |
| [Local console](docs/LOCAL_APP.md) | The desktop app on the Pi: options, keys, free-run output, debug log, hardware access |
| [Hardware Guide](docs/HARDWARE.md) | Verified wiring diagram, Adafruit Motor HAT pinout, NEMA 17 connection, opto timing, and safety |
| [Architecture](docs/ARCHITECTURE.md) | Real-time synchronization loop, video capture pipeline, Nipkow coordinate mapping, and threading |
| [Project Plan](docs/PROJECT_PLAN.md) | Hardware bring-up status, synchronization milestones, and roadmap |
| [User Guide](docs/USER_GUIDE.md) | Realtime studio controls, optical calibration, test patterns, and troubleshooting |
| [Installation](docs/INSTALL.md) | Pi OS setup, HDMI-to-CSI adapter (overlay, EDID, troubleshooting), settings, logs, and maintenance |
| [Validation](docs/VALIDATION.md) | Test commands, test pattern verification, and physical bring-up checklist |

## Tests

```bash
python3 -m unittest discover -s tests -v
bash -n install.sh packaging/make-test-clip.sh
node --check mechanical_tv/static/app.js
```

## Attribution and licensing

Based on the mechanical television concept and geometry by [bitluni](https://github.com/bitluni/MechanicalTV). This repository contains independently developed real-time Python/Linux software for Raspberry Pi 4, Adafruit Motor HAT, and HDMI video ingest.
