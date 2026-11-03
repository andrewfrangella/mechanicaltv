# Root-cause analysis of the Python mechanical_tv application: why "sync" never holds and why the LED picture cannot be stable on a Pi 4

Date: 2026-11-03. Scope: the code in the repository (Python version, main at 4c64391) at git `4c64391` (main), read completely: `mechanical_tv/hardware/{sensor,led,motor,controller}.py`, `mechanical_tv/{capture,v4l2,studio,app}.py`, `docs/ARCHITECTURE.md`, plus `install.sh`, `logs.py`, `packaging/*`, `docs/HARDWARE.md`, `docs/INSTALL.md`. Platform facts were checked against primary sources (kernel and library source, datasheets, Debian/Pi package data); each statement below is tagged **FACT** (verified from a source, cited), **ESTIMATE** (my arithmetic, shown) or **HYPOTHESIS** (plausible, not verified). Nothing in the repository was edited.

---

## 0. Verdict in one page

The two reported symptoms (lock that holds for about a second then drops; picture that never stabilises) are produced by the *design* of the timing path, with CPython's GIL as the amplifier. Ranked by contribution:

| Rank | Defect | Where | Contributes to |
| --- | --- | --- | --- |
| 1 | The index edge is **timestamped in a Python callback** (`time.perf_counter()` after the event has crossed kernel → lgpio thread → FIFO → lgpio Python thread → shim → callback), while a **pure-Python busy-wait thread (the LED worker) holds the GIL for the whole 100 ms frame**. CPython only forces a GIL hand-off after a waiter has waited 5 ms, so the callback's latency is ~0.5 ms when the LED thread is idle and ~5 ms (or 10, 15 ms with other waiters queued) when it is busy. The "jitter" the lock tests is the *difference* of consecutive latencies, so it crosses the 2 ms (and the 3 % band) threshold whenever the latency mode changes. | `sensor.py:173`, `led.py:228-249`, CPython `ceval_gil.c` | lock flapping (dominant), picture phase |
| 2 | The LED frame is **not aligned to the edge at all**: pixel 0 starts at `time.perf_counter()` when the worker gets round to it (`led.py:228`), the pixel clock is `previous measured period / 800` (`led.py:225`), which carries the same latency noise (±5-10 ms ⇒ ±5-10 % pixel-clock error ⇒ 40-80 px displacement by the end of the frame), and a new frame can only start after the previous 800-pixel loop finishes (`led.py:207-213`). The kernel's event timestamp (accurate to the IRQ, tens of µs) is thrown away. | `led.py:206-249`, `controller.py:196-230` | picture instability (dominant), frame skips |
| 3 | **Per-pixel software bit-banging under the GIL**: two `RPi.GPIO.output()` calls per 125 µs slot through the rpi-lgpio shim (Python → lgpio → `ioctl`), ~10-15 µs each (estimate), spin-waits for the rest. Every GIL hand-off (≈100 per second from the motor loop, capture, Tk, supervisor, sensor) freezes the worker mid-pixel for 0.1-5 ms: if it happens during the ON spin the LED stays on for a 1-40 px streak, then the loop "catches up" by emitting pixels back-to-back (compressed). The minimum representable pulse is not 5 µs but ~15-20 µs (posterised shadows). | `led.py:234-249`, rpi-lgpio `output()` | picture noise/smear, black crush |
| 4 | Motor **starts instantly at 0.5 rev/s** (1600 µsteps/s with no ramp from zero): a pull-in test the 80 g, R = 125 mm disk barely passes with a 17 N·cm motor at full current and fails with any current shortfall. If the MS1-3 pins are not all HIGH the A4988 defaults to full step and the same 1600 Hz means 8 rev/s from standstill: certain stall. P-only "PLL" trim on a crystal-timed stepper does nothing useful (and is below the 20 ns PWM quantum most of the time). | `motor.py:722-735`, A4988 datasheet | start-up stall (and lock if the motor slips) |
| 5 | Backend selection chain **swallows every exception** and falls back to **synthetic pulses** at the target FPS (`mock-fallback`), or to a `gpiod` thread that dies silently on Trixie's libgpiod 2.x. In those states the LED fires on an internal `time.sleep` clock, the lock flaps on sleep + GIL jitter, and the picture rolls against the disk. The HUD "INDEX SENSOR" cell is the only place this is visible. | `sensor.py:216-264`, `app.py:777` | both symptoms, if this path is active (bench check 1) |
| 6 | The LED's own 350 ms watchdog is fed one line before it is checked, so it can never trip; the only stall interlock is a 10 Hz supervisor. The L298N-plus-resistor LED chain can pass ~2 A through a 700 mA LED if the pin is left HIGH (GIL stall during ON, daemon-thread kill, crash). | `controller.py:214-224`, `led.py:189-204`, `docs/HARDWARE.md:96` | safety, not the symptoms |

A C++ rewrite that merely translates this design would remove the GIL (rank 1 shrinks from milliseconds to ~50-500 µs of scheduler jitter on a non-RT desktop) but would keep ranks 2, 4, 5 and 6 unchanged and keep rank 3 as scheduler-induced smears. The design must change: hardware-generated pixel waveform (DMA-fed PWM serialiser or SPI, or a Pico PIO), kernel/hardware edge timestamps, frames slaved to the edge (or a pixel clock derived from the step clock, 4 microsteps per pixel), ramp from zero. Section 10 lists what the port must do; section 11 lists bench checks that confirm or rule out each hypothesis on the user's Pi.

---

## 1. Platform facts established

