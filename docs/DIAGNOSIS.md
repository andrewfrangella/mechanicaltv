# Diagnosis: start-up stall, sync that never holds, and the HDMI adapter

Written 2026-11-03 for version 0.3.0 (the C++ port). It records what was wrong, what the new
software changes, and what must still be measured on the Pi, because two of the three problems
have a hardware side that no software can fix.

The research behind it is in [docs/research/](research/README.md). The motor and HDMI conclusions
were checked by three independent reviewers each (datasheet, platform and code lenses). Where they
corrected a number, the corrected figure is used here.

## 1. The motor stalls when it starts

### What the numbers say

| Quantity | 0.6 mm disk (bitluni) | 1.75 mm disk (now) | 2.4 mm disk |
| --- | --- | --- | --- |
| Mass (solid PLA, from the STL) | 30 g | 81 g | 110 g |
| Moment of inertia | 2,663 g·cm² | 7,611 g·cm² | 10,400 g·cm² |
| Ratio to the rotor (22 g·cm²) | 121 : 1 | **346 : 1** | 473 : 1 |
| Torque to accelerate at 2 rev/s² | 0.33 N·cm | 0.96 N·cm | 1.3 N·cm |

The motor (STEPPERONLINE 17HE08-1004S: 1.0 A, 3.6 Ω, 4 mH, 17 N·cm holding) has ample torque
for running speed and for the ramp: about 7 to 11 N·cm is still available at 600 RPM on 12 V.
**The heavier disk is not too heavy to turn.** What it changed is the start. The Python app
enabled the driver and jumped from standstill straight to 0.5 rev/s in the same instant. A
simulation of that jump shows the 0.6 mm disk survives it with about 0.35 A of drive, while the
1.75 mm disk needs about 0.65 A (about 11 N·cm) and the 2.4 mm disk about 1 A. So the new disk made
the start sensitive to anything that reduces drive current. Thinning the disk again would only hide
that.

### What the software now does

- The driver is enabled and the rotor allowed to settle for 200 ms before the first step.
- The step train starts at 0.02 rev/s and follows a jerk-limited S-curve to the target. No step in
  speed is larger than one 5 ms tick of acceleration. Settings: `MTV_START_FPS`, `MTV_RAMP_RATE`
  (default 2 FPS/s), `MTV_JERK_TIME` and `MTV_ENABLE_SETTLE`.
- **Stall detection.** The motor is stopped, the LED blanked and the reason shown if no index pulse
  arrives within 1 s, or 3 revolution periods, once the motor is commanded above 0.3 rev/s. It also
  triggers if the measured steps per revolution differ from the expected count by more than 8 % for
  3 revolutions.
- **Check microstepping** (a console button) spins the disk slowly and counts motor steps between
  two index pulses. It then reports the microstep mode the driver is really in.

### What to check on the bench, in order

1. **Microstep pins.** The software assumes 1/16 microstepping (3,200 steps per revolution). The
   A4988 has internal pull-downs on MS1, MS2 and MS3, so if they are not tied to 3.3 V the driver
   full-steps. Every speed is then 16 times faster than commanded: the old "soft start" became
   480 RPM from a standstill, which is an instant stall with a buzz. Measure MS1, MS2 and MS3 to
   ground: all three must read about 3.3 V. Then press **Check microstepping**. The console's
   own stall message names the mode if they are wrong while spinning.
2. **Vref, not pot position.** Read the two sense resistors next to the chip (R100 = 0.1 Ω,
   R068 = 0.068 Ω, R050 = 0.05 Ω). Measure the pot wiper to ground with the power on. Set it for
   0.7 to 0.8 A, using I = Vref / (8 × Rs):

   | Sense resistor | Vref for 0.7-0.8 A |
   | --- | --- |
   | 0.1 Ω | 0.56-0.64 V |
   | 0.068 Ω | 0.38-0.44 V |
   | 0.05 Ω | 0.28-0.32 V |

   "All the way to the right" is the top of the trimmer on most boards, about VDD/3, so roughly
   1.1 V with 3.3 V logic. That is about 1.4 A on a 0.1 Ω clone and 2 A on a 0.068 Ω board, above
   the motor's 1 A rating. A bare A4988 then reaches thermal shutdown (165 °C) within seconds,
   disables its outputs, and the disk stops with a buzz. On some boards the pot is reversed and the
   same position is the minimum. Measure it.
