# Project plan

## Intended experience

Power on → the console opens on the Pi's display → the MacBook's HDMI picture appears → spin up →
the disk shows the picture, phase-locked to its index notch → stop → shut down.

## Status (version 0.3.0, C++)

| Feature | Status | Next |
| --- | --- | --- |
| Desktop console (Qt), debug log, free-run, live previews | Implemented, tested off-screen | Persist calibration between runs |
| Live HDMI (TC358743, in-process V4L2, EDID and timings by ioctl) | Implemented, tested with a fake bridge | Validate on the Pi with the MacBook |
| Test patterns, spiral mapping, gamma, contrast, phase, flips | Implemented | Auto-centring from the notch angle |
| Motor: A4988 on hardware PWM with an S-curve start; Motor HAT | Implemented | Validate the start with the 1.75 mm disk |
| Stall and slip detection; microstep self-test | Implemented (simulation) | Tune thresholds on the real disk |
| Index sensor with kernel timestamps, lock with hysteresis | Implemented (simulation) | Measure jitter on the Pi |
| LED: SPI DMA bitstream or GPIO bit-bang on an isolated realtime core | Implemented (simulation) | Scope both on the Pi; measure `MTV_SPI_LATENCY_US` |
| Media library with background conversion | Implemented, tested with FFmpeg | |
| Installer with realtime OS settings | Implemented, tested in a Trixie container | Run on the Pi |
| Web interface | Removed in 0.3 by decision | |

## Milestones

1. **Bench (now):** wiring checks from [DIAGNOSIS.md](DIAGNOSIS.md), Check microstepping, spin-up with
   the 1.75 mm disk, kernel edge jitter.
2. **Static picture:** aperture grid stands still; phase and flips calibrated.
3. **Live HDMI on the disk:** MacBook picture at 10 FPS, locked.
4. **Long run:** 30 minutes without a stall, temperatures noted.
5. **Optional:** RGB LED sequencing; a microcontroller (RP2040) for pixel timing if the Pi's
   output is ever not stable enough; persisted calibration.