| # | Statement | Tag | Source |
| --- | --- | --- | --- |
| P1 | Raspberry Pi OS "based on Debian Trixie" was released 2025-10-01 with Linux 6.12.47; later images 6.12.75 and (2026) 6.18.x. The user's `v4l2-ctl` rejecting `--fix-edid-checksums` is consistent with trixie's v4l-utils **1.30.1-1** (bookworm had 1.22). | FACT | Pi OS release notes [S1]; packages.debian.org v4l-utils [S2] |
| P2 | Debian trixie ships **python3-libgpiod 2.2.1** (libgpiod 2 API: no `Chip.get_line`, no `LINE_REQ_EV_RISING_EDGE`), and still carries the original **python3-rpi.gpio 0.7.1~a4-1.2**. | FACT | packages.debian.org [S3][S4] |
| P3 | Original RPi.GPIO 0.7.1 `add_event_detect` fails on Pi kernels ≥ 6.6 with `RuntimeError: Failed to add edge detection`; cause per the Pi kernel maintainer: the main GPIO controller's global numbers now start at 512 and sysfs GPIO is going away; the recommended replacement is the **rpi-lgpio** shim (`python3-rpi-lgpio`) or gpiozero. Output via `/dev/gpiomem` is unaffected. | FACT | raspberrypi/linux issue #6037 [S5]; rpi-lgpio PyPI [S6] |
| P4 | The rpi-6.12.y bcm2711_defconfig has `CONFIG_PREEMPT=y`, `CONFIG_HIGH_RES_TIMERS=y`, `CONFIG_GPIO_SYSFS=y`, `CONFIG_PWM_BCM2835=m`, no `CONFIG_HZ` line (so the Kconfig default **HZ=250**, 4 ms jiffy). No PREEMPT_RT. | FACT | bcm2711_defconfig [S7]; kernel/Kconfig.hz [S8] |
| P5 | Pi 4 hardware PWM: the Pi kernel DT sets the PWM clock to **50 MHz** (`assigned-clock-rates = <50000000>` in bcm283x.dtsi, included by bcm2711.dtsi; pwm1 likewise). The `pwm` overlay does not change it (its `clock` param maps onto `assigned-clock-rates` but has no default). Upstream Linux uses 10 MHz; the BCM2835 peripherals manual says "nominally 100 MHz". | FACT | bcm283x.dtsi, bcm2711.dtsi, pwm-overlay.dts, overlays README [S9][S10][S11][S12]; BCM2835 ARM Peripherals ch. 9 [S13] |
| P6 | `pwm-bcm2835.c` computes `period_cycles = DIV_ROUND_CLOSEST_ULL(state->period * rate, NSEC_PER_SEC)`, `PERIOD_MIN = 2`, duty likewise; so the sysfs period is quantised to **1/50 MHz = 20 ns**. The driver only sets `PWM_MODE`; it never uses the serialiser or FIFO/DMA. | FACT | pwm-bcm2835.c [S14] |
| P7 | The BCM2835/2711 PWM block has two channels, a serialiser mode (`MODEi`), M/S mode (`MSENi`), a shared FIFO with DMA (`DMAC`, DREQ 5), "both modes clocked by clk_pwm". | FACT | BCM2835 ARM Peripherals ch. 9 [S13] |
| P8 | rpi-lgpio implements `bouncetime` by calling `lgpio.gpio_set_debounce_micros(chip, gpio, bouncetime*1000)`; the user callback receives only the channel: `_Alert._call(self, chip, gpio, level, timestamp)` discards `timestamp`. `output()` does `_gpio_list` → `gpio_get_mode` (C) → `_check_output` → `gpio_write` (C) per call. `setup()` on an input frees and reclaims the line (so Stop → Start in the app works). | FACT | rpi-lgpio 0.6 `RPi/GPIO/__init__.py` lines 81-128, 303-320, 633-710, 727-763 [S15] |
| P9 | lgpio's debounce is implemented in **lgpio's own alert thread**, not the kernel: "a rising edge is reported if and only no other edge is detected for at least debounce nanoseconds after it occurred"; reported timestamp = `last_evt_ts + debounce_nanos`. Only the requested edge flags are passed to the kernel (`xMakeFlags`), so with RISING only the falling edge of the notch pulse is never seen and does not reset the window. The thread polls with a **0.5 ms** `ppoll` timeout, times out pending debounces with a 50 µs leeway, and holds every report back **500 µs** (`emit(count, nowGT-500000)`) for ordering. | FACT | lgpio 0.2.2.0 `src/lgPthAlerts.c` 287-333, 438, 562, 592; `src/lgGpio.c` 228-240, 1108-1117 [S16]; lgpio docs [S17] |
| P10 | lgpio Python callbacks run in a **Python thread** (`_callback_thread`, daemon) that blocks in `self._file.read(16)` on the `.lgd-nfy<n>` FIFO and unpacks `tick, chip, gpio, level, flags` (`struct 'QBBBBI'`); `tick` is the kernel's ns timestamp (`ktime_get_ns()`, i.e. CLOCK_MONOTONIC, taken in **hard-IRQ** context by `edge_irq_handler`). | FACT | lgpio_extra.py 262-325 [S16]; gpiolib-cdev.c v6.12 `edge_irq_handler`, `line_event_timestamp` [S18] |
| P11 | CPython: `DEFAULT_INTERVAL 5000` µs; "A thread wanting to take the GIL will first let pass a given amount of time (`interval` microseconds) before setting gil_drop_request", checked "at every turn of the eval loop"; FORCE_SWITCHING "ensures that another GIL-awaiting thread gets scheduled". | FACT | ceval_gil.c (3.13) [S19]; `sys.setswitchinterval` docs [S20] |
| P12 | `_tkinter` releases the GIL while Tcl runs (`ENTER_TCL` = `Py_BEGIN_ALLOW_THREADS` + `tcl_lock`). NumPy "releases the GIL for many low-level operations" (not for object dtype). | FACT | _tkinter.c [S21]; NumPy thread-safety doc [S22] |
| P13 | V4L2 MMAP buffers from videobuf2-dma-contig are mapped with `dma_mmap_attrs` → `dma_pgprot` → for a non-`dma-coherent` device `pgprot_dmacoherent` = arm64 **MT_NORMAL_NC** (non-cacheable). bcm2711.dtsi has no `dma-coherent` property. (That unicam uses vb2-dma-contig is from memory of `bcm2835-unicam.c`, not re-fetched.) Measured in-repo: ~8 ms per converted frame. | FACT (chain) | videobuf2-dma-contig.c, kernel/dma/mapping.c, arm64 pgtable.h [S23][S24][S25]; `docs/ARCHITECTURE.md:153` |
| P14 | A4988: STEP minimum HIGH/LOW 1 µs; MS1/MS3 100 kΩ and MS2 50 kΩ internal **pull-downs** (floating = full step); `ITripMAX = VREF/(8·RS)`; TSD typ 165 °C; `IOUT ±2 A` abs max; `TJ(max)` 150 °C; after TSD/UVLO the translator resets to Home. Pololu: ~1 A/phase without heatsink, 2 A with cooling; boards since 2017 use 0.068 Ω (1 A ↔ VREF 0.54 V), earlier 0.050 Ω; VMOT 8-35 V; full-step limits coils to ~70 %. | FACT | Allegro A4988 datasheet [S26]; Pololu #1182 [S27] |
| P15 | Motor 17HE08-1004S (vendor listing): holding torque **17 N·cm**, 1.0 A/phase, 3.6 Ω, "rated input voltage 12-24 V DC". (Repo docs: 1 A, 3.6 Ω, 4 mH.) The sibling 17HS08-1004S: 0.16 N·m, 3.7 Ω, 4.5 mH. | FACT (listing data, medium confidence) | eBay product page [S28]; maritex [S29]; `docs/HARDWARE.md:50` |
| P16 | Published timing numbers: Python on lgpio (via GPIO Zero) toggles at ~24 kHz on a Pi 4 (≈20 µs per transition); RPi.GPIO software PWM on a Pi 4 shows ~±50 µs pulse-width jitter on a scope; original 2015 RPi.GPIO benchmark 70 kHz on a Pi 1/2. No published distribution of CPython busy-wait jitter on a loaded Pi 4 was found; the figures in sections 3-4 are estimates. | FACT (that these were published) | i-programmer [S30]; Barkhausen Institut [S31]; codeandlife [S32] |

---

## 2. Sensor backend chain (`sensor.py`) and what the HUD shows

### 2.1 The chain

`create_opto_sensor()` (`sensor.py:296-313`): `import RPi.GPIO` ⇒ `GPIOOptoSensor`; else `import gpiod` ⇒ `GPIOOptoSensor`; else `MockOptoSensor`. Then `GPIOOptoSensor.start()` (`sensor.py:207-242`) tries, **swallowing every exception with bare `except Exception: pass`** (lines 226-227, 236-237):

1. `RPi.GPIO`: `setmode`, `setup(4, IN, PUD_DOWN)`, `add_event_detect(4, RISING, callback, bouncetime=20)` → backend `"RPi.GPIO"`.
2. `gpiod`: sets backend `"gpiod"` and starts `_gpiod_loop`, whose body is wrapped in `try/except Exception: pass` (256-257). The loop uses the **libgpiod 1.x API** (`chip.get_line`, `gpiod.LINE_REQ_EV_RISING_EDGE`, `line.event_wait`).
3. `"mock-fallback"`: `_mock_loop` calls `time.sleep(1/target_fps)` then `_handle_edge()` (259-264) — **synthetic pulses at the target FPS, unrelated to the disk**.

The HUD "INDEX SENSOR" cell prints `sensor.get('backend', sensor.get('driver'))` (`app.py:777`), so the four possible states look like this:

