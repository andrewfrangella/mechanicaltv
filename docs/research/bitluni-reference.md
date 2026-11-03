# bitluni MechanicalTV reference design: timing, geometry, video path, and what a Linux/C++ port can copy

Date: 2026-11-03. Prepared for the C++ port of `mechanical_tv` and for the stall / sync diagnosis.

Legend used throughout: **FACT** = verified from a primary source (cited); **ESTIMATE** = my arithmetic, shown; **HYPOTHESIS** = unverified inference, with a bench check where possible.

## 0. Sources

Primary (all fetched 2026-11-03; local copies under `docs/research/bitluni-dl/`):

| Source | URL / path | Used for |
| --- | --- | --- |
| bitluni/MechanicalTV repo (default branch `main`, created and last pushed 2024-11-08, no licence file) | https://github.com/bitluni/MechanicalTV | tree listing via GitHub API |
| Firmware `MechanicalTvESP32/MechanicalTvESP32.ino` (211 lines) | https://raw.githubusercontent.com/bitluni/MechanicalTV/main/MechanicalTvESP32/MechanicalTvESP32.ino | all timing logic. Line numbers below refer to this file as fetched; the copy in the user's note (Obsidian: Mechanical TV / Original Arduino Code to adapt) is identical in content (it is double-spaced, so its line numbers differ). |
| `MechanicalTvESP32/websocket.h` (69 lines) | https://raw.githubusercontent.com/bitluni/MechanicalTV/main/MechanicalTvESP32/websocket.h | frame ingest, fps command |
| `MechanicalTvESP32/WebSocketScreenShare.html` (164 lines) | https://raw.githubusercontent.com/bitluni/MechanicalTV/main/MechanicalTvESP32/WebSocketScreenShare.html | video source and frame format |
| `Models/mechanical tv disk.stl`, `Models/mechanical tv base.stl`, `Models/ledfixture.stl` (Blender 4.0.1 exports) | https://github.com/bitluni/MechanicalTV/tree/main/Models | disk geometry, sensor and LED stations (analysed with trimesh; scripts in `.../scratchpad/research/scripts/`) |
| `readme.MD` | https://raw.githubusercontent.com/bitluni/MechanicalTV/main/readme.MD | only content: a link to an Arduino-forum A4988 wiring post |
| Arduino forum post #28 of "Stepper motor speed controller using Arduino" (the readme's wiring reference) | https://forum.arduino.cc/t/stepper-motor-speed-controller-using-arduino/542054/28 | shows a bare A4988 with STEP/DIR, RESET tied to SLEEP, no MS pins connected |
| YouTube "I made a hilarious mechanical TV" (bitluni, published 2024-11-09, 10:17) description | https://www.youtube.com/watch?v=R-wbfP1pmVw | parts list |
| Hackster write-up (Evan Rust) | https://www.hackster.io/news/this-mechanical-television-uses-an-esp32-a-motor-and-an-ultraviolet-led-to-display-videos-ce06e410807e | UV LED, phosphorescent screen, A4988 |
| Hackaday write-up (Al Williams, 2024-11-10) | https://hackaday.com/tag/nipkow-disk | confirms stepper; links the video |
| Fabbaloo (2024-12-02) | https://www.fabbaloo.com/news/design-of-the-week-3d-printed-mechanical-tv | light-sensitive filament screen, "< US$50" |
| Allegro A4988 datasheet (Pololu-hosted PDF) | https://www.pololu.com/file/0J450/a4988_DMOS_microstepping_driver_with_translator.pdf | STEP edge, min pulse, MS truth table, pull-downs, ITripMAX |
| Pololu A4988 carrier page | https://www.pololu.com/product/1182 | MS pins unconnected = full step; 0.068 ohm sense resistors since 2017; 8-35 V |
| ST L298 datasheet (SparkFun mirror) | https://www.sparkfun.com/datasheets/Robotics/L298_H_Bridge.pdf | switching delays for the LED path |
| arduino-esp32 `cores/esp32/Esp.h` | https://raw.githubusercontent.com/espressif/arduino-esp32/master/cores/esp32/Esp.h | `ESP.getCycleCount()` = `esp_cpu_get_cycle_count()` (line 119-121) |
| Linux 6.12 `include/uapi/linux/gpio.h` | https://raw.githubusercontent.com/torvalds/linux/v6.12/include/uapi/linux/gpio.h | line-event timestamp semantics (lines 275-304) |
| STEPPERONLINE 17HE08-1004S listing (page blocks fetches; data from the search snippet) | https://www.omc-stepperonline.com/e-series-nema-17-bipolar-1-8deg-17ncm-24-07oz-in-1a-42x42x23mm-4-wires-17he08-1004s | holding torque 17 N cm, 1.0 A, 3.6 ohm, 4.0 mH, 23 mm body |
| User's parametric disk | `3d_models/ (kept with the 3D model files) nipkow_disk.scad`, `.../3d_models/README.md` | the user's own STL-derived geometry (agrees with my STL analysis) |
| User's Python app | `(repo, Python version 4c64391) mechanical_tv/capture.py:1155-1170`, `hardware/sensor.py:73-94`, `hardware/motor.py:489,714,722` | cross-reference |

## 1. Key findings in one screen

