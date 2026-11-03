# Installation on the Raspberry Pi

Use Raspberry Pi OS 64-bit (Trixie, or Bookworm) with the desktop, on a Raspberry Pi 4 with its own
USB-C supply. Run the installer from the desktop account that will operate the TV. Internet access
is needed only for the packages.

```bash
sudo apt update && sudo apt install -y git
git clone https://github.com/andrewfrangella/MechanicalTV.git mechanicaltv
cd mechanicaltv
git checkout cpp-port                      # until the C++ port is merged into main
sudo bash install.sh                       # A4988 motor driver, LED on GPIO 21
sudo MTV_LED_OUTPUT=spi bash install.sh    # LED on SPI0 MOSI (GPIO 10, pin 19): the precise output
sudo reboot
```

The console opens full screen when the desktop starts. It is also on the desktop and in the menu
under Sound & Video > Mechanical TV.

## What the installer does

1. **Packages.** Installs the compiler and Qt 6 (`build-essential cmake ninja-build qt6-base-dev
   libsqlite3-dev`), `ffmpeg`, `v4l-utils`, `i2c-tools` and `gpiod`, plus `rt-tests` and
   `edid-decode` for bench checks.
2. **Groups.** Adds the desktop user to `gpio`, `i2c`, `video` and `spi`.
3. **Build and test.** Compiles the app as that user and runs the test suite (about a minute;
   `MTV_RUN_TESTS=0` skips it). It installs nothing if a test fails.
4. **Release.** Installs to `/opt/mechanical-tv/releases/<timestamp>/` (`bin/mechanical-tv`,
   `share/mechanical-tv/edid/`) and points `/opt/mechanical-tv/current` at it. Earlier releases stay
   for rollback.
5. **Settings.** Writes `/etc/mechanical-tv/environment` if missing, and updates `MTV_MOTOR_DRIVER`
   and `MTV_LED_OUTPUT`. Settings of the old web service (`MTV_HOST`, `MTV_PORT`,
   `MTV_DATA=/var/lib/mechanical-tv`) are commented out.
6. **Boot configuration.** Adds only lines that are missing, with backups
   `*.mechanical-tv.bak` next to the files:

   | File | Line | Why |
   | --- | --- | --- |
   | `config.txt` | `dtparam=audio=off` | Keeps the analogue audio driver off the PWM hardware (a precaution: on the Pi 4 audio uses the other PWM channel) |
   | `config.txt` (hat) | `dtparam=i2c_arm=on`, `dtparam=i2c_arm_baudrate=400000` | The HAT needs one I2C write per step |
   | `config.txt` | `dtoverlay=pwm,pin=18,func=2` | Hardware PWM on GPIO 18 clocks STEP |
   | `config.txt` | `gpio=24=op,dh` | A4988 ENABLE high (motor off) from power-on |
   | `config.txt` (spi) | `dtparam=spi=on` | `/dev/spidev0.0` for the LED |
   | `config.txt` (spi) | `core_freq_fixed=1` | The SPI bit clock divides the core clock, which must not scale |
   | `cmdline.txt` | `isolcpus=3 irqaffinity=0-2` | CPU 3 runs only the realtime LED thread (`MTV_ISOLATE_CPU=0` leaves it out, and removes it if an earlier run added it) |
   | `cmdline.txt` (spi) | `spidev.bufsiz=65536` | One transfer carries a whole revolution |

7. **Realtime permissions.**
   - `/etc/security/limits.d/90-mechanical-tv.conf` gives the gpio group `rtprio 80` and unlimited
     `memlock`, so the app can run its realtime thread without root.
   - For the GPIO LED output, `kernel.sched_rt_runtime_us = -1` in `/etc/sysctl.d/` stops the kernel
     pausing the spinning LED thread for 50 ms each second.
8. **Boot-time unit.** `mechanical-tv-hw.service` (oneshot, root) exports the PWM channel to the
   gpio group and sets the CPU governor to `performance`. At shutdown it stops the STEP clock and
   drives ENABLE high and the LED pins low, in case the console did not get to.
9. **Retires the web service.** `mechanical-tv.service` from versions before 0.3 is disabled and
   removed. Its data in `/var/lib/mechanical-tv` is left alone. To keep old clips, quit the console
   and run:

   ```bash
   sudo cp -r /var/lib/mechanical-tv/media ~/.local/share/mechanical-tv/
   sudo chown -R "$USER": ~/.local/share/mechanical-tv/media
   ```
10. **Desktop.** Installs the menu entry, icon, desktop shortcut and autostart entry (`MTV_AUTOSTART=0`
    skips autostart). Finally it runs `mechanical-tv --check`, and restores the previous release if
    that fails.

Re-running the installer is safe. Boot and kernel-command-line lines are never added twice. The
motor driver and LED output chosen last time are kept unless given again. Each run installs a new
release and keeps the three newest.