| State on Trixie (kernel 6.12) | What happens | HUD "INDEX SENSOR" | Lock | LED |
| --- | --- | --- | --- | --- |
| A. `python3-rpi-lgpio` installed (shim) | Real edges via lgpio; each reported ~20.5-21 ms after the edge (20 ms debounce + 0.5 ms hold) plus GIL wait; timestamp taken by `perf_counter()` in the callback. | `RPi.GPIO · GPIO 4 · N pulses`, N rising 10/s | flaps (section 3) | fires on real edges, phase noisy (section 4) |
| B. original `python3-rpi.gpio` 0.7.1 (Debian) or pip `RPi.GPIO`, no gpiod | `add_event_detect` raises (P3) → swallowed → gpiod import fails → **mock-fallback**. Output on GPIO 21 via `/dev/gpiomem` still works, so the LED lights. | `mock-fallback · GPIO 4 · N pulses` | flaps on `time.sleep` + GIL jitter; measured FPS always ≈ target | fires at 10.0 Hz from `time.sleep`, **rolls/drifts against the disk** |
| C. no RPi.GPIO, `python3-libgpiod` 2.2.1 present | backend `"gpiod"`, `_gpiod_loop` raises `AttributeError` on `chip.get_line` → swallowed, thread exits. LED `_init_gpio` also fails on gpiod 2 → LED `"mock-fallback"` (pin never driven). | `gpiod · GPIO 4 · 0 pulses` | never | dark |
| D. neither importable | `MockOptoSensor` (pulses at `simulated_fps`, which the supervisor sets to the motor's ramp value every 100 ms, `controller.py:240-241`); LED `GPIOLEDModulator` with backend `"mock-fallback"` — still busy-waits 800 pixels per frame doing nothing. | `MockOptoSensor · GPIO 4 · N pulses` | flaps mildly | dark |

Because `hardware_enabled` is decided by the **motor driver only** (`studio.py:150-151`), states B, C and D still show the green "REALTIME HARDWARE" pill. The `connected` flag is `True` for `mock-fallback` (`sensor.py:292`). Nothing is logged. The user reports a visible-but-unstable picture, which rules out C and D and leaves **A or B**: bench check 1 decides. Note that in state B the lock *and* the LED are driven by a sleep clock: the picture would precess continuously (the disk's true period differs from 100.000 ms by the PWM quantisation, P6, and by any slip), which also reads as "never stabilises".

### 2.2 Debounce semantics (state A) — the trap is not where the task text expected

The notch is 1.0 mm wide at r ≈ 120-125 mm. **ESTIMATE** of the pulse width at 10 rev/s: rim speed v = 2π·0.1225 m·10 s⁻¹ = 7.70 m/s; notch alone 1.0 mm / 7.70 m/s = 130 µs; adding a typical 0.5-1 mm interrupter aperture gives ≈ 200-260 µs. At 0.5 rev/s it is 20× longer (≈ 4-5 ms). Either way it is far shorter than the 20 ms `bouncetime`.

- If `bouncetime` were implemented as the **kernel's** debounce (gpiolib-cdev `debounce_work_func`: an edge is emitted only if the level is still different after `debounce_period_us`, so a pulse shorter than the period produces **no event** [S18]), the sensor would deliver zero pulses at any speed above ~0.1 rev/s. **It is not**: lgpio ignores the kernel attribute and implements its own rule in `lgPthAlerts.c` (P9). With only RISING requested, the kernel never reports the falling edge, so the 20 ms "quiet" window after the rising edge is satisfied and the edge **is** reported — delayed by 20 ms + up to 0.5 ms (poll) + 0.5 ms (emit hold), with `tick = edge + 20 ms`. So: pulses do arrive in state A, each one ≈ 21 ms late with ≈ 1 ms of lgpio-side delivery slop, before the GIL is even involved.
- `bouncetime` therefore adds a fixed 20 ms (= 160 pixels = 6.4 columns) phase offset that the operator has to dial out with the phase slider, and ~1 ms of extra delivery jitter. The rpi-lgpio docs explicitly warn "you may find shorter debounce periods preferable" and that signals with edges closer than the debounce "report no edges at all" [S33]. At 24 FPS (41.7 ms period) rising edges are still > 20 ms apart, so the window does not suppress them; above 50 rev/s it would. The in-code guard `period < 0.02 → return` (`sensor.py:177`) is a second, independent 50 rev/s limit; both are fine for 10-24 FPS.
- The GIL-free, IRQ-accurate `tick` that lgpio delivers is discarded twice: by the shim's `_Alert._call` (P8) and by `sensor.py:173`, which uses `time.perf_counter()` after the fact.

### 2.3 Defects

| ID | file:line | Mechanism | Symptom | Severity | C++ port must |
| --- | --- | --- | --- | --- | --- |
| S-1 | `sensor.py:173, 204-205` | Timestamp taken in a Python thread after FIFO + GIL; kernel timestamp discarded | ms-scale period noise ⇒ lock flap, LED phase noise | **Critical** | Use the GPIO v2 line-event `timestamp_ns` (hard-IRQ `ktime_get_ns()`), or a hardware capture; never timestamp in the consumer |
| S-2 | `sensor.py:216-242, 256-257` | All exceptions swallowed; silent fallbacks to synthetic pulses or a dead thread | LED on an internal clock, lock flapping, picture rolling; "REALTIME HARDWARE" still shown | **Critical** (if active) | Fail loudly; never synthesise index pulses unless the operator enabled free-run |
| S-3 | `sensor.py:222` | `bouncetime=20` via lgpio adds a 20 ms report delay and ~1 ms slop | fixed 6.4-column phase offset, extra jitter | Medium | No debounce on the index line; reject edges closer than 50 % of the expected period in software |
| S-4 | `sensor.py:244-255` | libgpiod 1.x API on a libgpiod 2.x system | thread dies silently, 0 pulses | High (Trixie) | Use libgpiod 2 or the raw cdev ioctls |
| S-5 | `sensor.py:183-196` | `jitter = |P − mean(last 10)|`; lock = within 3 % and jitter < 2 ms and ≥ 5 periods; no hysteresis; recomputed every edge | one bad period unlocks, one good relocks ⇒ visible flapping at 10 Hz UI | Medium | Lock on a filtered phase/period error with hysteresis (e.g. unlock after 3 consecutive misses), on hardware timestamps |
| S-6 | `sensor.py:292`, `studio.py:151` | `connected` true for mock-fallback; hardware pill from the motor only | operator misled | Medium | Per-subsystem health in the UI |

---

## 3. The lock definition under GIL contention: why it flaps

### 3.1 Latency chain for one index edge (state A)

| Stage | Delay | Variability | Tag |
| --- | --- | --- | --- |
| IRQ → `edge_irq_handler` timestamp | 5-30 µs | ±20 µs | ESTIMATE (PREEMPT, non-RT) |
| kernel → lgpio alert thread (`ppoll` 0.5 ms) | 0-0.5 ms | 0.5 ms | FACT (P9) |
| lgpio debounce window | 20 ms | timed out on the 0.5 ms poll grid ⇒ ±0.5 ms | FACT (P9) |
| lgpio emit hold-back | 0.5 ms | ~0 | FACT (P9) |
| FIFO write → lgpio Python thread wakes, **needs the GIL** | 0.1 ms if free; **≈5 ms** if a pure-Python thread holds it; **≈10, 15 ms** if other waiters are queued ahead | multimodal | FACT (P11) for the mechanism; ESTIMATE for the modes |
| shim `_Alert._call` → `_handle_edge` → `perf_counter()` | 30-80 µs | small | ESTIMATE |

Total ≈ 21-27 ms; the **variable** part is ≈1 ms from lgpio plus the GIL term.

### 3.2 Why the GIL term is ~5 ms almost always

`GPIOLEDModulator._pulse_loop` (`led.py:228-249`) is a pure-Python loop (`while time.perf_counter() < t_off: pass`) that runs for the entire frame (`period` seconds, i.e. ~100 ms) and never blocks. While it runs it holds the GIL. Any other thread that wants the GIL waits `interval` = 5 ms, then sets `gil_drop_request`, and the LED thread drops the GIL at its next bytecode (P11). So every wake-up of every other thread — the lgpio callback thread, the motor rate loop (50 Hz), the supervisor (10 Hz), the capture worker (≈30 dequeues + 15 conversions per second), the Tk tick (10 Hz), the log handlers — costs that thread ≈5 ms of latency and costs the LED thread a stall. The LED thread is idle only in the short gap between finishing one 800-pixel loop and receiving the next `_frame_ready` (it is in `Event.wait`, GIL released). Whether the index edge lands in that gap or in the busy part depends on the previous frame's start time and period estimate, so the callback latency alternates between the ~0.5 ms mode and the ~5 ms mode (and ~10 ms when another waiter wins the hand-off first).

### 3.3 Effect on the lock (ESTIMATE)

The sensor sees `P_k = P_true + (L_k − L_{k−1})`. With latency modes {0.5, 5, 10} ms, `P_k − P_true` takes values in {0, ±4.5, ±5, ±9.5, ±10} ms. The lock requires `|P_k − mean(last 10)| < 2 ms` **and** `|1/P_k − f_target| < 3 %` (3 % of 100 ms = 3 ms): a single mode change breaks both. If the mode changes on a fraction *q* of revolutions, the lock survives on average 1/q revolutions. With ~100 competing GIL requests per second (≈10 per revolution) the probability that at least one of them perturbs the callback's position in the queue is high; q ≈ 0.1-0.3 gives lock runs of 3-10 revolutions = 0.3-1 s, read by a 10 Hz HUD as "locks for about a second, then unlocks, repeatedly". The supervisor copies the flag into `state` every 100 ms (`controller.py:243-249`), and the pill colour follows (`app.py:753-754`).

A cross-check: with hardware timestamps the same disk would show period jitter of the order of the IRQ latency (tens of µs) plus true mechanical variation (a crystal-timed stepper has none at the revolution scale unless it slips). Bench check 4 measures both numbers side by side.

### 3.4 In state B (mock-fallback)

`time.sleep(0.1)` is hrtimer-accurate (P4) to ~0.1 ms, but after the sleep the thread needs the GIL (same ≈5 ms modes), and `_handle_edge` runs the callback chain, so the same flapping occurs; the measured FPS always centres on the target because the clock *is* the target.

---

## 4. The LED worker (`led.py:206-253`)

### 4.1 Per-pixel cost (ESTIMATE, with the published 24 kHz figure as anchor)

Each `_set_pin()` in state A goes Python → `RPi.GPIO.output()` (shim: build a list, a `tuple(bool(item) for item in value)` that raises and catches `TypeError` for an int, `gpio_get_mode` C call, `_check_output`, `gpio_write` C call → `ioctl(GPIO_V2_LINE_SET_VALUES)`). On a Pi 4 (Cortex-A72, 1.5 GHz, CPython 3.13) I estimate 8-15 µs per call; GPIO Zero on lgpio was measured at ~24 kHz toggling (≈20 µs per transition) [S30], consistent. Two calls per pixel ⇒ 16-30 µs of each 125 µs slot (13-24 %) is call overhead; `time.perf_counter()` in the spin costs ~0.3-0.5 µs per iteration, so the spin itself quantises to < 1 µs (fine).

Consequences:

- The ON time is **extended** by the latency of `_set_pin(0)` (~10 µs): `pulse_time = 5 µs` produces a real pulse of ~15 µs. Values with `pulse_time ≤ 5 µs` are skipped (`led.py:238`). At brightness 0.7, `pulse_time = 125·0.7·v/255` µs crosses 5 µs at v ≈ 15; so v < 15 is off, v = 15 lights like v ≈ 45. Posterised shadows and a lifted black floor on everything above the threshold (+10 µs ≈ +8 % of slot).
- Slot timing: `t_next += pixel_duration` and spin (`led.py:247-249`) keeps long-term slot timing exact relative to the loop's start, as long as the thread is not stalled.

### 4.2 GIL stalls mid-revolution (ESTIMATE)

Other threads request the GIL ≈100 times per second (section 3.2) ⇒ ≈10 forced hand-offs per 100 ms frame. During a hand-off the worker is frozen for the other thread's Python run: ~0.1-0.5 ms for the motor loop (two sysfs writes), ~0.3-0.7 ms for the sensor callback (`serialize_nipkow_stream` is an 800-iteration pure-Python loop, `capture.py:1211-1215`), ~0.5-3 ms for a Tk tick's Python part (`_paint` serialises the stream again plus `reconstruct_display_preview`, `app.py:694-697`), up to 5 ms if the other thread is itself CPU-bound in Python. Each stall:

- if it lands in the ON spin (probability ≈ the pixel's duty, 30-50 % for mid-grey at 0.7 brightness): the LED **stays on** for the stall ⇒ a bright streak of 1-40 pixels (0.1-5 ms);
- afterwards `t_next` is behind, the inter-pixel wait is skipped until it catches up (each pixel still costs `pulse_time` + ~20 µs), so the next ~2-90 pixels are emitted early and compressed.

Order of magnitude: 5-15 % of each frame's pixels displaced or smeared, in positions that differ every frame. This alone makes the picture "sparkle" and tear; it is independent of the lock.

### 4.3 Hand-off and phase (`controller.py:196-230`, `led.py:189-213`)

1. Frame start is **not** the edge time. `_on_revolution` runs ~21-27 ms after the edge (section 3.1); `output_frame` sets `_frame_ready`; the worker starts the 800-pixel loop at `t_next = time.perf_counter()` (`led.py:228`) when it gets there. If it is still inside the previous loop (which lasts the previous `period` plus stall overshoot), the new frame starts when that loop ends, i.e. at `Start_{k−1} + P_{k−2}`, carrying the previous period-measurement error into the phase. If two callbacks arrive before the worker consumes the event, `_pending_stream` is overwritten and a frame is **skipped** (`led.py:202-204`).
2. The pixel clock is `period / 800` with `period` = the **previous** measured revolution (`led.py:225`), which contains `L_k − L_{k−1}` (±4.5-10 ms in state A). A 5 % period error moves pixel 799 by 5 % of a revolution = 40 px = 1.6 columns; a 1 ms error moves it 8 px. The right-hand side of the picture breathes by a column or more frame to frame.
3. There is **no phase correction** inside the frame and no use of the kernel timestamp. A frame that starts 2.5 ms late is simply 20 px late everywhere.
4. The stall interlock inside `output_frame` (`led.py:193-196`, 350 ms) is fed on the line before it is checked (`controller.py:214-215`), so it can never trip; `feed_watchdog` also clears `safety_tripped`. The real interlocks are the supervisor's 0.5 s check (`controller.py:252-255`) and the worker's 0.2 s `Event.wait` timeout (`led.py:208-211`).

### 4.4 Defects

| ID | file:line | Mechanism | Symptom | Severity | C++ port must |
| --- | --- | --- | --- | --- | --- |
| L-1 | `led.py:228-249` | Software per-pixel waveform in a GIL-holding thread; stalls freeze the pin | streaks, compressed runs, 5-15 % of pixels wrong every frame | **Critical** | Generate the 800-pixel waveform in hardware: PWM serialiser via DMA (PWM0 ch1 on GPIO 13/19, shares `clk_pwm` with the STEP channel), or SPI0 MOSI via DMA with `core_freq` pinned, or an RP2040/PIO; software only fills the next frame buffer |
| L-2 | `led.py:225, 228` | Pixel clock from the noisy previous period; frame start at "whenever" | column breathing, phase jumps of tens of px | **Critical** | Pixel clock from a hardware clock; frame start aligned to the edge timestamp (or pixel clock = STEP clock / 4, section 5.4) |
| L-3 | `led.py:206-213`, `controller.py:217-224` | New frame only after the old loop ends; event overwritten | accumulated phase error, skipped frames | High | A new edge aborts/retimes the running frame (ring buffer indexed by phase) |
| L-4 | `led.py:238-243` + shim cost | ~10 µs minimum ON, +10 µs bias | posterised blacks, lifted floor | High | Sub-µs resolution (serialiser at ≥ 8 MHz gives ≤ 0.125 µs) |
| L-5 | `led.py:189-196`, `controller.py:214-215` | Interlock fed before check | no protection against a stalled disk except 0.5 s supervisor | Medium (safety) | Hardware-side limiter (DMA stops ⇒ pin low; or a one-shot that caps ON time) |
| L-6 | `led.py:139`, all workers `daemon=True` | Daemon threads killed at interpreter exit, possibly with the pin HIGH | LED stuck on at ~2 A (see 8.4) | Medium (safety) | Explicit shutdown ordering; hardware default-low |

---

## 5. Motor control (`motor.py:356-473, 716-755`, `controller.py:201-209`)

### 5.1 What the hardware does

STEP comes from PWM0 channel 0 at 50 % duty; period written as integer ns (`motor.py:430-438`), quantised by the kernel to 20 ns (P5, P6). At 10 rev/s × 3200 µsteps = 32 000 Hz: 31 250 ns = 1562.5 cycles → `DIV_ROUND_CLOSEST` gives 1562 (31 240 ns, 32 010.2 Hz, disk at 10.0032 rev/s, 99.968 ms) or 1563 (31 260 ns, 9.9968 rev/s). One cycle = 20 ns per step = **64 µs per revolution (0.064 %)**. The rotor follows this clock exactly unless it slips; a hardware-PWM stepper has no revolution-to-revolution speed noise of its own. (With upstream's 10 MHz the quantum would be 100 ns/step = 320 µs/rev = 0.32 %, still inside the 3 % band.)

### 5.2 The P-only trim (ESTIMATE of loop behaviour; code is FACT)

`controller.py:203-209`: `trim_us = 0.05 · (T_target − P_meas) · 1e6`, clamped ±5000 µs, only when `current_fps ≥ 0.8·target`. `motor.py:728-731`: `rev_period = 1/fps + trim_us·1e-6`, `rate = spr/rev_period`, slew-limited to `2·ramp·spr·dt` = 2·2·3200·0.02 = 256 steps/s per 20 ms tick (12 800 steps/s², 4 rev/s², i.e. far above anything the trim asks for).

Plant model: `P_cmd(n+1) = T + k·(T − P_meas(n))`, `P_meas(n) = P_cmd(n) + ε_n` where ε is the timestamp noise. Deviation `d(n+1) = −k·d(n) − k·ε_n` with k = 0.05: it converges in one or two revolutions and otherwise just injects 5 % of the timestamp noise into the real speed (ε = 5 ms ⇒ 250 µs/rev ⇒ the disk really does change period by 0.25 % for one revolution, which the sensor then sees as true noise). Most trims (50 µs per 1 ms of error) are **below the 64 µs/rev quantum**, so the written period rounds back to the same cycle count: the loop mostly does nothing, occasionally toggles ±1 cycle. It never oscillates (|−k| ≪ 1) and never converges to anything the stepper did not already do. Verdict: harmless, useless, and a source of 0.25 % speed kicks whenever the timestamps are bad. A DC motor would need a real PI(D) speed loop; a stepper needs **no frequency loop at all** and a **phase** relationship only if the pixel clock is independent of the step clock (5.4).

### 5.3 Start-up: instant 0.5 rev/s, and the physics (ESTIMATE)

`_pwm_loop` (`motor.py:722-735`) enables the driver and starts the PWM at `desired_rate(0.5)` = 1600 µsteps/s **in the same instant** (`set_rate` writes ENABLE low and `enable=1`, `motor.py:439-441`); the ramp (2 FPS/s) only acts above that. The rotor must therefore pull in from rest to 0.5 rev/s with no ramp.

- Disk inertia: uniform-disc approximation, I = ½·m·R² = 0.5 · 0.080 kg · (0.125 m)² = **6.25×10⁻⁴ kg·m²** (2.4 mm version: 8.6×10⁻⁴; bitluni's 0.6 mm, ≈27 g: 2.1×10⁻⁴). Motor rotor inertia (~4×10⁻⁶) is negligible.
- Pull-in: the electrical angle advances at ω = 3.14 rad/s from t = 0; the rotor accelerates at α = T_eff/I; the lag peaks at t = ω/α with θ_lag = ω²/(2α). Slip occurs when the lag exceeds one full step (1.8° = 0.0314 rad) where a 2-phase stepper's torque peaks. Taking T_hold = 0.17 N·m (P15) and an average available torque over the lag range of ≈ 0.64·T_hold ≈ 0.11 N·m: α = 176 rad/s², θ_lag = 9.87/(352) = 0.028 rad = **1.6°** — within 10 % of the 1.8° limit. With the 27 g disk: α = 524 rad/s², θ_lag = 0.0094 rad = 0.54° (comfortable). So the reprint moved the instant start from "fine" to "marginal", and **any** shortfall (Vref below 1 A, chopper not reaching set current, extra friction such as the rim rubbing the sensor slot, resonance) tips it into a stall. The ramp itself is irrelevant: 2 rev/s² = 12.6 rad/s² needs only I·α = 7.9 mN·m (5 % of holding torque).
- Microstep wiring **unknown**: A4988 MS pins have internal pull-downs (P14); if they are not all tied HIGH the driver is in full step and 1600 steps/s commands **8 rev/s = 480 RPM from standstill** — an instant, guaranteed stall, and at target the command would be 160 rev/s. Bench check 6 settles it in a minute.
- Current limit: "pot all the way to the right" is not a current value. With a Pololu 0.068 Ω board, 1 A ↔ 0.54 V; 0.05 Ω ↔ 0.40 V; 0.1 Ω clones ↔ 0.80 V (P14). If the pot's end stop gives e.g. 1.5-2 V, the set current is 2.5-5 A: the A4988 (abs max 2 A, ~1 A without a heatsink) hits **thermal shutdown (165 °C)** within seconds and resets its translator to Home — which looks exactly like "the motor stalls shortly after starting", and the 1 A motor overheats. If the pot direction is reversed on this board, "all the way right" may be minimum current. Bench checks 5 and 8.
- Supply voltage at speed (secondary): with Kt ≈ T_hold/(√2·I) ≈ 0.12 N·m/A, back-EMF at 62.8 rad/s ≈ 7.5 V/phase; the 500 Hz electrical frequency at 600 RPM needs ω·L·I ≈ 2π·500·4 mH·1 A ≈ 12.6 V just for the inductance; √((7.5+3.6)² + 12.6²) ≈ 17 V > 12 V, so at 9-12 V the winding current cannot reach 1 A at 600 RPM and torque is a fraction of holding torque. Windage/friction of a 250 mm disk at 10 rev/s is small (tens of mN·m), so this is usually survivable, but it leaves no margin at mid-range resonance (≈1-3 rev/s with a large flywheel, HYPOTHESIS). The motor listing says 12-24 V; the A4988 accepts 35 V; 24 V is the easy fix. (The 12 V ceiling in `docs/HARDWARE.md` applies to the TB6612 HAT, as the doc itself says.)

### 5.4 What a stepper on hardware PWM actually needs from the software

Nothing for frequency. For the picture: the pixel clock should be **derived from the step clock** — 3200 µsteps / 800 px = exactly **4 microsteps per pixel**. If both the STEP waveform and the LED serialiser run from `clk_pwm` (P7: both PWM channels are clocked by the same clk_pwm; channel 0 in M/S mode with range R gives STEP, channel 1 in serialiser mode streams the pixel bits from the FIFO via DMA), the picture is rigidly locked to the rotor's electrical angle; the only free variable is the load angle (constant at constant speed, absorbed by the phase-offset calibration). The index edge is then needed only once per start to find the notch (phase offset) and continuously as a stall/slip detector. Residual jitter is the stepper's own torque ripple (sub-pixel at 1/16 microstepping). This design needs no PLL and survives any CPU load, because software only has to write the next 800-pixel frame into a DMA ring before the DMA reaches it (100 ms deadline, trivially met).

### 5.5 Defects

| ID | file:line | Mechanism | Symptom | Severity | C++ port must |
| --- | --- | --- | --- | --- | --- |
| M-1 | `motor.py:722-735` | Instant start at 1600 µsteps/s | start-up stall with the heavier disk (marginal pull-in) | **High** | Start the STEP clock at ≤ 0.05 rev/s (≤ 160 Hz) and ramp linearly/S-curve from there; enable the driver ≥ 1 ms before the first step (A4988 charge-pump note) |
| M-2 | wiring (unknown) | MS1-3 floating ⇒ full step ⇒ 8 rev/s command | certain stall | **High** (if true) | Read back/assert the mode in setup docs; expose µsteps as a verified setting |
| M-3 | hardware setting | Vref unknown; pot end-stop may exceed 2 A or be minimum | thermal shutdown / no torque | **High** (if true) | Document Vref target per sense resistor; add a heatsink/fan rule |
| M-4 | `controller.py:203-209`, `motor.py:728-731` | P trim on a synchronous plant, below the quantum | nothing useful; 0.25 % speed kicks from timestamp noise | Low | Delete; use slip detection (edge period vs commanded) instead |
| M-5 | `motor.py:736-753` | 100 sysfs open/write/close per second rewriting unchanged period/duty | GIL hand-offs that stall the LED worker (section 4.2) | Low | Write only on change; or program PWM registers directly |
| M-6 | `motor.py:455-459` | `release()` de-energises coils; 80 g disk coasts unbraked for many seconds with the sensor still pulsing | UX/safety | Low | Ramp down under control, then hold briefly, then release |

---

## 6. `controller.py`: supervisor, watchdogs, races

- State machine (`controller.py:232-255`): every 100 ms `state` = `locked` if `motor.current_fps ≥ 0.95·target and sensor.sync_locked`, else `ramping`/`running`. The pill mirrors the raw lock flag with no hysteresis, so the flapping in section 3 is displayed verbatim.
- Revolution watchdog: `perf_counter() − _last_rev_time > 0.5 s and frame_counter > 0` ⇒ `led.blank()` and `locked → running`. This is the only effective stall interlock (the LED's 350 ms one is dead, section 4.3). OK for a stopped disk; it does nothing about a *slipping* motor.
- Locks: the sensor releases `sensor._lock` before invoking the callback (`sensor.py:202-205`), the supervisor and `get_status()` hold the controller RLock while calling into sensor/motor/LED/pipeline telemetry (`controller.py:236-249, 259-282`); `_drive_frame` takes the controller lock only *after* `output_frame` (`controller.py:227-230`). No inversion found; but the Tk thread's `get_status()` (10 Hz) can delay the callback's tail by the telemetry time (~1 ms) — harmless.
- `stop()` (`controller.py:91-107`): sets `_running=False` and blanks under the lock, then stops the motor, then the sensor. Between those, a last edge can still call `_on_revolution`... `_running` is checked first (198-199), so no frame is lit after stop. Fine.
- `set_free_run` / `_free_run_loop` (`controller.py:121-162`): correct joins, but the free-run clock has the same GIL latency problem for its frame starts (it calls `_drive_frame` from a Python thread, `stop.wait(delay)` wake-up needs the GIL).
- The supervisor's `time.sleep(0.1)` and the motor loop's `Event.wait(0.02)` are hrtimer-accurate (P4), but each wake-up is a GIL request that stalls the LED worker (section 4.2).
- `_on_revolution` runs `serialize_nipkow_stream` (pure-Python 800-iteration loop, ~0.3-0.7 ms, ESTIMATE) **in the callback thread** while holding the GIL, delaying both the LED hand-off and everything else.

| ID | file:line | Mechanism | Symptom | Severity | C++ port must |
| --- | --- | --- | --- | --- | --- |
| C-1 | `controller.py:243-249` | Raw lock flag → state → pill, no hysteresis | flapping display | Medium | Hysteresis + filtered criterion |
| C-2 | `controller.py:214-215` | Interlock fed before check | dead watchdog | Medium | See L-5 |
| C-3 | `controller.py:219-224` | Heavy serialisation inside the edge callback | adds latency on the critical path | Medium | Pre-serialise the next frame off the critical path; the edge handler only swaps a pointer |
| C-4 | `controller.py:252-255` | Stall detection only by absence of edges for 0.5 s | a slipping stepper (edges still arrive) is never detected | Medium | Compare measured period against the commanded one (slip ⇒ blank + re-ramp) |

---

## 7. The capture thread (`capture.py:756-788`, `v4l2.py:337-368`)

- 30 `select`/`DQBUF`/`QBUF` cycles per second (all GIL-releasing syscalls, cheap) and 15 conversions per second. Each conversion reads 250 rows × 3840 B = 960 KB through numpy fancy indexing from a **Normal-NC (non-cacheable)** mapping (P13): the repo measured ≈ 8 ms per conversion (≈ 120 MB/s), i.e. ≈ 12 % of one core. NumPy releases the GIL inside take/ufunc loops (P12), so the GIL cost is the Python glue between the ~15 array operations (≈ 0.2-0.5 ms per conversion, ESTIMATE) — but every return to Python is a GIL request, so each conversion causes several forced hand-offs from the LED worker (section 4.2). Memory-bus contention with the LED spin is nil (the spin is not memory-bound). Net: a minor contributor to lock flapping (it changes who wins the GIL queue) and a steady source of LED stalls (~15-45 per second).
- Nothing in the capture path affects the motor (hardware PWM) — correct claim in `motor.py:359-360`.
- Design note for the port: keep the capture/convert on its own core (e.g. `taskset`/`pthread_setaffinity_np`), or let the ISP/GPU scale (the 1080p UYVY → 32×25 reduction is a classic use of the V4L2 ISP on the Pi 4, HYPOTHESIS as to availability on the unicam path).

---

## 8. Other findings

1. **Sleep granularity**: `time.sleep`/`Event.wait` use hrtimers (P4), ~50-100 µs accuracy; not a problem by itself. The problem is always the GIL re-acquisition after the sleep.
2. **Daemon threads** everywhere (sensor, LED, motor, capture, pattern, supervisor, Tk job loop): on interpreter exit they are killed wherever they are; the LED pin can be left HIGH (see 4 below). `close()` ordering (`controller.py:290-294`) is correct when it runs.
3. **Exception swallowing hides hardware failures**: `sensor.py:226-227, 236-237, 256-257, 272-273`; `led.py:152-153, 164-165`; `capture.py:876-878` (recovers but logs); the hardware "doctor" cannot tell the operator why the sensor is synthetic. `logs.py` captures stderr, so the shim's "Bug compatibility" `print(exc, file=sys.stderr)` for callback exceptions *does* reach the log — the only visible failure path.
4. **LED current if the pin sticks HIGH** (HYPOTHESIS, hardware): `docs/HARDWARE.md:96` suggests a 2.7-3.3 Ω 5 W resistor in series with a "3 W" LED on a 9-12 V rail. With Vf ≈ 3.3 V: I ≈ (12 − 3.3)/3 ≈ 2.9 A (9 V: ≈ 1.9 A), far above a 3 W LED's ~700 mA. Pulsed at ≤ 70 % duty that is already marginal; DC (stuck pin) is destructive. The port needs a hardware pulse-width/duty limiter or a constant-current driver (the doc's alternative).
5. **bouncetime at 24 FPS**: 41.7 ms period > 20 ms window ⇒ still reported (lgpio rule, P9); both the 20 ms window and `period < 0.02` guard fail only above 50 rev/s. Fine for the 1-24 FPS range, but the 20 ms report delay stands (S-3).
6. **HUD text** `LOCKED (±0.4%)` (`app.py:763`) does not match the 3 % criterion in code.
7. **SPILEDModulator** (`led.py:277-434`, unused by default): writes the whole revolution as ≤ 4096-byte chunks with an ioctl gap between chunks (each gap is a timing error), starts at "whenever" like the GPIO path, and on a Pi 4 the SPI clock follows `core_freq` unless pinned (`core_freq=500`, `core_freq_min=500`, rpi_ws281x README [S34]). Not a drop-in fix.
8. **Service unit** (`packaging/mechanical-tv.service`): `Nice=-5`, `CPUQuota=150%`; the local console (the user's mode) runs at default priority with no CPU isolation and no `SCHED_FIFO`; `MemoryMax`/`TasksMax` irrelevant to timing.
9. **MockOptoSensor lock threshold** uses ≥ 3 periods vs 5 in the GPIO class (`sensor.py:90` vs `192`) — cosmetic.
10. The `A4988PWMDriver` keeps a second `lgpio` chip handle while the shim keeps its own; both open `/dev/gpiochip0` and claim different lines (4, 21 vs 23, 24) — no conflict (checked against the shim's `setup()`/`_set_alert` code, P8).

---

## 9. Consolidated defect ranking (contribution to the two symptoms)

| Rank | ID | Symptom affected | Severity | One-line fix direction |
| --- | --- | --- | --- | --- |
| 1 | S-1 + GIL (section 3) | lock flap, picture phase | Critical | hardware/kernel timestamps; no Python on the edge path |
| 2 | L-2 / L-3 | picture breathing, jumps, skips | Critical | frame slaved to edge with hardware pixel clock; or pixel clock = STEP/4 |
| 3 | L-1 / L-4 | smears, streaks, posterised blacks | Critical | DMA-fed serialiser/SPI or external PIO |
| 4 | S-2 / S-4 | both (if the synthetic path is active) | Critical-if-active | fail loudly; libgpiod 2 |
| 5 | M-1 / M-2 / M-3 | start-up stall | High | ramp from zero; verify MS pins and Vref; 24 V |
| 6 | C-1 / S-5 | displayed flapping | Medium | filtered lock with hysteresis |
| 7 | C-3 / M-5 / capture hand-offs | LED stalls | Medium | move work off the critical path; fewer GIL requests (moot in C++) |
| 8 | L-5 / L-6 / 8.4 | safety | Medium | hardware limiter |
| 9 | M-4 / S-3 / S-6 / 8.6 | cosmetic/efficiency | Low | delete or fix |

---

## 10. What would persist in a C++ port that only changes the language

| Design element kept | Residual problem in C++ | Why |
| --- | --- | --- |
| Timestamp in a userspace callback (even a C++ thread reading the cdev fd) | 50-500 µs jitter normally, ms spikes under desktop/capture load without `SCHED_FIFO`+`mlockall`; lock mostly holds (2 ms) but the picture phase still wobbles ±0.5-4 px | the Linux scheduler, not the GIL, is now the noise source; hard-IRQ timestamps (P10) cost nothing and remove it |
| `bouncetime`-style debounce through lgpio | fixed 20 ms report delay + 1 ms slop | P9 |
| Per-pixel busy-wait on a general-purpose core | preemption by CFS/IRQ/softirq for 0.1-several ms ⇒ the same streaks and compressed runs, less often; with an isolated core and SCHED_FIFO ~20-50 µs (ESTIMATE) | no hardware timing, pin state owned by software |
| Frame = previous measured period, started when the worker is ready, previous frame finished first | identical column breathing per 1 % period error, identical phase accumulation and frame skips | language-independent |
| P trim on a stepper | identical no-op | plant is synchronous |
| Instant 0.5 rev/s start | identical stall | physics |
| Software-only interlock, pin default state owned by software | identical stuck-on risk | hardware |
| Synthetic-pulse fallback | identical rolling picture | design |

What the port must do instead (the design contract for the spec workstream):

1. **Pixel waveform in hardware.** Preferred: BCM2711 PWM0 channel 1 (GPIO 13 or 19) in serialiser mode fed by DMA from a ring buffer, with channel 0 (GPIO 18) producing STEP in M/S mode from the same `clk_pwm` (P7, P5) — this is the rpi_ws281x mechanism, proven on the Pi 4 [S34]; it needs register-level access (`/dev/mem` or a small kernel module) because the sysfs PWM driver does not expose the serialiser (P6). At `clk_pwm` = 8 MHz: 1000 bits per 125 µs pixel slot (10-bit PWM resolution, 0.125 µs), 100 KB per revolution, 1 MB/s of DMA. Alternative: SPI0 DMA (`spidev` with `bufsiz` raised, `core_freq` pinned). Alternative with the least Pi-side risk: an RP2040 (PIO) generating both STEP and the LED stream from one clock, receiving frames over SPI/USB.
2. **Pixel clock = STEP clock / 4** (3200 µsteps : 800 px). The index edge then only sets the initial phase (which DMA position corresponds to the notch) and monitors slip. If the port keeps an independent pixel clock, the edge timestamp (hard-IRQ `timestamp_ns`) must retime the running frame, not just the next one.
3. **Edge handling**: libgpiod 2 / raw cdev with no debounce; software guard at 50 % of the expected period; period compared against the **commanded** period to detect slip (both are known exactly).
4. **Motor**: start at ≤ 160 Hz STEP, ramp to 32 kHz over ≥ 5-10 s with an S-curve; enable the driver ≥ 1 ms before stepping; ramp down before release; expose microstep mode and Vref guidance in the install docs.
5. **Lock**: hysteresis (e.g. 10 good revolutions to lock, 3 bad to unlock), computed from hardware timestamps; display the actual criterion.
6. **Process layout**: realtime thread (`SCHED_FIFO`, `mlockall`, pinned to an isolated core via `isolcpus`/`cset`) only touches the DMA ring and the edge fd; capture, conversion, UI on the other cores; no shared mutex between them (lock-free frame swap).
7. **Safety**: DMA under-run or stop ⇒ serialiser emits zeros (pin low); optional hardware one-shot limiter on the LED gate; constant-current LED driver instead of resistor + L298N.

---

## 11. Bench checks (confirm or rule out each hypothesis on the Pi)

1. **Which sensor backend is live** (section 2.1): read the HUD cells "INDEX SENSOR" and "LED OUTPUT" while running. Expected texts: `RPi.GPIO · GPIO 4 · N pulses` (state A), `mock-fallback · …` (B), `gpiod · GPIO 4 · 0 pulses` (C), `MockOptoSensor · …` (D). Also run on the Pi:
   `dpkg -l | grep -E "rpi-lgpio|rpi.gpio|python3-lgpio|libgpiod|python3-numpy"`; `python3 -c "import RPi.GPIO as g, inspect; print(g.__file__)"` (a path containing `rpi_lgpio`/`RPi/GPIO/__init__.py` from the shim vs `RPi/_GPIO*.so` for the original); `python3 -c "import gpiod; print(gpiod.__version__)"`; `uname -r; cat /etc/os-release | head -3; v4l2-ctl --version`.
2. **Who owns the lines** while the app runs: `gpioinfo gpiochip0 | grep -E "line +(4|18|21|23|24):"` — consumers should be the shim ("rpi-lgpio"/lgpio label) on 4 and 21 and lgpio on 23/24; `[used]` by something else means the shim raised and the app silently fell back.
3. **Index pulse width** (section 2.2) with the disk spinning and the app stopped: `python3 - <<'EOF'` → `import lgpio,time; h=lgpio.gpiochip_open(0); lgpio.gpio_claim_alert(h,4,lgpio.BOTH_EDGES,lgpio.SET_PULL_DOWN); last=[None]` → callback printing `(tick-last)/1e3` µs for level 0 events (pulse width) — expect ~200-300 µs at 10 rev/s. (Or a scope on GPIO 4.)
4. **Kernel vs Python timestamp jitter** (section 3), disk spinning, app stopped:
   ```python
   import lgpio, time, statistics, threading
   h = lgpio.gpiochip_open(0)
   lgpio.gpio_claim_alert(h, 4, lgpio.RISING_EDGE, lgpio.SET_PULL_DOWN)
   last = [None, None]; pk = []; pp = []
   def cb(chip, gpio, level, tick):
       now = time.perf_counter_ns()
       if last[0]: pk.append((tick-last[0])/1e6); pp.append((now-last[1])/1e6)
       last[0], last[1] = tick, now
   c = lgpio.callback(h, 4, lgpio.RISING_EDGE, cb)
   time.sleep(10)
   print("kernel  period ms: mean %.3f  p-p %.3f  sd %.3f" % (statistics.mean(pk), max(pk)-min(pk), statistics.pstdev(pk)))
   print("python  period ms: mean %.3f  p-p %.3f  sd %.3f" % (statistics.mean(pp), max(pp)-min(pp), statistics.pstdev(pp)))
   threading.Thread(target=lambda: [None for _ in iter(int, 1)], daemon=True).start()   # a pure-Python GIL hog like the LED worker
   pk.clear(); pp.clear(); time.sleep(10)
   print("with GIL hog -> kernel sd %.3f ms, python sd %.3f ms" % (statistics.pstdev(pk), statistics.pstdev(pp)))
   ```
   Expected: kernel sd ≲ 0.05 ms in both cases; Python sd ~0.3 ms without the hog and **~2-5 ms with it** (lock threshold is 2 ms). To spin the disk without the app, drive the PWM by hand (step 6).
5. **Vref / current limit** (M-3): with logic power on, multimeter DC from the A4988 pot wiper (or VREF via) to GND. Identify the sense resistors (R068 / R050 / R100 marking). Target for 1.0 A: 0.54 V (0.068 Ω), 0.40 V (0.050 Ω), 0.80 V (0.100 Ω). Note whether clockwise raises or lowers it. Anything above ~1.1-1.4 V on a 0.05/0.068 Ω board is > 2 A (abs max) and will trip TSD without a heatsink.
6. **Microstep wiring and the instant-start test** (M-1, M-2), app stopped, motor powered, the user in the `gpio` group (or use `sudo`):
   ```sh
   echo 0 | sudo tee /sys/class/pwm/pwmchip0/export 2>/dev/null; sleep 0.5
   pinctrl set 23 op dl; pinctrl set 24 op dl          # DIR, ENABLE low = driver on (use raspi-gpio on older images)
   echo 6250000 | sudo tee /sys/class/pwm/pwmchip0/pwm0/period      # 160 Hz = 0.05 rev/s at 1/16
   echo 3125000 | sudo tee /sys/class/pwm/pwmchip0/pwm0/duty_cycle
   echo 1       | sudo tee /sys/class/pwm/pwmchip0/pwm0/enable
   # expect one revolution per 20 s. If the disk turns ~16x faster (one rev per 1.25 s) the driver is in FULL step (MS pins low).
   echo 312500 | sudo tee /sys/class/pwm/pwmchip0/pwm0/duty_cycle; echo 625000 | sudo tee /sys/class/pwm/pwmchip0/pwm0/period   # jump to 1600 Hz (= the app's instant start)
   # does it pull in from rest? Then lower the period by ~5 % every 2 s down to 31250 (32 kHz) and note the RPM at which it stalls.
   echo 0 | sudo tee /sys/class/pwm/pwmchip0/pwm0/enable; pinctrl set 24 op dh
   ```
   Also measure MS1/MS2/MS3 against GND with a meter: all ≈ logic HIGH (3.3/5 V) for 1/16; ~0 V = floating/full step.
7. **PWM clock and quantum** (P5/P6): `sudo cat /sys/kernel/debug/clk/clk_summary | grep -i pwm` → expect 50000000. Writing 31250 then reading back `period` returns the requested value (the kernel rounds internally), so only `clk_summary` shows the quantum.
8. **Thermal shutdown vs mechanical stall** (M-3): after a "stall", touch/IR the A4988 (TSD at 165 °C junction; the package will be too hot to touch). Cut STEP, wait 60 s, retry: an immediate restart after cooling indicates TSD, not a torque problem. Fit a heatsink + airflow before any current above ~1 A.
9. **Motor supply**: measure VMOT at the driver terminals under load (≥ 8 V minimum; 24 V recommended for 600 RPM, motor listing 12-24 V). Confirm the LED rail is separate or sized for both.
10. **Lock flapping correlates with GIL load** (section 3): in the app, with the picture on, note OPTO JITTER; then switch the source to a static pattern (removes the capture thread's 15 conversions/s) and hide the debug window: if the jitter envelope shrinks, GIL contention is confirmed; it cannot vanish while the LED worker runs.
11. **Rim clearance**: with the motor off, rotate the disk by hand through the sensor slot; any rub adds friction at exactly the start-up phase where margin is thinnest (section 5.3).

---

## 12. Sources

- [S1] Raspberry Pi OS release notes: https://downloads.raspberrypi.com/raspios_arm64/release_notes.txt (2025-10-01 "Based on Debian Trixie release", kernel 6.12.47; 6.12.75; 6.18.x in 2026).
- [S2] Debian trixie v4l-utils 1.30.1-1: https://packages.debian.org/trixie/v4l-utils
- [S3] Debian trixie python3-libgpiod 2.2.1-2+deb13u1: https://packages.debian.org/trixie/python3-libgpiod
- [S4] Debian trixie python3-rpi.gpio 0.7.1~a4-1.2: https://packages.debian.org/trixie/python3-rpi.gpio
- [S5] raspberrypi/linux issue #6037 "Failed to add edge detection" on 6.6, GPIO base 512, rpi-lgpio recommendation: https://github.com/raspberrypi/linux/issues/6037
- [S6] rpi-lgpio on PyPI (0.6): https://pypi.org/project/rpi-lgpio/
- [S7] bcm2711_defconfig (rpi-6.12.y): https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm64/configs/bcm2711_defconfig
- [S8] kernel/Kconfig.hz (v6.12), `default HZ_250`: https://github.com/torvalds/linux/blob/v6.12/kernel/Kconfig.hz
- [S9] bcm283x.dtsi (rpi-6.12.y) pwm@7e20c000 `assigned-clock-rates = <50000000>`: https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm283x.dtsi
- [S10] bcm2711.dtsi (rpi-6.12.y) includes bcm283x.dtsi; pwm1 50 MHz; no `dma-coherent`: https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm2711.dtsi
- [S11] pwm-overlay.dts: https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/overlays/pwm-overlay.dts
- [S12] overlays README, `pwm` entry: https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/overlays/README
- [S13] BCM2835 ARM Peripherals, ch. 9 PWM (serialiser, M/S, FIFO, DMA channel 5, "clk_pwm nominally 100 MHz"): https://datasheets.raspberrypi.com/bcm2835/bcm2835-peripherals.pdf
- [S14] pwm-bcm2835.c (rpi-6.12.y): https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/pwm/pwm-bcm2835.c
- [S15] rpi-lgpio 0.6 source, `RPi/GPIO/__init__.py` (sdist from PyPI; `_Alert`, `_set_alert`, `setup`, `output`, `add_event_detect`).
- [S16] lgpio 0.2.2.0 source (sdist from PyPI; `src/lgPthAlerts.c`, `src/lgGpio.c`, `lgpio_extra.py`); GitHub mirror https://github.com/joan2937/lg
- [S17] lgpio Python documentation (callback, gpio_set_debounce_micros): https://abyz.me.uk/lg/py_lgpio.html
- [S18] gpiolib-cdev.c v6.12 (`edge_irq_handler`, `debounce_work_func`, `line_event_timestamp`): https://github.com/torvalds/linux/blob/v6.12/drivers/gpio/gpiolib-cdev.c
- [S19] CPython 3.13 `Python/ceval_gil.c` (`DEFAULT_INTERVAL 5000`, gil_drop_request, FORCE_SWITCHING): https://github.com/python/cpython/blob/3.13/Python/ceval_gil.c
- [S20] `sys.setswitchinterval`: https://docs.python.org/3/library/sys.html#sys.setswitchinterval
- [S21] CPython `Modules/_tkinter.c` (`ENTER_TCL`): https://github.com/python/cpython/blob/3.13/Modules/_tkinter.c
- [S22] NumPy thread safety: https://numpy.org/doc/stable/reference/thread_safety.html
- [S23] videobuf2-dma-contig.c v6.12 (`vb2_dc_mmap` → `dma_mmap_attrs`): https://github.com/torvalds/linux/blob/v6.12/drivers/media/common/videobuf2/videobuf2-dma-contig.c
- [S24] kernel/dma/mapping.c v6.12 (`dma_pgprot`): https://github.com/torvalds/linux/blob/v6.12/kernel/dma/mapping.c
- [S25] arm64 pgtable.h v6.12 (`pgprot_dmacoherent` = MT_NORMAL_NC): https://github.com/torvalds/linux/blob/v6.12/arch/arm64/include/asm/pgtable.h
- [S26] Allegro A4988 datasheet: https://www.allegromicro.com/-/media/files/datasheets/a4988-datasheet.pdf
- [S27] Pololu A4988 carrier (#1182) notes on Vref, sense resistors, cooling, MS pull-downs: https://www.pololu.com/product/1182
- [S28] STEPPERONLINE 17HE08-1004S listing (17 N·cm, 1.0 A, 3.6 Ω, 12-24 V): https://www.ebay.com/p/7062407561
- [S29] 17HS08-1004S specifications (0.16 N·m, 1 A, 3.7 Ω, 4.5 mH): https://www.maritex.eu/en/product/omc-17hs08-1004s-stepper-motors
- [S30] i-programmer, Pi IoT in Python using GPIO Zero (lgpio toggle ~24 kHz on a Pi 4): https://i-programmer.info/programming/148-hardware/16985-pi-iot-in-python-using-gpio-zero-pins.html?start=2
- [S31] Barkhausen Institut, PWM performance on a Pi 4 (RPi.GPIO software PWM ≈ ±50 µs jitter): https://www.barkhauseninstitut.org/research/lab-1/our-blog/pwm-performance
- [S32] codeandlife, Benchmarking Raspberry Pi GPIO speed: https://codeandlife.com/2012/07/03/benchmarking-raspberry-pi-gpio-speed
- [S33] rpi-lgpio "Differences" (debounce semantics, GPIO reservation): https://rpi-lgpio.readthedocs.io/en/latest/differences.html ; API: https://rpi-lgpio.readthedocs.io/en/latest/api.html
- [S34] rpi_ws281x README (PWM/PCM/SPI pins, Pi 4 `core_freq` pinning, DMA channel caveats): https://github.com/jgarff/rpi_ws281x
- Repository files cited as `path:line` above; project measurement of 8 ms per converted frame: `docs/ARCHITECTURE.md:153`.
