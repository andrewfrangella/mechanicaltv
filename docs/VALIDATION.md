# Validation

## Automated tests

```bash
cmake -S . -B build -G Ninja -DMTV_WERROR=ON && cmake --build build
QT_QPA_PLATFORM=offscreen ./build/mtv_tests        # doctest; --help lists filters
bash -n install.sh packaging/make-test-clip.sh && sh -n packaging/mechanical-tv-hw-setup
python3 packaging/make-edid.py --check
```

`install.sh` runs the suite on the Pi before installing. GitHub Actions builds with warnings as
errors and runs it on Ubuntu.

| File | Covers |
| --- | --- |
| `tests/test_pipeline.cpp` | Gamma LUT (round-half-even like Python), all ten patterns, spiral map and its permutation, phase and flips, single brightness application, reconstruction, calibration clamps, source switching, HDMI without signal is black, library clips loop |
| `tests/test_capture.cpp` | FFmpeg argv and filter, frame split, adapter detection from sysfs, format/EDID per adapter, shipped EDID checksums and 1080p30 timing. With a fake bridge and reader: EDID once, timings per open, frames flow, buffers requeued, stop closes, no signal then recovery, EDID failure message, missing device, stop during a slow open, short frame skipped, explicit `v4l2` vs `auto` fallback, USB never native, dead FFmpeg reported with its stderr, stalled stream reopened. Also placement, sampler luma levels and crop, odd widths, and a frozen source turning black within 1 s |
| `tests/test_v4l2_reader.cpp` (Linux) | V4L2 reader against a fake driver: setup and teardown order, bad and short buffers requeued, sequence gaps counted, source-change event, non-UYVY refused |
| `tests/test_hardware.cpp` | A realistic spin-up with a blocking LED output at three notch phases (the audit's false-stall case), step accounting at past instants, reversed-direction step magnitudes, microstep check ignoring bounces and being cancellable, motor driver errors reported. Also: ramp setting, S-curve (no speed steps, no overshoot, jerk limit), mock motor, revolution tracker (debounce, lock hysteresis, silence, steps ratio), LED bit builder (pulse widths, minimum pulse, chunked equals whole, late start). In simulation: lock, LED stream equals the pipeline's, stop, LED dark below 1 FPS, free-run pacing and no double clocking, emergency blank, speed limits, stall stop, microstep mismatch while spinning, the microstep self-test, live preview when idle |
| `tests/test_engine.cpp` | Subprocess cancel, crash failsafe writes the PWM off, library CRUD, id validation, interrupted-job recovery, real FFmpeg conversion (20 frames from a 2 s clip) and a corrupt file, Studio import and worker, health fields, environment file, log file, listener and rotation |
| `tests/test_console.cpp` | Options and env file, choices, default source, data-directory fallback, `--check` fields; the real window off-screen: pills, HUD, slider labels, free-run, start to LOCKED, emergency blank, calibration, source change, error counter and debug window, emergency blank acting at once during a microstep check; SIGTERM to the real binary shuts it down cleanly |

Recorded 2026-11-03:

- **macOS (Apple clang 21, Qt 6.11):** 57 test cases pass. The suite is also clean under
  ThreadSanitizer, AddressSanitizer and UBSan.
- **Debian Trixie arm64 (GCC 14.2, Qt 6.8, `-Werror`), in a container:** 60 test cases pass, the
  extra three being the Linux-only V4L2 reader tests.
- **Bookworm (GCC 12) and Ubuntu 24.04 (GCC 13):** the independent audit built and tested on both. The installer was run end to end in the same
container with stubbed systemd and boot files, and a second run was idempotent.

**No test drives a motor, sensor or LED.** These checks prove the logic, not the timing on the Pi.

## On the Pi: bring-up checklist (not yet performed)

### Install

- [ ] Fresh Raspberry Pi OS 64-bit with desktop. `sudo bash install.sh` builds, tests and installs;
  after the reboot the console opens full screen.
- [ ] `mechanical-tv --check` shows `hardware_pwm: true`, `gpiochip: true`, the rtprio limit 80,
  `isolated_cpus: "3"`, and for SPI `spidev_bufsiz: 65536`.
- [ ] The debug log at start has no `SCHED_FIFO … refused` warning.

### Motor ([DIAGNOSIS.md](DIAGNOSIS.md) section 1)

- [ ] MS1-MS3 at 3.3 V, Vref measured and set for 0.7-0.8 A, heatsink fitted, VMOT ≥ 12 V under load.
- [ ] Check microstepping passes (3,200 steps per revolution).
- [ ] Spin up to 10 FPS with the disk fitted: no stall, motor pill reaches LOCKED within about 8 s.
- [ ] 30-minute run at 10 FPS: no STALLED, driver temperature acceptable.

### Sync and picture

- [ ] Console closed, disk turned by hand: `gpiomon -c gpiochip0 -e rising -b pull-down 4` (Trixie;
  Bookworm: `gpiomon -r gpiochip0 4`) prints one line per revolution.
- [ ] With the motor at 10 FPS, the debug log's `index:` lines show jitter well under 0.1 ms and
  3200 steps/rev.
- [ ] OPTO JITTER in the HUD stays well under 1 ms while LOCKED; lock holds for minutes.
- [ ] Aperture grid stands still on the disk; phase offset centres it; Sweeping Column runs in order.
- [ ] GPIO output: `cyclictest -m -a 3 -t1 -p90 -i125 -D 5m` with the console open and the motor
  stopped, max under 100 µs.
- [ ] `kill <pid>` of a spinning console stops the motor and blanks the LED (log: `signal 15`).
- [ ] SPI output: scope GPIO 10 shows 0.5 µs bits, the frame starts within a pixel of the notch
  every revolution, and MOSI is low between revolutions.

### HDMI

- [ ] `dmesg | grep tc358743` finds the bridge; selecting Live HDMI logs `EDID … loaded`; the Mac
  lists MechanicalTV at 1920 × 1080 30 Hz.
- [ ] Status `CSI · v4l2 · 1920x1080 @ 30 FPS · LIVE`; SMPTE bars from the Mac match the disk.
- [ ] Unplug HDMI: NO SIGNAL and a black disk within 5 s; replug: recovers without restarting.
- [ ] `v4l2-ctl --stream-mmap=4 --stream-count=600 --stream-to=/dev/null` reports about 30 fps
  with no errors.

### Library and shutdown

- [ ] Add a clip made by `packaging/make-test-clip.sh`; it converts to READY while the motor is
  stopped; Show on disk plays it.
- [ ] Ctrl+Q stops the motor and blanks the LED before the window closes.