1. **FACT.** bitluni's firmware never measures the revolution period and has no PLL. The pixel clock and the STEP clock are both derived from one free-running CPU cycle counter and the *commanded* frame rate (`updateSteps()`, .ino:53-60). The index sensor is used for exactly three things: resetting the pixel counter (phase), pacing the speed ramp, and swapping frame buffers (.ino:136-156). The stepper is treated as a synchronous clock: 800 pixel slots are, by construction, exactly 200 STEP pulses.
2. **FACT.** The STEP output is toggled every `ticksPerFrame/400` cycles, i.e. **200 rising edges per revolution** (.ino:20, 59, 158-163). An A4988 advances one (micro)step per rising edge (datasheet: "A low-to-high transition on the STEP input"), so his disk only makes one revolution per frame if the A4988 is in **full-step** mode, which is what an A4988 with MS1-3 unconnected does (internal 100 k / 50 k / 100 k pull-downs). At 10 fps that is a 2.000 kHz STEP signal, not the 32 kHz the Python app generates at MTV_MICROSTEPS=16.
3. **FACT.** The LED is a software PWM of one pulse per 125 us pixel slot, **right-aligned** in the slot (turns on at `(1 - v/255)` of the slot, off at slot end), computed by a polled loop with cycle-counter granularity (.ino:164-174). Grey levels are 16 (4-bit nibbles from the browser, expanded x0x11, websocket.h:41-42).
4. **FACT.** The frame starts on the **trailing (falling) edge** of the sensor pulse: while OPTO reads HIGH the pixel counter is held at zero every loop iteration, and the ramp/buffer swap happen once when it returns LOW (.ino:136-156). The pulse itself therefore blanks ~1-2 pixel slots per revolution (estimate below). An early pulse restarts the frame immediately; a late pulse leaves the LED frozen in its last state after pixel 799 (no `digitalWrite` once `pixelIndex >= 800`).
5. **FACT (STL).** Hole *i* (i = 0..31) is at radius 84.5 + i mm and angle 180 - 11.25 i degrees; the 1.0 mm sync notch (r 120 mm to the rim) is at -168.75 degrees, **the same angle as hole 31 (the outermost hole)**; hole 0 (innermost) is 11.25 degrees = 25 pixel slots away from the notch. The base STL puts the opto-interrupter holder centred at -174.6 degrees and the LED window centred at 180 degrees, 5.4 degrees apart; with the window half-width of 5.625 degrees this means hole 31 enters the window 0.5 px before the notch reaches the sensor axis, which is why the firmware needs no phase parameter.
6. **FACT.** Pixel index -> image: `x = 31 - idx/25`, `y = 24 - idx%25` (.ino:168-169). So the first hole after the sync edge scans column 31, top-to-bottom in *time* means y = 24 first and y = 0 last. The Python app's `capture.py:1161-1166` reproduces this exactly, plus `phase_offset` and `invert_x/y`.
7. **FACT.** Video: browser `getDisplayMedia` screen share -> 32x25 canvas -> luma + gamma -> 400-byte 4-bit frame over a WebSocket, sent at most every 25 ms and only when the socket buffer is empty; a 1-byte message sets fps = byte/10 (slider 1.0-25.0, default 10.0). The firmware boots at `setTargetFps(10)` and ramps 0.2 fps per **revolution** from 1.0 fps.
8. **FACT.** Hardware per the video's parts list: NEMA 17 stepper, A4988, "Photoelectric Encoder" (slot module), "UV Power LEDs" switched through an "N298L" (L298N) board, TinyPICO / TinyS3 (ESP32 / ESP32-S3), variable bench PSU. Screen = phosphorescent printed card. Neither the LED current nor the motor supply voltage is stated anywhere I could find.
9. **FACT (STL) with consequence for the user's disk.** In bitluni's base the sensor holder's through-slot for the rim is 4.0 mm wide, from z = -2.0 to +2.0, while his 0.6 mm plate occupies z = -1.6..-1.0: 0.4 mm clearance on the hub side, 3.0 mm on the far side. The user's SCAD grows the plate symmetrically about z = -1.3 (`hub_side_fraction = 0.5`), so the 1.75 mm plate occupies z = -2.175..-0.425 and **interferes with the hub-side slot wall by about 0.18 mm** if the printed base reproduces bitluni's holder and the disk sits at the designed axial position. The README's "rim stays centred in the sensor slot" assumption is not true of bitluni's base. Rubbing at the holder is a candidate contributor to the start-up stall; see bench check B1.
10. **ESTIMATE.** The heavier disk is not, by itself, a plausible stall cause at the ramp rates in use: accelerating the 1.75 mm disk (I about 7.6e-4 kg m^2) at 2 rev/s^2 needs about 0.96 N cm, under 6 % of the motor's 17 N cm holding torque. A start-up stall points at the commanded step rate / microstep mode, driver current, supply voltage or mechanical interference instead.

## 2. Hardware bitluni used