Update with `git pull --ff-only` and `sudo bash install.sh`. To roll back, list
`/opt/mechanical-tv/releases/` and run
`sudo ln -sfn /opt/mechanical-tv/releases/<timestamp> /opt/mechanical-tv/current`.

The HAT alternative: `sudo MTV_MOTOR_DRIVER=hat bash install.sh` (enables I2C, no PWM lines).

## HDMI input

### HDMI-to-CSI-2 adapter (GeeekPi/52Pi, Geekworm, Waveshare, Auvidea B101: Toshiba TC358743)

These boards plug into the **CAMERA** ribbon connector. The Pi 4 has two identical 15-pin sockets:
CAMERA is between the micro-HDMI ports and the audio jack, DISPLAY is next to the microSD end. Use
the 15-pin cable with the contacts facing the right way at both ends. The adapter's red LED only
shows it has power.

1. **Kernel overlay.** In `/boot/firmware/config.txt` change `camera_auto_detect=1` to `0`, add
   `dtoverlay=tc358743`, and reboot. The installer prints a reminder if it is missing; it cannot add
   it because the adapter cannot be detected before the overlay loads.
2. **Check the bridge:**

   ```bash
   sudo dmesg | grep -i tc358743    # "tc358743 ... found"
   v4l2-ctl --list-devices          # unicam owning /dev/video0
   ```

3. **Select Live HDMI Video** in the console. With the default `MTV_DEFAULT_SOURCE=auto` it is
   selected at start whenever `/dev/video0` exists. The app then:
   - loads the packaged EDID once per run (`edid/1080p30.txt`: 1080p30 preferred, 720p60
     alternate). Until an EDID is loaded the source sees no display;
   - before every open, waits for a source and locks its timings, retrying every second;
   - reads UYVY straight from the driver's buffers and converts every frame (about 2 ms each).

   Within a few seconds the Mac's Displays settings should list **MechanicalTV** at 1920 × 1080,
   30 Hz. If not, hold Option in Displays and click **Detect Displays**.

4. **Read the status line** next to the input selector:
   - `HDMI /dev/video0 · CSI · v4l2 · 1920x1080 @ 30 FPS · LIVE`: frames are flowing.
   - `… · BLACK PICTURE (peak n/255)`: frames arrive but are black. Wake the source or move a window
     onto that display.
   - `… · WAITING FOR FRAMES`: the device is open but no frame has arrived in 5 s.
   - `… · NO SIGNAL`: no source, or the device could not be opened. The reason is underneath.

   Every transition is in the debug log (F12) with the prefix `HDMI capture:`.

#### If the Mac shows no display

1. Live HDMI must be selected: the status line must not read `PROCEDURAL PATTERN`.
2. `sudo v4l2-ctl --log-status` prints the chip state:
   - `Cable detected (+5V power)` missing means the cable or USB-C adapter is not delivering +5 V,
     so try another.
   - `Hotplug enabled` with `No video detected` means the Mac sees the display but is not sending:
     re-detect displays.
   - `TMDS signal detected` and `Stable sync signal` mean video is arriving.
3. The log must contain `HDMI capture: EDID … loaded`. `EDID load failed` usually means
   `/dev/video0` is a Pi camera, or the ribbon is in the DISPLAY socket.

**Manual check** with the console closed (it holds the device while Live HDMI is selected):

```bash
v4l2-ctl --set-edid=file=/opt/mechanical-tv/current/share/mechanical-tv/edid/1080p30.txt
v4l2-ctl --query-dv-timings
v4l2-ctl --set-dv-bt-timings query
v4l2-ctl -v pixelformat=UYVY
v4l2-ctl --stream-mmap=4 --stream-count=600 --stream-to=/dev/null   # prints the achieved fps
```

The shipped EDIDs already have correct checksums. Regenerate them with
`python3 packaging/make-edid.py`.

### USB HDMI capture dongle (UVC)

Plug it into a USB 3.0 port. No overlay or EDID is needed; the app runs FFmpeg in MJPEG. Prefer a
stable `/dev/v4l/by-id/...` path for `MTV_HDMI_DEVICE`.

## Settings

`/etc/mechanical-tv/environment` holds one `KEY=value` per line. The desktop entry passes it with
`--env-file`. Restart the console after editing. Real environment variables win over the file, and
command-line options win over both.