3. **Heat.** Fit a heatsink on the A4988 and give it some airflow. Time the seconds from Spin Up to
   the stall with and without the heatsink. A longer or vanished stall time means thermal shutdown.
4. **Supply voltage.** Set the motor rail to at least 12 V and measure VMOT at the driver during
   spin-up: it must stay above 8.5 V (the A4988's minimum operating voltage is 8 V). 24 V is better at 600 RPM (the
   A4988 is rated to 35 V; the 12 V limit applies only to the old Adafruit HAT). Before raising it,
   separate the LED rail or recalculate the LED resistor (see HARDWARE.md, LED). Put a ≥ 47 µF
   capacitor across VMOT.
5. **Driver logic wiring.** VDD must be 3.3 V from the Pi, and RESET and SLEEP must be tied high.
   ENABLE (GPIO 24) reads 3.3 V idle and 0 V after Spin Up.
6. **Rim clearance.** With the motor released, turn the disk slowly through a full revolution and
   watch the sensor slot. The thickened plate (1.75 mm) can touch the holder wall: bitluni's holder
   leaves 0.4 mm on the hub side and the plate grew 0.575 mm per face. If it rubs, seat the hub
   further out, re-render the disk with the extra thickness on the far face only, or use a
   wider-slot sensor.

## 2. Sync "locks" for about a second, then drops

### Cause

This was a software problem, and C++ alone would not have fixed it. It came from how the Python
app timed things:

- **Edge timestamps were taken late and unevenly.** The index edge was timestamped with
  `time.perf_counter()` inside a Python callback. That callback sat behind lgpio's notification
  thread and the GIL, while the LED worker held the GIL for 100 ms at a time. Callback latency
  therefore jumped between about 0.5 ms and 5-10 ms. The lock test compared consecutive periods
  against a 2 ms jitter limit, so it dropped every time the latency mode changed. On screen that was
  "locks for about a second".
- **The picture was never anchored to the edge.** Pixel 0 started whenever the worker got to it. The
  pixel clock came from the previous, noisy period, and a new frame only started after the previous
  800 pixels finished. Phase drifted and frames were skipped.
- **Bit-banging in Python was too slow.** Two GPIO calls took 10-15 µs each, inside every 125 µs
  pixel slot. Each GIL hand-off froze the LED mid-pixel.
- **Failures were silent.** The sensor backend chain swallowed errors and could fall back to
  synthetic pulses (`mock-fallback`) that had nothing to do with the disk.
- **The speed trim did nothing useful.** The P-only speed trim was below the PWM clock's resolution.
  A stepper on a hardware clock needs no speed loop.

### What the software now does

- **Kernel edge timestamps.** The index sensor is read through the GPIO character device. Each edge
  carries a CLOCK_MONOTONIC timestamp taken in the hard-IRQ handler, microseconds after the edge,
  and no kernel debounce is used (it would move timestamping to a workqueue). Bounces are rejected
  in software if they arrive within 20 ms or within half a revolution of the previous edge.
- **Every revolution re-anchored.** Each revolution is emitted on an absolute timeline that starts
  at that edge's timestamp. Late wake-ups only darken the first pixel or two; they never shift the
  picture, and the phase cannot drift.
- **Lock hysteresis.** Locking needs three good revolutions in a row and unlocking needs two bad
  ones. It still requires measured speed within 3 % of the target and jitter under 2 ms. With kernel
  timestamps, jitter is now microseconds instead of milliseconds.