| Item | What the sources say | Status |
| --- | --- | --- |
| MCU | "I used the TinyPICO and the TinyS3 in my tests" (video description); `.ino:1` "the pins are configured for ESP32 S3"; `ticksPerSecond = 240000000` default (.ino:34) and `ESP.getCpuFreqMHz()` at run time (.ino:55) | FACT |
| Motor | "Nema 17 stepper motors" (video description); `halfStepsPerRev = 400; //common nema17` (.ino:20). Exact model unknown. | FACT (type) / unknown (model) |
| Driver | "A4988 Driver" (video description); Hackster: "NEMA17 stepper motor and A4988 driver"; readme links an A4988 wiring post whose diagram has RESET jumpered to SLEEP and no MS pins connected | FACT |
| Microstep mode | Not stated. Code emits 200 STEP rising edges per frame (section 3.3); one revolution per frame therefore requires full-step. Pololu: "leaving these three microstep selection pins disconnected results in full-step mode". | INFERENCE from code + driver default |
| Index sensor | "Photoelectric Encoder" (video description); pin `OPTO = 4`, `INPUT_PULLDOWN` (.ino:13, 199); logic in .ino:136-156 requires the pin to be HIGH only while the notch passes (see 3.4) | FACT (presence) / HYPOTHESIS (module polarity: common LM393 slot modules are described by vendors as "blocked = HIGH", which would NOT work with this code; his module or wiring must give HIGH during the notch) |
| LED | "UV Power Leds" through an "N298L driver board" (video description); Hackster: "ultraviolet LED combined with a reactive plastic card"; GPIO 21 driven as a plain digital output (.ino:15, 171-173). LED current, supply and any resistor: not stated. | FACT (type, driver) / unknown (current) |
| Screen | Phosphorescent ("light-sensitive filament", Fabbaloo; "reactive plastic card", Hackster); non-fluorescing variants had less burn-in | FACT |
| Motor supply | "Variable Power Supply" (video description), voltage not stated | unknown |
| Frame rate | Firmware default `setTargetFps(10)` (.ino:202) = 600 RPM; client slider 1.0-25.0 fps, default 10.0 (html:155-156) | FACT |

The Hackster article's sentence "In lieu of some form of photosensor ... bitluni instead went with a NEMA17 stepper motor and A4988 driver" is contradicted by the code and parts list (both have the opto sensor); treat that sentence as a journalist's paraphrase of "no closed-loop speed control".

## 3. Firmware timing logic (FACTS, with line numbers in `MechanicalTvESP32/MechanicalTvESP32.ino`)

### 3.1 Task structure

- `setup()` disables the task watchdog (`esp_task_wdt_deinit()`, :191), then pins `stepperTask` to **core 1** and `wifiTask` to **core 0**, both at priority `2 | portPRIVILEGE_BIT` (:204-205). `loop()` is empty (:208-210). Comment at :2 "core 1 is used realtime for driver timings".
- `stepperTask` is an infinite `while(true)` with **no delay, no yield, no interrupt**: it polls the sensor pin and the cycle counter every iteration (:121-175). The WiFi task polls the WebSocket every 20 ms (:102-109).
- Everything time-critical (STEP generation, sensor sampling, pixel clock, LED on/off) lives in that one loop on a dedicated core. There is no ISR anywhere in the project.

### 3.2 Clock source

- `ESP.getCycleCount()` (:113, 124) is the CPU cycle counter (`esp_cpu_get_cycle_count()`, arduino-esp32 `Esp.h:119-121`), 240 MHz on the S3. Deltas are computed with unsigned 32-bit subtraction `dt = t - t0` (:125), which is wrap-safe (2^32 cycles = 17.9 s at 240 MHz); the running `time` is a uint64 accumulator (:117, 127) that is only used in a commented-out debug print.
- Three accumulators are advanced by `dt` each iteration: `dtOpto` (time since the sensor pulse ended), `dtHalfStep` (time since the last STEP toggle) and `time` (:127-129).

### 3.3 Step generation (motor)

- `updateSteps()` (:53-60): `ticksPerFrame = ticksPerSecond / currentFps`, `ticksPerPixel = ticksPerFrame / 800`, `ticksPerHalfStep = ticksPerFrame / 400` (integer division).
- Each iteration: `if (dtHalfStep >= ticksPerHalfStep) { digitalWrite(STEP, stepState); stepState ^= 1; dtHalfStep -= ticksPerHalfStep; }` (:158-163). The subtraction (not reset) keeps the long-term rate exact. The STEP line therefore **toggles** 400 times per frame = **200 rising edges per frame**, a 50 % duty square wave.
- `DIR` is written LOW once in `setup()` (:198) and never changed. `EN` is configured as an output and never written (:200) - it stays LOW, which enables an A4988.
- The `step(dir, steps, delay)` helper (:88-98) is unused.
- The stepper runs **open loop**. Nothing in the firmware ever adjusts the step rate from a measurement.

Numbers at 10 fps, 240 MHz (ESTIMATE, exact integers): `ticksPerFrame` = 24,000,000; `ticksPerPixel` = 30,000 (125.000 us); `ticksPerHalfStep` = 60,000 (250 us per toggle, 500 us STEP period, **2.000 kHz**). 800 x 30,000 = 400 x 60,000 = 24,000,000, so the pixel frame and the stepping revolution have identical length with zero rounding error at 10 fps. At non-integer rates the integer divisions differ by at most a few hundred cycles (about 1 us per revolution), which the per-revolution reset absorbs.

### 3.4 Sensor handling and frame start

```
if (digitalRead(OPTO)) {            // :136   pin HIGH
    dtOpto = 0;                     // :138   hold the pixel counter at zero for the WHOLE pulse
    frameStart = true;              // :139
} else if (frameStart) {            // :143   first iteration after the pulse ended
    frameStart = false;
    rampFps();                      // :146   speed ramp advances once per revolution
    if (frameReady) { swap(shownFrame, loadedFrame); frameReady = false; }   // :147-153
}
```

Consequences (FACTS from the code, timing ESTIMATES in 3.9):