| Variable | Default | Meaning |
| --- | --- | --- |
| `MTV_MOTOR_DRIVER` | `a4988` | `a4988` (also `drv8825`, `tmc2209`, `stepdir`) or `hat`. Anything else runs in simulation with a note |
| `MTV_MOTOR_PORT` | `1` | HAT stepper port (1 = M1/M2, 2 = M3/M4) |
| `MTV_MICROSTEPS` | `16` | Must match the driver's MS jumpers; check with **Check microstepping** |
| `MTV_DIR_GPIO`, `MTV_ENABLE_GPIO` | `23`, `24` | A4988 DIR and ENABLE |
| `MTV_MOTOR_DIRECTION` | `1` | `-1` reverses the motor |
| `MTV_PWM_CHIP` | first `/sys/class/pwm/pwmchip*` | PWM chip for STEP, as a full sysfs path such as `/sys/class/pwm/pwmchip0` |
| `MTV_RAMP_RATE` | `2.0` | Maximum acceleration in FPS per second (0.1-50) |
| `MTV_START_FPS` | `0.02` | Speed of the first steps after enable (rev/s) |
| `MTV_JERK_TIME` | `0.5` | Seconds over which acceleration builds up (S-curve) |
| `MTV_ENABLE_SETTLE` | `0.2` | Seconds the coils hold before the first step |
| `MTV_STALL_DETECT` | `1` | `0` disables stall and slip detection |
| `MTV_STEP_MODE` | `half` | HAT only: `full` for 200 steps per revolution |
| `MTV_OPTO_PIN`, `MTV_OPTO_PULL` | `4`, `down` | Index sensor GPIO and bias (`down`, `up`, `none`) |
| `MTV_MICROSTEP_TEST_RPS` | `0.25` | Speed of Check microstepping, in revolutions per second at the configured setting |
| `MTV_LED_OUTPUT` | `gpio` | `gpio` or `spi` |
| `MTV_LED_PIN` | `21` | GPIO for the `gpio` output |
| `MTV_SPI_LED` | `0` | `1` is the same as `MTV_LED_OUTPUT=spi` |
| `MTV_SPIDEV`, `MTV_SPI_HZ` | `/dev/spidev0.0`, `2000000` | SPI device and bit rate |
| `MTV_SPI_LATENCY_US` | `30` | Measured ioctl-to-first-bit delay; constant offsets are absorbed by the phase calibration |
| `MTV_LED_MIN_PULSE_US` | `5` | Shorter pulses are not emitted |
| `MTV_LED_GUARD_US` | `150` | Dark time before the predicted next edge (at least 3 % of the period while not locked) |
| `MTV_RT_CPU`, `MTV_RT_PRIORITY` | `3`, `60` | Realtime thread CPU (`-1`: no pinning) and SCHED_FIFO priority |
| `MTV_MLOCK` | `1` | Lock the app's memory (no page faults on the realtime path) |
| `MTV_GPIOCHIP` | auto (`pinctrl-bcm2711`) | GPIO chip number or path |
| `MTV_DEFAULT_SOURCE` | `auto` | `auto` (HDMI when the device exists), `pattern` or `hdmi` |
| `MTV_HDMI_DEVICE` | `/dev/video0` | V4L2 capture node |
| `MTV_HDMI_BRIDGE` | `auto` | `csi` or `usb` if detection from `/sys/class/video4linux` is wrong |
| `MTV_HDMI_FORMAT` | `auto` | FFmpeg input format: `uyvy422` (CSI), `mjpeg` (USB), `none` |
| `MTV_HDMI_EDID` | `auto` | Packaged 1080p30, `none`, or a path such as `…/share/mechanical-tv/edid/720p60.txt` |
| `MTV_HDMI_FIT` | `fit` | `fit` letterboxes; `crop` fills 32 × 25 from the centre |
| `MTV_HDMI_BACKEND` | `auto` | `auto` (in-process V4L2 for UYVY, else FFmpeg), `v4l2` (no fallback), `ffmpeg`. `opencv` from older settings is treated as `auto` |
| `MTV_HDMI_CONVERT_FPS` | `30` | Maximum HDMI frames converted per second |
| `MTV_DATA` | `~/.local/share/mechanical-tv` | Library and logs |
| `MTV_SHARE_DIR` | the install's `share/mechanical-tv` | Where `edid/` is found (development only) |
| `MTV_FULLSCREEN`, `MTV_DEBUG` | `0` | Start full screen / with the debug window |
| `MTV_MOCK_HARDWARE` | `0` | Simulated motor, sensor and LED |

## Status and logs

- **Debug window:** F12, or click the LOG pill.
- **Log file:** `~/.local/share/mechanical-tv/logs/mechanical-tv.log`, rotating at 1 MB with 5 kept.
- **Environment report:** `/opt/mechanical-tv/current/bin/mechanical-tv --check --env-file
  /etc/mechanical-tv/environment` prints the OS facts the app depends on: the PWM and GPIO devices,
  the spidev buffer size, the realtime limits and isolated CPUs, and the HDMI adapter.

## Development computer

macOS or Linux with CMake, a C++20 compiler, Qt 6 (or 5.15) Widgets and SQLite:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug     # macOS: -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase);$(brew --prefix sqlite)"
cmake --build build -j
QT_QPA_PLATFORM=offscreen ./build/mtv_tests
./build/mechanical-tv --mock-hardware
```

Off the Pi, the motor, sensor and LED are simulated, and Live HDMI reports the missing device.
