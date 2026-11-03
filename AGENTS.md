# Agent guide

Instructions for AI agents and contributors working on Mechanical TV. Read this first. Read
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) before touching `src/mtv/hardware/`.

## What this is

A C++20 Qt desktop application for a Raspberry Pi 4 that drives a Nipkow-disk mechanical television.
It takes HDMI in through a TC358743 CSI bridge, drives a stepper through an A4988 on hardware PWM,
reads an optical index sensor, and pulses a 3 W LED 800 times per revolution. There is **no web
interface**: the owner removed it on purpose in 0.3. Do not add a server, HTTP API or browser UI.
The Python app (0.1-0.2) is gone from the tree; `git show 4c64391:mechanical_tv/<file>` shows it
if behaviour must be compared.

## Build, test, run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DMTV_WERROR=ON
cmake --build build
QT_QPA_PLATFORM=offscreen ./build/mtv_tests          # all; -tc="*name*" filters
./build/mechanical-tv --mock-hardware                # window, simulated hardware
./build/mechanical-tv --check                        # environment report, JSON
./build/mechanical-tv --mock-hardware --free-run --screenshot out.png   # screenshot for docs
```

On macOS add `-DCMAKE_PREFIX_PATH="$(brew --prefix qtbase);$(brew --prefix sqlite)"`. Linux-only
code (`MTV_LINUX`: V4L2, GPIO cdev, gpiomem, PWM, spidev, i2c) is compiled only on Linux. Check it
in a Debian Trixie arm64 container with `build-essential cmake ninja-build qt6-base-dev
libsqlite3-dev ffmpeg`; GCC 14 with `-Werror` catches things clang does not. `install.sh` must pass `bash -n` and stay
idempotent.

## Layout

`src/mtv/{common,capture,hardware,engine,console}`, `tests/test_*.cpp` (doctest; `tests/helpers.h`
has `TempDir`, `EnvGuard`, `wait_until`), `edid/`, `packaging/`, `third_party/` (vendored headers;
do not edit). The module table is in ARCHITECTURE.md.

## Invariants: do not break these

1. **The realtime thread never blocks on slow work.** `RealtimeController::rt_loop` may take only
   short mutexes (shared-pointer copies, stats). No file I/O, no logging per revolution, no
   `getenv` (read settings once in `launch()`), no source switching, no allocation-heavy work.
   Locks it shares with other threads must never be held across device I/O: `StepAccount` has its
   own lock for exactly this reason. New per-revolution work goes in `drive()` and must be
   microseconds.
2. **Timing comes from kernel edge timestamps** (`EdgeResult::t_ns`, CLOCK_MONOTONIC from the GPIO
   cdev hard IRQ). Never timestamp edges in user space, and never request kernel debounce
   (`GPIO_V2_LINE_ATTR_ID_DEBOUNCE` moves timestamping to a workqueue). Anything compared with an
   edge must be taken at the edge's time: use `MotorController::step_count_at(t_ns)`, not
   `step_count()`. The edge may be processed tens of milliseconds late.
3. **Each revolution is emitted on an absolute timeline from its own edge.** Late pixels are
   skipped, never caught up. Never derive the next frame's start from the end of the previous one.
4. **No speed loop on the stepper.** The STEP clock is hardware PWM; index pulses only measure,
   lock and detect stalls. The motor start is enable, settle, S-curve from `MTV_START_FPS`. Do not
   reintroduce a velocity jump at enable: the 1.75 mm disk cannot follow it.
5. **Brightness is applied once,** in `serialize_nipkow`. LED outputs emit `value/255` of each slot.
6. **Failures are loud.** No silent fallback to synthetic data. A missing device or hardware error
   is shown in the HUD/footer and logged. Simulation happens only with `--mock-hardware` or when
   real hardware fails at start (the reason is shown).
7. **HDMI without signal shows black,** never a stale frame or a test pattern. A failure clears the
   frame first, then records the error.
8. **The capture worker generation guard stays:** a stale worker must not register or release a
   device of a newer `start()`.
9. **Safety:** the LED is blanked 350 ms (or 1.5 revolutions) after the last pulse unless free-run
   is on. Emergency blank always leaves free-run and runs on the GUI thread at once, never queued.
   Stop releases the coils. Every exit path turns the outputs off: signals through the self-pipe in
   `main.cpp`, crashes through `failsafe::trigger()`. Register any new output that must be made safe
   with `failsafe::add_line`.
10. **Library compatibility:** the SQLite schema and folder layout (`media/<32-hex id>/original,
    frames.raw, profile.json`) stay readable by older data directories.

## Conventions

- Telemetry is `nlohmann::json` with the key names the console reads. Add keys; do not rename them
  without updating `console_window.cpp`.
- User-visible strings are plain sentences that say what happened and what to do. Keep the existing
  ones stable; tests match on them.
- Settings are `MTV_*` environment variables (also read from `/etc/mechanical-tv/environment` via
  `--env-file`). Document every new one in `docs/INSTALL.md`'s settings table.
- Qt defines `emit` as a macro, so the log function is `log::post`. Avoid other Qt macro names
  (`signals`, `slots`) as identifiers.
- Iterating `.items()` of a temporary `json` dangles (GCC catches it). Bind the json to a local
  first.
- Every new behaviour needs a doctest case. Hardware behaviour is tested through the mocks:
  `MockMotorDriver`, `MockIndexSensor` (edges derived from the motor's step count, or injected),
  `MockLedOutput`, and the HDMI fakes in `test_capture.cpp`.
- Tests that need realistic timing use `MockLedOutput(pin, /*blocking=*/true)`. The non-blocking
  mock returns instantly, which is how the late-step-count bug went unnoticed.
- Commit messages: imperative summary line; body says why. The owner asks for independent
  ("double blind") audits of code and docs before commits.

## Where things are decided

| Question | Place |
| --- | --- |
| Lock rule, debounce, jitter | `RevolutionTracker` in `src/mtv/hardware/sensor.cpp` |
| Stall and slip thresholds | Constants at the top of `src/mtv/hardware/controller.cpp` |
| Ramp shape | `SpeedProfile::advance` in `src/mtv/hardware/motor.cpp` |
| Pixel timing and bitstream | `build_led_bits`, `SpiLedOutput`, `GpioLedOutput` in `src/mtv/hardware/led.cpp` |
| Spiral mapping, gamma, brightness | `build_pixel_map`, `serialize_nipkow` in `src/mtv/capture/pipeline.cpp`, `build_gamma_lut` |
| HDMI sampling | `UYVYSampler` in `src/mtv/capture/uyvy_sampler.cpp` |
| Boot and OS settings | `install.sh`, `packaging/mechanical-tv-hw-setup` |

## Open hardware questions (not software bugs)

See [docs/DIAGNOSIS.md](docs/DIAGNOSIS.md). Before blaming code for a stall, check the A4988
microstep pins, Vref, heat, supply voltage and rim clearance in that order.