- The frame (pixel 0) begins at the **falling edge** of the sensor pulse, not the rising edge. During the pulse the counter is pinned at zero, so the LED is driven by the pixel-0 decision (`frame[31][24] >= 255`, i.e. off unless that pixel is full white).
- For this to work the pin must be LOW for most of the revolution and HIGH only while the notch passes. (A "HIGH when blocked" module would hold the counter at zero all the time and show nothing.)
- **Early pulse** (sensor fires before pixel 799 has finished): the counter is reset immediately; the remaining pixels of the frame are skipped. **Late pulse** (sensor fires after 800 slots elapsed): the `pixelIndex < 800` guard (:165) stops all LED writes, so the LED **holds its last state** until the pulse arrives; with the right-aligned pulse that last state is ON for any non-black last pixel. Because the step clock and pixel clock share one counter, with a synchronous stepper neither case is more than a few microseconds in practice.
- The ramp is only advanced by sensor pulses (:146). With no sensor the motor stays at 1 fps forever; a stalled disk freezes the ramp (but keeps stepping at the last rate).
- No debounce, no edge timestamps, no period measurement, no "lock" state exists anywhere.

### 3.5 Pixel clock and LED brightness

```
pixelIndex = dtOpto / ticksPerPixel;                     // :164
if (pixelIndex < 800) {                                  // :165
    subPixel = dtOpto % ticksPerPixel;                   // :167   0 .. ticksPerPixel-1
    x = 31 - pixelIndex / 25;  y = 24 - pixelIndex % 25; // :168-169
    LED = frame[shown][x][y] >= 255 - (subPixel * 255) / ticksPerPixel;   // :170-173
}
```

- Brightness is a **single pulse per pixel slot, right-aligned**: the LED turns on when the elapsed fraction of the slot exceeds `1 - v/255` and stays on until the slot ends. Pulse width = `v/255 x 125 us`; v = 0 is never on (the threshold never reaches 0 because `subPixel < ticksPerPixel`), v = 255 is on for the whole slot.
- It is **software PWM from a polled loop**: no ledc, no DAC, no hardware timer. Placement resolution = one loop iteration (ESTIMATE 0.5-1.5 us on an S3, i.e. better than 1/80 of a slot). Since the input only has 16 levels (websocket.h:41-42), the effective grey resolution is 16 levels at 10 fps.
- Only `x` (hole) and `y` (line) are computed from the slot index; there is **no per-hole phase or radius correction** - the disk's hole spacing is assumed perfectly uniform in angle.

### 3.6 Speed ramp

`rampFps()` (:68-86) moves `currentFps` toward `targetFps` by `fpsStep = 0.2` (:28) and recomputes the tick constants. It runs once per sensor pulse (:146), so acceleration is 0.2 fps **per revolution**: alpha = 0.2 x f rev/s^2 (0.2 rev/s^2 at 1 fps, 2.0 rev/s^2 at 10 fps). ESTIMATE: 1.0 -> 10.0 fps takes 45 revolutions, sum(1/(1 + 0.2k), k = 0..44) = **11.98 s**. The motor starts directly at `currentFps = 1.0` (:29) = 200 full steps/s = 60 RPM from rest. Compare the Python app: constant `MTV_RAMP_RATE` = 2.0 fps/s from 0.5 fps on a 20 ms timer (motor.py:722, 741), i.e. ten times harsher at the start and the same at the end.

### 3.7 Frame buffers

Three `volatile uint8_t frame[3][32][25]` buffers with indices `loadingFrame`, `loadedFrame`, `shownFrame` (:22-26). The WiFi core writes `loadingFrame`, then swaps loading/loaded and sets `frameReady` (websocket.h:45-48); the stepper core swaps shown/loaded at the next frame start (:147-153). The display therefore changes only at the frame boundary (tear-free) and always shows the most recent complete frame; excess frames are simply overwritten. The swaps are not atomic across cores (benign in practice; a port should use a real triple buffer with atomics). `initFrame()` fills the start-up frame with a horizontal ramp `256 * x / 32` (:178-187).

### 3.8 Language-neutral pseudo-code of the reference loop

```
constants: XRES = 32, YRES = 25, PIX = 800, TOGGLES_PER_REV = 400 (-> 200 STEP pulses)
state:     fps_cmd = 1.0, fps_target = 10.0, ticks_frame, ticks_pixel, ticks_toggle
           dt_opto = 0, dt_toggle = 0, step_level = 0, in_pulse = false

recompute():                                  # on every fps change
    ticks_frame  = CPU_HZ / fps_cmd
    ticks_pixel  = ticks_frame / PIX          # integer
    ticks_toggle = ticks_frame / TOGGLES_PER_REV

loop forever (dedicated core, never yields):
    dt = cycle_counter() - last ; last += dt
    dt_opto += dt ; dt_toggle += dt

    if SENSOR_PIN is HIGH:                    # notch in front of the sensor
        dt_opto = 0 ; in_pulse = true         # phase reset, held for the whole pulse
    else if in_pulse:                         # trailing edge = frame start
        in_pulse = false
        fps_cmd  = step_toward(fps_target, 0.2) ; recompute()
        if new_frame_ready: swap(shown, loaded)

    if dt_toggle >= ticks_toggle:             # open-loop stepper clock
        write STEP_PIN = step_level ; step_level ^= 1 ; dt_toggle -= ticks_toggle

    idx = dt_opto / ticks_pixel
    if idx < PIX:
        sub = dt_opto % ticks_pixel
        x = XRES-1 - idx / YRES ; y = YRES-1 - idx % YRES
        write LED_PIN = (shown[x][y] >= 255 - sub*255/ticks_pixel)   # right-aligned pulse
    # else: LED keeps its last state until the next pulse
```

### 3.9 Derived timing numbers (ESTIMATES)