- **One realtime thread** does all LED and index work. It runs SCHED_FIFO, pinned to CPU 3 (which
  the installer isolates), with memory locked. Capture and the UI run on the other cores and hand
  frames over through shared pointers.
- **Two LED outputs:**
  - **`spi` (recommended):** SPI0 MOSI on GPIO 10 (pin 19) sends the revolution as a DMA bitstream
    at 2 MHz, giving 0.5 µs pulse resolution. It is immune to scheduling inside the revolution.
    Rewire the L298N input from pin 40 to pin 19 and install with `MTV_LED_OUTPUT=spi`.
  - **`gpio` (default, original wiring):** GPIO 21 bit-banged through `/dev/gpiomem` registers.
    Pixels whose slot has already passed are skipped, never crammed in.
- **No more silent fallbacks.** The HUD names the real backend (`gpio-cdev`, `gpiomem`, `spi-dma`
  or `simulated`) and any sensor error.

### What to check on the Pi

- While the motor runs, the debug log gets a line every 5 s:
  `index: 10.00 FPS, period 100.032 ms, jitter 0.012 ms (rms 0.009), 3200 steps/rev, locked, …`.
  These figures come from the kernel's edge timestamps. Jitter should stay well under 0.1 ms and the
  steps per revolution should read 3200. If the period wanders by milliseconds, the disk itself is
  slipping: go back to section 1.
- To check the sensor alone, quit the console (it holds GPIO 4 while the motor runs) and turn the
  disk by hand:
  - Trixie (libgpiod 2): `gpiomon -c gpiochip0 -e rising -b pull-down 4`
  - Bookworm (libgpiod 1.6): `gpiomon -r gpiochip0 4`

  There should be one line per revolution.
- The HUD's **INDEX SENSOR** cell should read `gpio-cdev · GPIO 4 · N pulses`, with N rising about
  10 per second.
- For the bit-banged output, `sudo cyclictest -m -a 3 -t1 -p90 -i125 -D 5m` shows the realtime
  core's latency. Run it with the console open but the motor stopped: it preempts the LED thread
  and would disturb the picture. Under about 100 µs is fine. If it is worse, use the SPI output.

## 3. Does the HDMI adapter encode H.264? Could firmware bypass it?

**No to both.** The adapter on the Waveshare wiki is the same chip as the GeeekPi board: a Toshiba
TC358743 HDMI-to-CSI-2 bridge. It re-emits the HDMI pixels uncompressed over the camera ribbon, as
16-bit UYVY (BT.601 limited range from the driver) or 24-bit RGB. It has no encoder, no scaler and
no user-loadable firmware. The Linux driver configures it over I2C, and the EDID is written into the
chip's RAM by the app itself.

The H.264 on the wiki page is the **Pi's own** video encoder, applied after capture to save a file:
`yavta --encode-to`, a GStreamer `v4l2h264enc` pipeline, `raspivid`. This app never encodes or
decodes anything. It reads the raw UYVY frames straight from the driver's memory-mapped buffers.

The real costs on that path are on the Pi side:

- **Reading the DMA buffers.** They are uncached, so the CPU reads them at only about 450 MB/s. A
  full 1080p frame would take about 9 ms just to touch. The C++ reader copies only the rows it
  samples, about 250 of 1,080, in one wide read each: about 2 ms per frame, so it converts every
  frame (30 per second) and costs about 6 % of one core.
- **Lane bandwidth.** 1080p30 UYVY uses about half of the Pi 4's two CSI lanes. 1080p60 does not
  fit, which is why the shipped EDID advertises only 1080p30 and 720p60.
- **Latency.** From the Mac's screen to the LED is roughly 0.1 s. It is dominated by the disk's own
  100 ms frame time at 10 FPS, not by capture.

Changing the EDID to 720p60 saves almost nothing in CPU (111 versus 124 MB/s). Keep 1080p30 unless a
600-frame `v4l2-ctl --stream-mmap` test shows dropped frames.