- Rim speed at the notch radius (r = 122.5 mm) at 10 rev/s: 2 pi x 0.1225 x 10 = 7.70 m/s, so the 1.0 mm notch takes 130 us to pass a point. Slot sensors have apertures of roughly 0.5-1 mm (unknown for his module), so the pulse is roughly 130-260 us = **1-2 pixel slots**. In this firmware the frame is reset at the pulse's start and restarted at its end, so pixels 798-799 of the previous frame (column 0, lines 1 and 0) are cut and the LED is blanked during the pulse.
- Sensor phase latency: one loop iteration, roughly 1 us = 0.01 pixel. No interrupt latency, no scheduler.
- A stepper's rotor runs behind the commanded field by a load angle of up to one full step (1.8 degrees = 4.0 pixel slots at 800 px/rev) depending on torque; that is the only mechanical phase error the open-loop design has, and the per-revolution reset removes its DC part.

## 4. Disk geometry and mapping

### 4.1 Hole spiral and notch (FACTS from `Models/mechanical tv disk.stl`, slice at z = -1.55)

Disk: diameter 250.0 mm; plate from z = -1.6 to -1.0 (0.6 mm); hub cone toward -z from z = -1.5 to -11.5, bore 5.0 mm, 32 facets; stiffener ring r 73-75 mm from z = -2.5; four lightening cutouts r 15-70 mm between two crossed 10 mm spokes. All of this matches the user's `nipkow_disk.scad` constants (lines 40-62).

Holes: 32 square holes, 1.0 mm side (interior area 0.991 mm^2 each), **hole i at radius 84.5 + 1.0 i mm and angle 180 - 11.25 i degrees** (STL centroids within 0.01 mm / 0.01 degree). Spiral pitch 1.0 mm per 11.25 degrees, 31 mm span over 348.75 degrees. First and last few, from the slice:

| i | angle (deg) | r (mm) |
| --- | --- | --- |
| 0 | 180.000 | 84.500 |
| 1 | 168.750 | 85.500 |
| 2 | 157.500 | 86.500 |
| ... | ... | ... |
| 16 | 0.000 | 100.500 |
| ... | ... | ... |
| 30 | -157.500 | 114.500 |
| 31 | -168.750 | 115.500 |

Notch: 1.0 mm wide, from r = 120.0 to the rim (125.0), centred at **-168.75 degrees** (STL rim points at -168.51 and -168.99 degrees, r 120.0). **It is on the same radial line as hole 31**, the outermost hole, and 11.25 degrees (25 pixel slots) from hole 0.

Window: holes span r 84.5-115.5 (31 mm radial = 32 columns at 1.0 mm pitch) and each hole sweeps 11.25 degrees = 19.6 mm of arc at r = 100 mm (25 lines of 0.79 mm). `ledfixture.stl` is a 37.6 x 19.6 mm aperture (x -118.8..-81.2, y +-9.8) standing 29 mm tall on the +z side (z 4..33), centred on disk angle **180 degrees** (the -x axis) and exactly covering that window.

### 4.2 Mapping, direction of rotation and phase (FACT for the formula, INFERENCE for the physical orientation)

- Time-ordered hole k (k = 0 is the first hole whose scan starts at the sync edge) maps to column `x = 31 - k`; within a hole, slot j = 0..24 maps to row `y = 24 - j` (.ino:168-169). The Python mapping is identical (`capture.py:1161-1166`) with `shifted_i = (i + phase_offset) % 800` applied first and the two inversion flags.
- The base STL (`Models/mechanical tv base.stl`, same Blender frame: motor mount at the origin, holder straddling the rim) has the sensor holder at r 117-133, tangential extent -10.2..+9.9 mm about -174.5 degrees, i.e. centred at **-174.6 degrees**, with a pocket open from z = -5 to +7 for the module and a 4.0 mm rim slot from z = -2.0 to +2.0 through its walls. The LED window is at 180 degrees. Angular distance 5.4 degrees.
- For the sync edge to coincide with "first hole enters the window" without a phase parameter, the notch must reach the sensor 5.625 degrees (half a window) before reaching the window centre. With the sensor 5.4 degrees from the window that holds only if the disk turns **clockwise as seen from the +z (LED fixture) side**: the notch passes the sensor, 5.4 degrees later it is at the window centre, and hole 31 (which shares the notch's angle) entered the window 0.225 degrees = 0.5 px *before* the notch centre reached the sensor axis. Add 1-2 px for the trailing-edge sync and bitluni's first displayed slot lands 1.5-2.5 px into hole 31's pass: a fixed misregistration of about 2 lines in the first column, which he evidently accepted.
- Hence under his rotation the hole order after the sync edge is 31, 30, ..., 0 (radius decreasing 115.5 -> 84.5 mm) and `x = 31 - k` makes **column x = physical hole x, at radius 84.5 + x mm**: column 0 innermost, column 31 outermost. In the STL frame at the 180 degree station, clockwise-from-+z motion is +y, so slot j = 0 (y = 24) is at the bottom and y = 0 at the top: the image is upright for a viewer on the -z (motor) side and mirrored for a viewer on the +z side. The LED box is on +z and the screen must be on the other side, so the viewer is on the motor side and sees the disk turn **counter-clockwise**. (INFERENCE from geometry + code; the video would confirm it. The Python app's `invert_x`/`invert_y` cover the opposite build.)
- Opposite rotation also yields a clean raster (order 0, 1, ..., 31) but needs the sensor on the other side of the window; with bitluni's base the picture would arrive 354.6 degrees late and wrap.

**Phase-offset default for the user's build.** Let beta be the disk rotation between the software-detected sync edge and the moment the hole that follows the notch (hole 31 for clockwise-from-LED-side rotation) enters the window:

```
phase_offset_px = round(800 * beta / 360) mod 800
beta = (angle from the sensor's optical axis to the window centre, measured along the rotation)
       - 5.625 deg
       - (0 if the leading edge of the notch is used; about +0.5..+1 deg = 1..2 px if the trailing edge is used)
```

For bitluni's placement beta is about -0.2 degrees, so 0 is the right default **only if the sensor leads the window by about 5.6 degrees**. A sensor co-located with the window needs about 800 - 12.5 = 787; one 90 degrees upstream needs about 200 - 12 = 188. The user's slider (0-799) and the single-column test pattern are the practical way to find it; the formula predicts where to look.

### 4.3 Rim clearance at the sensor holder (FACT from STL, consequence is a HYPOTHESIS about the user's print)

Probing the base solid at the holder (-174.5 degrees): at r 118-119 the wall is solid for z in [-4.0, -2.0] and [2.0, 5.0] and open for z in (-2.0, 2.0); at the holder's side walls (-178 to -171 degrees, r 119-128) the same 4 mm gap exists; vertex levels sit exactly at z = -2.0 and +2.0. bitluni's plate (z -1.6..-1.0) therefore has **0.4 mm clearance toward the hub and 3.0 mm away from it**. The user's SCAD (`hub_side_fraction = 0.5`, plate 1.75 mm) puts the plate at z -2.175..-0.425: 0.175 mm into the hub-side wall, before print tolerance. `hub_side_fraction = 0` would give z -1.6..+0.15 (0.4 / 1.85 mm). The real axial position also depends on how far the hub is pressed on the shaft, so measure (B1) before re-printing.

## 5. Video source and frame format (FACTS from `WebSocketScreenShare.html` and `websocket.h`)

- Source: `navigator.mediaDevices.getDisplayMedia({video:{cursor:"always"}, audio:false})` (html:93-99, 141): any screen/window/tab, so any player. Frames are pulled with `requestVideoFrameCallback` (html:119-130).
- Scaling: `ctx.drawImage(video, 0, 0, 32, 25)` into a 32 x 25 canvas (html:122, 160-161); the browser does the downsampling. Greyscale: `0.2989 * pixels[a+2] + 0.5870 * pixels[a+1] + 0.1140 * pixels[a]` then `pow(c/255, gamma) * 255` (html:46-49). Note the Rec.601 weights are applied with R and B swapped (canvas ImageData is RGBA, so `pixels[a]` is R); harmless for luma, but do not copy it. Gamma slider 0.50-4.00, default 1.00 (html:101-106, 157-158).
- Packing: two pixels per byte, low nibble = even x, high nibble = odd x, row-major y then x: `buff[i++] = (R>>4) | ((R_next>>4)<<4)` (html:18-35) = **400 bytes per frame**. The ESP32 expands `nibble * 0x11` into 8-bit (websocket.h:35-44): 16 grey levels.
- Rate: a frame is sent only if `socket.bufferedAmount === 0` and at least 25 ms have elapsed (html:118-128), so at most 40 fps and never queued; the ESP32 displays the latest complete frame once per revolution (3.7).
- Control: a 1-byte binary message sets `targetFps = byte * 0.1` (websocket.h:26-27); the slider sends 10..250 -> 1.0..25.0 fps, default 100 -> 10.0 (html:108-115, 155). Text messages are ignored (websocket.h:18-23). WebSocket server on port 81 (websocket.h:1).
- Resolution 32 x 25 = 800 pixels, no audio, no SD card, no on-device decoding of any kind. **The ESP32 never touches compressed video**: the heavy lifting (decode, scale, grey, gamma) is in the browser.

## 6. What bitluni's approach solves for this project, and what Linux user space cannot copy

### 6.1 The problems as the Python app has them (cross-referenced)

- Sensor timestamps are taken in a Python callback with `time.perf_counter()` after RPi.GPIO's edge thread has acquired the GIL (`sensor.py:73`); "lock" requires |fps - target| < 3 % **and** jitter < 2 ms (`sensor.py:86-94`). 2 ms = 16 pixel slots = 7.2 degrees. Any GIL hold of a few ms (the LED thread busy-waits for a whole 100 ms revolution; the 5 ms switch interval; numpy capture; Tk) shows up as "jitter" and drops the lock, even when the disk itself is perfectly steady. HYPOTHESIS: the "locks for a second, then unlocks" symptom is measurement jitter, not disk jitter. Bench check B4 separates the two.
- The P-only PLL (`motor.py:714`, +-5000 us trim) rewrites the hardware-PWM period from those jittery timestamps, injecting speed noise into a motor that, being a stepper, was already synchronous. bitluni never changes the step rate from a measurement.
- LED pixel timing is a Python busy-wait on `perf_counter` per 125 us slot for 800 slots per revolution: any preemption longer than a slot shifts every following pixel.

### 6.2 What to copy (architecture, not code)

1. **One time base for STEP and pixels.** The stepper is the clock. On the Pi, hardware PWM on GPIO 18 already gives an exact STEP rate from the SoC's oscillator. Derive the LED pixel stream from a clock of the same oscillator (the PWM channel's own clock, PCM/I2S or SPI with DMA) so the pixel-to-step ratio is fixed by construction, as `ticksPerPixel = ticksPerFrame/800` and `ticksPerHalfStep = ticksPerFrame/400` are in the firmware.
2. **Sensor = phase reset, nothing else.** Use the index edge to (re)start the 800-slot stream and to pace the ramp; use its period only for diagnostics and stall detection (missing pulse or > a few % off the commanded period). Drop the lock/PLL gating from the display path.
3. **Gentle, per-revolution ramp**: 0.2 fps per revolution from 1 fps, which is 0.2 rev/s^2 at the start. The Python ramp's 2 fps/s from standstill is ten times harsher where the motor is weakest.
4. **Frame swap at the sync edge**, latest-frame-wins, triple buffer with atomics.
5. **Blank during the sync pulse**, start the frame on a defined edge, and keep the pixel counter free-running until the next edge (restart on early, hold on late).
6. 200 STEP pulses per revolution in full-step mode worked for him at 600 RPM with a 30 g disk. Microstepping (1/16) is smoother but needs 32 kHz; the A4988 accepts it (tA = tB = 1 us), but see B2: the software's assumed mode and the MS pins must agree.

### 6.3 What a Linux user-space port cannot copy directly

- A core that spins with ~1 us loop granularity and no preemption. On Raspberry Pi OS (non-RT kernel) user-space scheduling latency is tens to hundreds of microseconds and occasionally milliseconds; with `SCHED_FIFO`, `mlockall`, `isolcpus`/`nohz_full` and IRQ affinity it can be kept in the tens of microseconds most of the time but not guaranteed. A 125 us slot cannot be placed reliably from a user-space busy loop.
- Cycle-counter polling of the sensor. The nearest equivalent is the GPIO character device: `struct gpio_v2_line_event.timestamp_ns` is the "best estimate of time of event occurrence, in nanoseconds", from `CLOCK_MONOTONIC` by default (Linux 6.12 `include/uapi/linux/gpio.h:275-294`). On the BCM2711 that stamp is taken in the kernel IRQ path, so it has IRQ latency (microseconds) but no scheduler or GIL latency: use it instead of user-space `perf_counter()`.
- Direct register writes to the LED pin per pixel. Move the per-pixel work to a peripheral: build the revolution's bitstream (800 slots x N sub-slots) and let DMA/SPI/PCM clock it out; or use pigpio-style DMA waveforms (1 us resolution). Then the CPU only decides when a revolution's stream starts.
- If the port must keep bit-exact behaviour, the faithful route is an MCU (RP2040 PIO or an ESP32-S3 running this very loop) fed by the Pi over a serial/USB link with 400-byte frames, which is literally bitluni's split (browser does video, MCU does timing).

## 7. Claims, estimates, hypotheses (condensed)

FACTS: items 1-9 in section 1; all line-cited statements in sections 3-5; A4988 datasheet values: STEP acts on the low-to-high transition, tA = tB = 1 us minimum, tC = tD = 200 ns, MS1/MS2/MS3 = L/L/L full step, H/H/H sixteenth step, MS1 and MS3 100 k pull-down, MS2 50 k pull-down, ITripMAX = VREF/(8 RS); Pololu carrier: 0.068 ohm sense resistors since 2017, about 1 A/phase without a heatsink, 8-35 V; L298 datasheet: source turn-on delay 2 us, sink turn-on delay 1.6 us, source turn-off delay 1.5 us, sink turn-off 0.7 us, rise 0.7/0.2 us, fall 0.2/0.25 us, commutation 25-40 kHz (adequate for the 8 kHz pixel rate, but pulses under about 3 us will not reach full current); 17HE08-1004S: 17 N cm holding torque, 1.0 A, 3.6 ohm, 4.0 mH.

ESTIMATES (arithmetic shown in sections 3.6, 3.9 and below): ramp 11.98 s; 2.000 kHz STEP at 10 fps full-step vs 32 kHz at 1/16; notch pulse 130-260 us; load angle 4 px per full step; disk mass from the SCAD geometry with PLA 1.24 g/cm^3 and net plate area 36,600 mm^2 (49,087 full circle minus 12,487 cutouts): 0.6 mm -> 27 g plate + about 3 g hub/ring = 30 g; 1.75 mm -> 79 + 3 = 82 g; 2.4 mm -> 109 + 3 = 112 g (all three agree with the user's README). Inertia: I = 0.5 m_full R^2 - 0.5 m_cut (r1^2 + r2^2) = 2.6e-4 (0.6 mm), 7.6e-4 (1.75 mm), 1.05e-3 kg m^2 (2.4 mm); rotor inertia of a 23 mm NEMA 17 is of order 3e-6 and negligible. Acceleration torque at 2 rev/s^2 (12.57 rad/s^2): 0.33 / 0.96 / 1.31 N cm; at bitluni's start-of-ramp 0.2 rev/s^2 a tenth of that. Air drag of a 250 mm disk at 600 RPM, laminar (Re = 6.5e4, Cm = 3.87/sqrt(Re) = 0.015): T = 0.5 Cm rho omega^2 R^5 = 1.1e-3 N m = 0.11 N cm. Start-up step rates: bitluni 200 steps/s (60 RPM) from rest; Python app 0.5 fps x 3200 = 1600 microsteps/s = 30 RPM if the driver is really at 1/16, but 480 RPM from rest if the driver is in full step.

HYPOTHESES: H1 the user's A4988 is not in 1/16 mode (MS pins floating = full step), so the software commands 16x the intended speed and the motor cannot start (B2). H2 the thick disk rubs the sensor holder on the hub side (section 4.3, B1). H3 the sync "unlock" is timestamp jitter from the GIL/scheduler, not disk jitter (B4). H4 bitluni's sensor module is HIGH during the notch, the user's may be the opposite polarity, which only shifts phase by 1-2 px (B3). H5 the motor supply is at the low end (9 V) which limits torque at 2 kHz full-step / 32 kHz microstep rates (not analysed here; motor agent).

## 8. Recommendations

| # | Action | Rationale | Priority |
| --- | --- | --- | --- |
| R1 | In the C++ port, make the stepper the master clock: STEP from hardware PWM, LED stream from a DMA-fed peripheral on the same oscillator, 800 slots per 200 (or 3200) steps by construction; use the sensor only to start/re-phase the stream, pace the ramp and detect stalls. Remove the PLL and the lock gate from the display path. | This is exactly bitluni's working architecture; it removes the two user-space timing paths (sensor callback, LED busy-wait) that are the weakest on Linux. | high |
| R2 | Verify the A4988 microstep mode (B2) before any software work, and make `MTV_MICROSTEPS` fail loudly if the measured revolutions per 3200 pulses is not 1. | bitluni drives 200 pulses/rev; the Python app assumes 3200. If the MS pins are open the motor is commanded 16x too fast and stalls at start. | high |
| R3 | Measure the rim-to-holder gap (B1); if tight, re-render the disk with `hub_side_fraction = 0` (or seat the hub 0.6 mm further out) rather than thinning the plate. | bitluni's holder slot is z -2..+2 and his plate z -1.6..-1.0; the symmetric thickening pushes the user's plate 0.18 mm into the hub-side wall. | high |
| R4 | Ramp like the firmware: 0.2 fps per revolution from 1.0 fps (about 12 s to 10 fps), advanced by the index pulse, not by wall-clock. | Ten times gentler at standstill where a stepper has the least margin; also a free stall interlock. | medium |
| R5 | Take sensor timestamps from the kernel (gpiod v2 line events, `timestamp_ns`), from a `SCHED_FIFO` thread with `mlockall`; report jitter from those stamps. | Removes GIL/scheduler latency from the measurement that currently decides "lock". | medium |
| R6 | Start each frame on a defined edge, blank the LED during the sync pulse, let the slot counter free-run to the next edge (restart on early, hold on late), swap frames at the edge, latest frame wins. | Mirrors .ino:136-174 and avoids tearing. | medium |
| R7 | Default `phase_offset` from geometry: `round(800 * beta/360)` with beta = sensor-to-window angle - 5.625 deg (section 4.2); expose it as degrees as well as pixels. | bitluni's "0" is only correct for a sensor 5.4 deg upstream of the window. | low |
| R8 | Keep the right-aligned or centred single pulse per slot with a 3 us minimum when using an L298N; prefer a logic-level MOSFET for the LED. | L298 turn-on delays are 1.6-2 us, so sub-3 us pulses never reach full current; a MOSFET removes the 2+ V drop and the delays. | low |
| R9 | If the port cannot meet 125 us slot timing from user space in tests (B5), move the real-time loop to an RP2040/ESP32-S3 and keep the Pi for video. | That is bitluni's split and it demonstrably works. | low |

## 9. Bench checks for the user

- **B1 (rim clearance / rubbing):** with power off, turn the disk slowly by hand through a full revolution and listen/feel at the sensor holder; slide a 0.3 mm and a 0.5 mm feeler (or paper, 0.1 mm per sheet) between the rim and the hub-side wall of the holder slot. Measure the slot width with calipers and where the plate sits in it. Expected per bitluni's base: 4.0 mm slot, plate 0.4 mm from the hub-side wall for a 0.6 mm plate, -0.18 mm (interference) for the 1.75 mm plate centred as in the SCAD.
- **B2 (microstep mode):** (a) multimeter on MS1, MS2, MS3 with the board powered: all three must read > 2 V for 1/16; floating pins read 0 V (100 k / 50 k pull-downs). (b) Functional: with the LED off, write `echo 1000000 > /sys/class/pwm/pwmchip0/pwm0/period; echo 500000 > .../duty_cycle; echo 1 > .../enable` (1 kHz STEP) and count index pulses for 32 s with `gpiomon -r gpiochip0 4` (or the app's RPM readout): expect 10 revolutions in 32 s at 1/16 (3200 pulses/rev), 160 revolutions at full step.
- **B3 (sensor polarity and pulse width):** `gpioget gpiochip0 4` with the disk stopped with the rim in the slot, then with the notch in the slot. Then at speed `gpiomon -F "%e %s.%n" gpiochip0 4` (both edges) and compute HIGH duration: expect 130-260 us for HIGH during the notch, or 99.7-99.9 ms for the opposite polarity. Whichever it is, the Python app's RISING edge is one edge per revolution; only the phase shifts by 1-2 px.
- **B4 (disk jitter vs measurement jitter):** record 200 consecutive index periods with `gpiomon` (kernel timestamps) while the app is running and while it is stopped (motor driven by the sysfs PWM alone). If kernel-timestamp jitter is < 100 us but the app reports > 2 ms, the "unlock" is software latency (H3). If the kernel timestamps themselves wander by milliseconds at a fixed PWM period, the motor is losing steps or oscillating (load angle / resonance), which the user hears as a change in pitch.
- **B5 (can user space hold a 125 us slot?):** run `cyclictest -t1 -p 99 -i 125 -l 100000 -m` (package `rt-tests`) with the capture and UI running: the max latency column must stay well below 125 us for a user-space LED loop to be viable; typical stock-kernel results of 100-500 us max say it is not, which decides between R1's DMA path and R9.
- **B6 (STEP waveform sanity):** scope or logic analyser on GPIO 18 at target 10 fps: expect 32.0 kHz / 50 % if the software assumes 1/16; bitluni's equivalent is 2.0 kHz. Compare with the measured RPM from the index sensor: RPM = 60 x f_STEP / (200 x microsteps).
- **B7 (phase default):** display the single-column test pattern at column 31, set `phase_offset` with the slider until it lands on the outermost column, and record the value; compare with `round(800 * beta/360)` from the measured sensor angle (section 4.2) to confirm the rotation-direction inference.
