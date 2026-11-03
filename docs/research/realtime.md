# Real-time LED / index / STEP timing for the mechanical TV C++ port (Raspberry Pi 4, Raspberry Pi OS 64-bit)

Date: 2026-11-03. Scope: evaluate options A-F from the task, recommend the realtime architecture for the C++ port, and give the parts of the stall/sync diagnosis that follow from the timing analysis. Nothing in the repo was edited.

Every statement is tagged **FACT** (verified from a primary source cited inline), **ESTIMATE** (my arithmetic, shown), or **HYPOTHESIS** (unverified, with the bench check that settles it). Repo citations are `path:line` in the repository (Python version, main at 4c64391). Kernel citations are `path:line` in `raspberrypi/linux` branch `rpi-6.12.y` unless stated; the same lines were re-checked on `rpi-6.18.y` (see 0.2).

---

## 0. Executive summary

1. **Recommended LED path: SPI0 MOSI (GPIO 10) DMA bitstream, one transfer per revolution, started on the index edge and phase-compensated with the kernel's hard-IRQ timestamp (options B + D).** Per-pixel timing is then paced by the SPI clock (crystal-derived), not by the CPU: 0.5 us edge resolution at 2 MHz (0.25 us at 4 MHz), zero scheduling jitter inside a revolution, no root, ~0.1 % of one core. The only software-timed event is one ioctl per revolution, and its latency is measured and compensated rather than avoided.
2. **Bit-banging from an isolated SCHED_FIFO core (option A) is the fallback, not the primary.** The stock Pi kernel has `CONFIG_PREEMPT=y` but no `NO_HZ_FULL`/`RCU_NOCB_CPU`, so `nohz_full=`/`rcu_nocbs=` are ignored and the 250 Hz tick keeps hitting the isolated core (one tick every 32 pixels); measured Pi 4 wake-up latencies are ~100-300 us max on stock kernels and 58-93 us max on PREEMPT_RT. Those stalls land inside individual 5-125 us pulses as bright specks. Keep it behind the same interface for bench use and for the existing GPIO 21 wiring.
3. **pigpio (option C) is not viable going forward**: needs root (`/dev/mem`), 1-5 us resolution with 10-25 % CPU, its BCM2711 DMA-channel defaults (7/6) sit inside the kernel's own channel mask (`0x07f5`), it is absent from the Trixie repository, and it does not support the Pi 5.
4. **Index pulse: Linux GPIO cdev v2 edge events, no DEBOUNCE attribute.** The timestamp is taken in the hard-IRQ handler with `ktime_get_ns()` (CLOCK_MONOTONIC); the BCM2835/2711 GPIO interrupt is a chained hard IRQ, so the timestamp is accurate to IRQ-entry latency (a few us) even though user space wakes 50-300 us later. The software debounce path moves timestamping into a workqueue (don't use it; the Python app's `bouncetime=20` does exactly that via rpi-lgpio).
5. **STEP: keep the hardware PWM.** Its clock is 50 MHz by default on this kernel (not 10 MHz as assumed): the 31,250 ns period rounds to 1563 cycles = 31,260 ns, i.e. the motor runs 0.032 % slow (9.9968 FPS); `dtoverlay=pwm,pin=18,func=2,clock=100000000` should make it exact (to verify). Because the LED stream follows the *measured* period, the motor needs no tight PLL at all.
6. **Config for the port**: `core_freq_fixed=1` (the SPI clock is derived from the core clock, which otherwise scales 200-500 MHz), `spidev.bufsiz=65536`, `isolcpus=3 irqaffinity=0-2` (drop `nohz_full`/`rcu_nocbs` unless an RT/NO_HZ_FULL kernel is installed), `performance` governor, `rtprio`/`memlock` in `/etc/security/limits.d`, `mlockall()`. An official PREEMPT_RT kernel exists (`linux-image-rpi-v8-rt`) and is optional.
7. **Diagnosis hooks that fall out of this analysis**: (a) the Python "locks for a second then unlocks" pattern has a sufficient software cause (edge timestamps taken in a Python callback behind the GIL while a Python busy-loop spins for 100 ms per revolution; lock needs jitter < 2 ms); (b) the start-up stall is consistent with the A4988 MS1-3 pins not being at 1/16 (first commanded rate 1,600 steps/s = 8 rev/s in full-step mode) and/or a 9-12 V supply that cannot push 1 A through 4 mH at 500 Hz electrical (needs > 16 V by the L·di/dt + IR estimate). Both are 5-minute bench checks (section 12).

### 0.1 Comparison table (details in sections 3-8)

| | A: bit-bang GPIO, isolated SCHED_FIFO core | B: SPI DMA bitstream (recommended) | C: pigpio waves | F1: Pico/ESP32 offload | F2: Pi 5 RP1 PIO |
|---|---|---|---|---|---|
| Pulse-edge resolution | 18.5 ns (cntvct, 54 MHz) / ~50-100 ns (vDSO) | 0.5 us @ 2 MHz, 0.25 us @ 4 MHz, 0.125 us @ 8 MHz | 1 us (5 us default) | 8 ns (RP2040 PIO @125 MHz) | 5 ns class |
| Per-pixel timing error, typical | < 1 us, but +2-5 us on every 32nd pixel (250 Hz tick) | quantisation only (<= 0.5 us) | <= 1 sample (1-5 us) | ~0 | ~0 |
| Per-pixel timing error, worst | 50-300 us stalls (stock kernel), 60-90 us (RT) | none inside a revolution | none inside a wave | none | none |
| Frame-start phase error | ~ timestamp accuracy (us) | jitter of ioctl-to-first-SCLK: est. +/-5-20 us (0.04-0.16 px), compensated mean | sample-rate quantised (1-5 us) + wave-start latency | us-level (MCU-side index capture) | us-level |
| CPU cost | 100 % of one core, spinning | ~0.1 % (25 KB build + 1 ioctl per rev) | 10-25 % (sampling thread) + waves | ~0 on the Pi | low |
| Privileges | gpio group (/dev/gpiomem), rtprio limit | spi + gpio groups, rtprio limit | root (/dev/mem) or root daemon | USB/UART access | gpio group (udev rule) |
| Kernel/OS risk | tick/IPI stalls; RT throttling if runtime not -1 | core-clock scaling changes SPI clock unless `core_freq_fixed=1`; 65,532-byte transfer cap | missing from Trixie; DMA channel overlap; no Pi 5 | none | Pi 5 only; TC358743 adapter support unclear |
| Wiring change | none (GPIO 21) | LED input to GPIO 10 (already documented alternative) | none | new board + cable | new Pi |

---

## 0.2 Environment facts to pin down first

- **FACT**: the current Raspberry Pi OS Trixie arm64 image (`2026-10-06-raspios-trixie-arm64`) ships kernel `linux-image-6.18.50+rpt-rpi-v8` (1:6.18.50-1+rpt1), `python3-rpi-lgpio 0.6`, `python3-lgpio 0.2.2`, `python3-libgpiod 2.2.1` (libgpiod **v2** API), `python3-spidev 3.6`, `v4l-utils 1.30.1`, and **no** `python3-rpi.gpio`. Source: the image's SBOM, https://downloads.raspberrypi.com/raspios_arm64/images/raspios_arm64-2026-10-06/2026-10-06-raspios-trixie-arm64.sbom.xz (downloaded and parsed; see also `.info` alongside it). The Raspberry Pi apt index for trixie lists `linux-image-rpi-v8` = 1:6.18.50-1+rpt1 and `linux-image-6.12.{25,34,47,62,75}+rpt-rpi-v8(-rt)` (http://archive.raspberrypi.com/debian/dists/trixie/main/binary-arm64/Packages.gz). So the Pi is on **6.12.x or 6.18.x** depending on when it was imaged/upgraded; the task's "probably 6.12" is plausible but not certain. Verify: `uname -r`.
- **FACT**: all kernel-source facts below were checked on both `rpi-6.12.y` and `rpi-6.18.y`: `bcm2711_defconfig` (6.18 lines 6,7,10,67,787,795,1615), `bcm283x.dtsi` PWM clock 50 MHz (6.18 line 418), `pwm-overlay.dts` clock override (6.18 line 42), `gpiolib-cdev.c` hard-IRQ timestamp (6.18 lines 608/812/1076), `spi-bcm2835.c` (6.18 lines 72/1070/1218/1398), `pwm-bcm2835.c` (6.18 lines 67/131). Same code, different line numbers.
- **FACT**: `v4l-utils 1.30.1` is what the image ships, consistent with the user's observation that `--fix-edid-checksums` is not accepted (that is a 1.22-era option).

---

## 1. The timing requirement in numbers (ESTIMATE, arithmetic)

- 10 FPS => T_rev = 100 ms; 800 pixels => slot = 125.0 us (8.0 kHz). 0.1 px = 12.5 us.
- Minimum on-time 5 us (led.py:238). Desired edge resolution <= 0.5 us.
- Sync-lock tolerance in the Python app: |fps - target|/target < 3 % (= 3 ms per revolution) and jitter < 2 ms (sensor.py:189-196).
- Stepper: 200 full steps x 16 = 3,200 microsteps/rev (motor.py:371); 10 rev/s => 32,000 steps/s => STEP period 31,250 ns; 2,000 full steps/s => 500 Hz electrical.

---

## 2. What the Python app does today (facts from the code) and the parts that matter for the port

- **FACT** sensor path order: RPi.GPIO `add_event_detect(RISING, bouncetime=20)` (sensor.py:221-223) -> else libgpiod **v1** API (`chip.get_line`, `LINE_REQ_EV_RISING_EDGE`, sensor.py:247-249) -> else `mock-fallback` that synthesises an "edge" every `1/target_fps` seconds with `time.sleep` (sensor.py:239-242, 259-264). All three call `_handle_edge()` which timestamps with `time.perf_counter()` **in the Python callback** (sensor.py:173), applies a 20 ms software debounce (sensor.py:177), computes jitter against the mean of the last 10 periods and sets `sync_locked` (sensor.py:183-196). Telemetry exposes which backend is live as `backend` (sensor.py:280-293).
- **FACT** on the Trixie image `import RPi.GPIO` resolves to **rpi-lgpio** (SBOM above; no real RPi.GPIO installed). `bouncetime=20` then most likely maps to lgpio's debounce (HYPOTHESIS: rpi-lgpio's implementation was not read; check with `python3 -c "import inspect, RPi.GPIO as G; print(inspect.getsource(G.add_event_detect))"`), and lgpio documents that "level changes will be timestamped debounce microseconds after the actual level change" (https://abyz.me.uk/lg/lgpio.html, lgGpioSetDebounce). If the real `RPi.GPIO 0.7.1` were installed instead (Debian trixie carries `python3-rpi.gpio 0.7.1~a4-1.2`, whose changelog has no kernel-6.6 fix: https://metadata.ftp-master.debian.org/changelogs/main/r/rpi.gpio/rpi.gpio_0.7.1~a4-1.2_changelog; PyPI's latest is 0.7.1 of 2022-02-06, https://pypi.org/project/RPi.GPIO/), `add_event_detect` fails on kernels >= 6.6 because the first gpiochip's global number base became 512 (Phil Elwell, raspberrypi/linux issue #6037, 2024-03-13: "the choice to stop swimming against the tide was intentional"; Dave Jones: "rpi-lgpio is intended as a drop-in replacement for RPi.GPIO in the new kernel-based /dev/gpiochip world", https://github.com/raspberrypi/linux/issues/6037). The libgpiod fallback in sensor.py uses the v1 API which does not exist in `python3-libgpiod 2.2.1`, so a failure of the first path lands in **mock-fallback**, where "sync" is a sleep loop unrelated to the disk.
- **FACT** LED output (GPIO path): a Python thread busy-waits per pixel for the whole revolution (led.py:206-253; spin loops at 240-242 and 247-249). CPython's switch interval is 5 ms, so while this thread spins the sensor callback (and its `perf_counter()` timestamp) can be delayed by up to ~5 ms per GIL hand-over; that alone exceeds the 2 ms jitter threshold.
- **FACT** the existing `SPILEDModulator` cannot work as written: it rounds each pixel to whole bytes (`bits_per_pix` 250 -> `bytes_per_pix` 32 = 256 bits, led.py:370-371), so a 100 ms revolution is emitted as 102.4 ms (ESTIMATE: 2.4 %, 19 pixels of drift per revolution), it splits the stream into 4,096-byte ioctls with a gap at every boundary (led.py:284, 391-401), and it never aligns the start of the stream to the index edge.
- **FACT** motor: `A4988PWMDriver.set_rate` writes integer-ns `period`/`duty_cycle` to sysfs, shrinking duty first when speeding up (motor.py:430-437); the rate thread runs every 20 ms, ramps from 0.5 FPS, slew-limits and applies `trim_us` (motor.py:716-755); the trim is P-only, kp = 0.05 (controller.py:67, 203-209).
- **HYPOTHESIS (diagnosis, from the above)**: the "locks ~1 s, unlocks, repeats" symptom is a software limit cycle: as soon as the sensor reports lock, frames are handed to the LED thread, whose 100 ms Python spin loops delay the next sensor callbacks by several ms => jitter > 2 ms => unlock => `_supervise_loop`/watchdog blanks (controller.py:252-255), the spin loops stop, callbacks get timely again => lock => repeat. Bench check 7 in section 12 settles it (look at `sensor.backend` and `jitter_ms` while the LED is on vs blanked). This mechanism is independent of the disk mass.

---

## 3. Option A - bit-banged GPIO from a SCHED_FIFO thread on an isolated core

### 3.1 Facts

- **Register map (FACT)**: BCM2711 GPIO lives at bus address `0x7e200000` (ARM physical `0xfe200000`); register offsets `GPFSEL0 0x00`, `GPSET0 0x1c`, `GPCLR0 0x28`, `GPLEV0 0x34`, `GPEDS0 0x40`, `GPREN0 0x4c`, `GPIO_PUP_PDN_CNTRL_REG0 0xe4` (BCM2711 ARM Peripherals, GPIO register table; https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf). Note the 2711 pull registers differ from the 2835's GPPUD/GPPUDCLK.
- **/dev/gpiomem (FACT)**: the `gpiomem` DT node maps exactly that 4 KiB page (`arch/arm/boot/dts/broadcom/bcm270x-rpi.dtsi:53-56`, `reg = <0x7e200000 0x1000>`), driver `drivers/char/broadcom/bcm2835-gpiomem.c`; udev gives it `GROUP="gpio", MODE="0660"` (`SUBSYSTEM=="*gpiomem*"` in raspberrypi-sys-mods `99-com.rules`, https://github.com/RPi-Distro/raspberrypi-sys-mods/blob/master/etc.armhf/udev/rules.d/99-com.rules). No root needed. `CONFIG_STRICT_DEVMEM` is **not set** in `bcm2711_defconfig`, so root could also use `/dev/mem`, but that is not needed here.
- **Time source (FACT)**: the kernel enables EL0 access to the virtual counter `CNTVCT_EL0` unless an erratum workaround applies (`drivers/clocksource/arm_arch_timer.c:983-1004`, `cntkctl |= ARCH_TIMER_USR_VCT_ACCESS_EN`). The Pi 4 generic timer runs at 54 MHz (18.5 ns): public Pi 4 dmesg logs show `arch_timer: cp15 timer(s) running at 54.00MHz` and `sched_clock ... 54MHz` (e.g. https://openbenchmarking.org/system/2403028-FI-RAM10393839/ARMv8%20Cortex-A72%20-%20-%20Raspberry%20Pi%204%20Model%20B%20Rev%201.5/dmesg). Verify on the user's board (bench check 2). `clock_gettime(CLOCK_MONOTONIC)` goes through the arm64 vDSO, which reads the same counter.
- **Kernel configuration (FACT, `arch/arm64/configs/bcm2711_defconfig`, same on 6.12 and 6.18)**: `CONFIG_PREEMPT=y` (full preemption, not RT), `CONFIG_NO_HZ=y` (= NO_HZ_IDLE), `CONFIG_HIGH_RES_TIMERS=y`, `CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND=y`, `CONFIG_GPIO_SYSFS=y`; **absent**: `CONFIG_NO_HZ_FULL`, `CONFIG_RCU_NOCB_CPU`, `CONFIG_PREEMPT_RT`, `CONFIG_HZ_*` (so the arm64 default `HZ_250` applies, `kernel/Kconfig.hz:6-8`). `RCU_NOCB_CPU` is `default n` and `depends on RCU_EXPERT || NO_HZ_FULL` (`kernel/rcu/Kconfig:238-242`). `CPU_ISOLATION` is default y, so `isolcpus=` works.
- **cmdline semantics (FACT, `Documentation/admin-guide/kernel-parameters.txt`)**: `nohz_full=` only acts "In kernels built with CONFIG_NO_HZ_FULL=y" (line 3984-3992) => a no-op on the stock kernel; `rcu_nocbs=` likewise needs `RCU_NOCB_CPU`; `isolcpus=` (line 2436) defaults to the `domain` flag, is documented as deprecated in favour of cpusets, and its `nohz` flag also needs NO_HZ_FULL; `irqaffinity=` (line 2399) sets the default IRQ affinity mask. Net: on the stock kernel `isolcpus=3 irqaffinity=0-2` is what actually takes effect; the 250 Hz scheduler tick still runs on CPU 3 while a task runs there.
- **RT throttling (FACT)**: a SCHED_FIFO thread that spins 100 % is throttled to 950 ms per second by default (`/proc/sys/kernel/sched_rt_runtime_us` = 950000; -1 disables; `Documentation/scheduler/sched-rt-group.rst:92-114`). A bit-bang design must set it to -1 (or accept a 50 ms hole every second).
- **PREEMPT_RT availability (FACT)**: Raspberry Pi publishes `linux-image-rpi-v8-rt` (trixie: 1:6.18.50-1+rpt1; also `linux-image-6.12.xx+rpt-rpi-v8-rt`) in archive.raspberrypi.com; install with `sudo apt install linux-image-rpi-v8-rt` (6by9, forum thread t=396623, 2026-03-03). The announcement thread (t=388298, May-Jun 2025) calls it experimental, says Pi 4/CM4 need `arm_64bit=1`, and timg236 notes the Pi 4/5 display compositor (HVS) "has high priority and can significantly affect latency measurements" (https://forums.raspberrypi.com/viewtopic.php?t=388298).
- **Measured latencies on Pi 4 (FACT, secondary sources)**:
  - lemariva, Pi 4B, cyclictest `-l50000000 -m -S -p90 -i200 -h400`: max **301 us** on 4.19.57 standard vs **93 us** on 4.19.59-rt23 (https://lemariva.com/blog/2019/09/raspberry-pi-4b-preempt-rt-kernel-419y-performance-test).
  - ros-realtime image, Pi 4, 2 h of CPU stress, pinned frequency + performance governor, RT kernels: worst case **58 us** (5.15.98-rt62) and **82 us** (6.8.4-rt11) (https://github.com/ros-realtime/ros-realtime-rpi4-image).
  - Pi forum t=382231 (Jan 2025): Pi 4 "maxed out at ca. 200 us" under `stress`, Pi 5 RT 6.13-rc spikes to 800 us (https://forums.raspberrypi.com/viewtopic.php?t=382231).
  - MDPI Electronics 10(11):1331 (2021), as summarised in search results (the paper itself returned HTTP 403 here): Pi 4 stock kernel 717 us max under load vs 279 us with PREEMPT_RT (https://www.mdpi.com/2079-9292/10/11/1331).
  - No source gave a clean Pi 4 "with vs without isolcpus" pair; treat isolation gains as HYPOTHESIS until measured (bench check 10).
- **Interrupt sources that matter (FACT/HYPOTHESIS)**: vc4 HVS/CRTC vblank and HDMI hotplug IRQs, the VL805 xHCI (USB) on PCIe, and unicam (CSI) frame IRQs all default to CPU0's mask; `irqaffinity=0-2` keeps them off CPU 3 (FACT: parameter semantics). What cannot be moved: the per-CPU tick and IPIs (TLB shoot-downs from `munmap`/`mprotect` in the same process, e.g. a Qt UI freeing memory, hit every core the process has run on). The HVS high-priority effect reported by timg236 is a bus-contention effect, not an IRQ, so affinity does not remove it (HYPOTHESIS as to magnitude).

### 3.2 Estimates

- vDSO `clock_gettime` cost on Cortex-A72 @ 1.5 GHz: ~40-120 ns (ESTIMATE; it is a counter read plus a multiply/shift). Direct `mrs x0, cntvct_el0` (+ `isb`): ~10-30 ns. Either is negligible against a 125 us slot; use the raw counter inside the spin loop and convert with `cntfrq_el0`.
- Tick interference: HZ = 250 => 25 ticks per revolution, one every 32 pixels; each tick costs ~2-5 us of CPU on the isolated core (ESTIMATE). Because the loop uses absolute deadlines, the damage is confined to the pixel in which the tick lands: if it lands inside the on-time, the pulse is 2-5 us longer (a 5 us minimum pulse becomes 7-10 us, i.e. up to 2x brightness on that pixel); if it lands in the off-time, that pixel's off-edge is unaffected. About 1 in 32 pixels gets a 2-5 us error, visible as sparkle at 8 Hz.
- Worst case: the cyclictest numbers above are wake-up latencies, i.e. the sum of the stalls a spinning thread would also suffer: expect 50-100 us events occasionally on a stock kernel with isolation (HYPOTHESIS), 200-300 us without, 60-90 us on RT. A 100 us stall is nearly a whole pixel.
- CPU: 100 % of CPU 3, permanently (and the core runs hot; with `performance` governor no frequency change, but thermal throttling could still kick in).

### 3.3 Verdict

Workable for a bench, and the natural fallback for the current GPIO 21 wiring, but it spends a core to achieve timing that is still perturbed every 4 ms and occasionally by 100 us. Option B achieves better per-pixel timing with ~0.1 % CPU and no isolation tricks.

---

## 4. Option B - SPI DMA bitstream from spidev (recommended)

### 4.1 Facts

- **spidev buffer (FACT)**: `static unsigned bufsiz = 4096; module_param(bufsiz, uint, S_IRUGO)` ("data bytes in biggest supported SPI message"), `drivers/spi/spidev.c:86-88`; writes/ioctls larger than `bufsiz` are rejected (lines 146, 183, 254-264). spidev is a module (`CONFIG_SPI_SPIDEV=m`), so either `spidev.bufsiz=65536` on the kernel command line or `options spidev bufsiz=65536` in `/etc/modprobe.d/spidev.conf` works. The current value is readable at `/sys/module/spidev/parameters/bufsiz`.
- **Max transfer (FACT)**: `spi-bcm2835.c` caps a single transfer at **65,532 bytes** when DMA is available ("DMA transfers are limited to 16 bit (0 to 65535 bytes) by" the DLEN register; `drivers/spi/spi-bcm2835.c:1218`), and DLEN is a 16-bit byte count (BCM2711 datasheet, SPI DLEN register). Transfers of >= 96 bytes use DMA (`BCM2835_SPI_DMA_MIN_LENGTH 96`, line 72); shorter ones are polled (`polling_limit_us = 30`, line 79). SPI0 has DMA channels wired in DT (`bcm270x.dtsi:262-265`, `dmas = <&dma 6>, <&dma 7>` = DREQ TX/RX).
- **Clock (FACT)**: SPI0's clock is the VPU/core clock (`bcm283x.dtsi:329-337`, `clocks = <&clocks BCM2835_CLOCK_VPU>`). The driver computes `cdiv = DIV_ROUND_UP(clk_hz, spi_hz); cdiv += (cdiv % 2);` with `cdiv = 0` meaning 65536 (`spi-bcm2835.c:1069-1070`), i.e. **SCLK = core / even integer**, matching the datasheet ("The divisor must be a multiple of 2. Odd numbers rounded down."). `bs->clk_hz` is read **once at probe** (`clk_get_rate`, line 1398, "core clock cached speed"), never per transfer.
- **Core clock scaling (FACT)**: Pi 4 defaults are `core_freq = 500`, `core_freq_min = 200`; `core_freq_fixed=1` "disables active scaling of the core clock frequency" so that "peripherals using the core clock keep a consistent speed" (https://www.raspberrypi.com/documentation/computers/config_txt.html). Phil Elwell: "SPI, like I2C, is run from the core clock" and "The SPI clock is generated using a divisor from the core clock" (raspberrypi/firmware issue #1308, 2020-07-20/21); popcornmix closed it on 2026-10-06 with "The setting `core_freq_fixed=1` should resolve this" (https://github.com/raspberrypi/firmware/issues/1308). Without it, a transfer started while the core is at 200 MHz runs at 40 % of the programmed bit rate - fatal for a time-base. Note the 4Kp60 case: `core_freq` is boosted to 550 MHz with `hdmi_enable_4kp60` (same doc page), so don't enable that.
- **Permissions (FACT)**: `SUBSYSTEM=="spidev", GROUP="spi", MODE="0660"` (99-com.rules); the desktop user is in `spi` per install.sh:65. `dtparam=spi=on` is required (overlays README line 442-443).
- **Between transfers (FACT from driver structure)**: each `spi_transfer` ends with `bcm2835_spi_reset_hw()` (clears TA/DMAEN, clears FIFOs, DLEN = 0; `spi-bcm2835.c:350-360`) and the next transfer is set up from the completion path, so **two transfers in one `SPI_IOC_MESSAGE(n)` are not gapless**: there is a completion-IRQ/DMA-setup gap between them (ESTIMATE 10-50 us). Consecutive ioctls add user-space wake-up on top. Design for one transfer per revolution.
- **MOSI idle level (HYPOTHESIS)**: the datasheet does not state the MOSI level after TA is cleared; conventional controllers either drive low or hold the last bit (see the kernel's `SPI_MOSI_IDLE_LOW/HIGH` mode discussion in `include/uapi/linux/spi/spi.h`). Mitigation that costs nothing: end every bitstream with >= 8 zero bits and keep the LED driver's enable polarity "high = on". Bench check 11 measures it.

### 4.2 Estimates (arithmetic)

- 2 MHz: cdiv = 500e6/2e6 = 250 (even => exact at a 500 MHz core); 250 bits per 125 us slot; 0.5 us resolution; 5 us minimum = 10 bits; 200,000 bits = **25,000 bytes per revolution** (fits one transfer). 4 MHz: cdiv 125 is odd => the driver uses 126 => 3.968 MHz; choose 500e6/124 = 4.032 MHz or just compute bit count from the *actual* `effective_speed_hz` (spidev returns it); 50,000 bytes per revolution (fits). 8 MHz: 100,000 bytes (would need two transfers => gap; not recommended).
- Pixel boundaries are not byte-aligned in general (the bit count per revolution = round(T_rev x f_spi) varies with the measured period), so the builder must work at bit granularity: pixel i spans bits [floor(i x B/800), floor((i+1) x B/800)) and the on-run is the first `round(value x brightness/255 x (B/800))` bits of that span (MSB-first within each byte, as the controller shifts). Cost: ~4 x 64-bit masked word ops per pixel => ~3,200 word ops + 25 KB of stores: **~10-30 us per revolution** on an A72 (ESTIMATE). Use a 256-entry table of run lengths; no per-pixel division.
- Start-of-transfer alignment (ESTIMATE of the chain): index edge -> hard-IRQ timestamp T_edge (accurate to IRQ-entry latency, ~1-5 us typical) -> `edge_irq_thread` -> `poll()` wake of the index thread (stock kernel, SCHED_FIFO, isolated core: ~30-100 us typical, 200-300 us rare; RT: < 100 us) -> build (~20 us) -> `ioctl` -> DMA start (copy_from_user 25 KB ~5 us, DMA map, register writes: ~10-30 us). Call the last leg L. The thread computes `elapsed = now - T_edge` just before the ioctl and shifts the stream by `(elapsed + L_cal) x f_spi` bits (bit-granular, since it builds the stream after the edge anyway). The **residual error is only the jitter of L**, estimated +/-5-20 us (0.04-0.16 px), and it is re-zeroed every revolution so it cannot accumulate. The first `elapsed + L` of each revolution (1-3 pixels at a fixed disk angle next to the notch) is dark.
- End of revolution: size the transfer to `period_pred - guard` with guard ~ 300-500 us (3-4 pixels dark at the end, also at a fixed angle) so the ioctl returns before the next index event; a late ioctl that is still running when the next edge arrives would queue behind and lose a frame of phase.
- Within-revolution drift: the stream assumes the period predicted from the last N edges. A synchronous stepper on a hardware STEP clock has very small revolution-to-revolution period variation (HYPOTHESIS: < 20 us; bench check 14 measures it). A 20 us period error = 0.16 px at the end of the frame, zero at the start.
- CPU: ~50-100 us per revolution => ~0.1 % of a core.

### 4.3 Verdict

Hardware-paced, 0.25-0.5 us edges, no root, immune to scheduler stalls inside the frame, one compensated software event per revolution. The two configuration pre-requisites (`spidev.bufsiz`, `core_freq_fixed=1`) are cheap and verifiable. The wiring change (LED driver input from GPIO 21 to GPIO 10) is already documented in docs/HARDWARE.md:70 and :95.

---

## 5. Option C - pigpio waveforms

- **FACT** (pigpio v79 headers/source, https://github.com/joan2937/pigpio): default sample period **5 us** on the **PCM** peripheral (`pigpio.h:6543-6544`, `PI_DEFAULT_CLK_MICROS 5`, `PI_DEFAULT_CLK_PERIPHERAL PI_CLOCK_PCM`), configurable to 1/2/4/5/8/10 us via `gpioCfgClock`; documented CPU cost of the sampling thread: 1 us 25 %, 2 us 16 %, 4 us 11 %, 5 us 10 %, 8 us 15 %, 10 us 14 % (https://abyz.me.uk/rpi/pigpio/cif.html, gpioCfgClock). PCM is the default, so the hardware-PWM STEP output on GPIO 18 is not disturbed unless `PI_CLOCK_PWM` is chosen; `dtparam=audio=off` (install.sh:81) frees both.
- **FACT** wave limits: `PI_WAVE_BLOCKS 4`, `PI_WAVE_MAX_PULSES = 4 x 3000 = 12,000`, `PI_WAVE_MAX_MICROS = 30 min` (`pigpio.h:664-692`); a revolution is 1,600 pulses (on + off per pixel), well inside. `PI_WAVE_MODE_ONE_SHOT_SYNC` "wait[s] for the current waveform to reach the end of a cycle or finish before starting the new waveform" (cif.html, gpioWaveTxSend), so revolutions can be chained without a gap - but then the phase free-runs on pigpio's DMA clock and must be re-synchronised by trimming wave lengths, exactly as in option B.
- **FACT** privileges: the library opens `/dev/mem` (`pigpio.c:7360`) and the daemon "requires sudo privileges to launch the library" (https://abyz.me.uk/rpi/pigpio/pigpiod.html); after that, ordinary users talk to `pigpiod` over a socket/pipe. `CAP_SYS_RAWIO` alone does not grant `/dev/mem` (the device node is root:kmem 0640), and pigpio also needs the mailbox (`/dev/vcio`) for DMA memory. The realistic shape is a root daemon plus the app as a socket client (`pigpiod_if2`), with ~1,600-pulse wave uploads per revolution over localhost.
- **FACT** DMA channels: on BCM2711 pigpio defaults to primary channel **7** and secondary **6** (`pigpio.h:6550-6551`, `pigpio.c:7389-7402`), while the kernel's DMA controller owns channel mask `0x07f5` = channels {0,2,4,5,6,7,8,9,10} (`bcm2711.dtsi:106`). Overlap with kernel-allocated channels (spi0, i2s, ...) is therefore possible (HYPOTHESIS as to whether a conflict occurs on this system; there is no arbitration).
- **FACT** index timestamps: `gpioSetAlertFunc` ticks are "microseconds since boot" from the sampled DMA stream; "Level changes shorter than the sample rate may be missed" (cif.html). Resolution = sample period (1-5 us).
- **FACT** availability: `pigpio` is in the bookworm index of archive.raspberrypi.com but **not** in the trixie index (checked 2026-11-03); forum thread t=392438 (Oct 2025) confirms "Package pigpio is not available" on Trixie and reports building v79 from source works after installing `python3-setuptools python3-full` (runtime on Pi 4 + 6.12/6.18 not confirmed there) (https://forums.raspberrypi.com/viewtopic.php?t=392438). pigpio does not support the Pi 5 (same thread; pigpio has no RP1 support).
- **ESTIMATE** timing: 1 us resolution => 125 levels per slot, 5 us minimum = 5 samples; comparable to SPI at 1 MHz but with 25 % of a core consumed by sampling and a root daemon. Verdict: no advantage over B on any axis that matters here.

---

## 6. Option D - index pulse via the GPIO character device (and what RPi.GPIO/lgpio do)

- **FACT** (`drivers/gpio/gpiolib-cdev.c`): the edge detector registers `request_threaded_irq(irq, edge_irq_handler, edge_irq_thread, irqflags | IRQF_ONESHOT, ...)` (line 1192); `edge_irq_handler` runs in hard-IRQ context and does `line->timestamp_ns = line_event_timestamp(line)` with the comment "Just store the timestamp in hardirq context so we get it as close in time as possible to the actual event" (lines 921-930); `line_event_timestamp` returns `ktime_get_ns()` (CLOCK_MONOTONIC) unless `GPIO_V2_LINE_FLAG_EVENT_CLOCK_REALTIME` or HTE is requested (lines 718-726). The thread half only copies the event to the FIFO and wakes `poll()`; if the timestamp is 0 it means "a nested threaded interrupt" and the thread timestamps late (line 875 ff.).
- **FACT** the BCM2835/2711 pinctrl GPIO interrupt is a **chained** hard-IRQ handler, not a nested/threaded one: `bcm2835_gpio_irq_handler` with `chained_irq_enter/exit` (`drivers/pinctrl/bcm/pinctrl-bcm2835.c:456-489`), installed as `girq->parent_handler` (line 1374), child lines use `handle_edge_irq` (line 682). So the hard-IRQ timestamp path is the one taken on the Pi 4. Even on PREEMPT_RT the primary handler is not force-threaded because it carries `IRQF_ONESHOT` (`kernel/irq/manage.c:1368-1373`).
- **FACT** software debounce: with the `DEBOUNCE` attribute the line is requested with a plain `request_irq(irq, debounce_irq_handler, ...)` (line 1069) that only does `mod_delayed_work(system_wq, &line->work, usecs_to_jiffies(debounce_period_us))` (lines 958-962); the timestamp is taken in `debounce_work_func` when the work runs (line 968 ff.) - i.e. >= the debounce period later, with workqueue jitter. **Do not use it**; debounce in user space by ignoring edges closer than, say, 50 % of the predicted period (the Python code already does a 20 ms version at sensor.py:177).
- **FACT** permissions: `SUBSYSTEM=="gpio", GROUP="gpio", MODE="0660"` makes `/dev/gpiochip*` group-gpio read/write (99-com.rules). `BIAS_PULL_DOWN` and `EDGE_RISING` are plain `GPIO_V2_LINE_FLAG_*` bits in the request.
- **ESTIMATE** accuracy vs latency: the timestamp error is the hard-IRQ entry latency (interrupts-disabled sections elsewhere, GIC delivery): typically 1-5 us, rarely tens of us; the user-space wake-up (`poll`/`read`) is the cyclictest-class number (section 3), 30-300 us on a stock kernel. Only the former matters once the start of the SPI transfer is compensated with the timestamp.
- **FACT** RPi.GPIO / lgpio status (summarised; details in section 2): real `RPi.GPIO 0.7.1` uses `/dev/gpiomem` for I/O and the sysfs GPIO interface for edge detection, which broke on kernels >= 6.6 when the first chip's global base became 512 (issue #6037); `CONFIG_GPIO_SYSFS=y` is still on in 6.12/6.18 but the numbering is 512+N. The Trixie image ships `rpi-lgpio` (RPi.GPIO API over lgpio over the cdev) instead. lgpio reports the kernel event timestamp in ns ("alert time in nanoseconds", origin "kernel dependent", https://abyz.me.uk/lg/lgpio.html) and delivers it from its own notification thread, so a C++ port should read the cdev directly rather than go through lgpio's thread.

---

## 7. Option E - hardware PWM for STEP

- **FACT** clock: the PWM node sets `assigned-clock-rates = <50000000>` (`arch/arm/boot/dts/broadcom/bcm283x.dtsi:413-421`, same in 6.18), i.e. a **50 MHz / 20 ns** time base, not 10 MHz / 100 ns. The datasheet calls clk_pwm "nominally 100MHz, but can be varied by the clock manager" (BCM2711 peripherals, PWM chapter). The `pwm` overlay has an override `clock = <&frag1>,"assigned-clock-rates:0"` (`pwm-overlay.dts:42`); the overlays README lists `clock  PWM clock frequency (informational)` and note 4 "Currently the clock must have been enabled and configured by other means" (README:4213-4229) - both stale-looking, since the Pi documentation states that integer parameters "can refer to non-existent properties" (https://www.raspberrypi.com/documentation/computers/configuration.html, Device Tree parameters), which would make `clock=100000000` create the property in the fragment and override the base node's 50 MHz. **HYPOTHESIS**: `dtoverlay=pwm,pin=18,func=2,clock=100000000` gives a 100 MHz / 10 ns time base; verify with `vcgencmd measure_clock pwm` (bench check 3).
- **FACT** driver arithmetic (`drivers/pwm/pwm-bcm2835.c`): `pc->rate = clk_get_rate(pc->clk)` once at probe (line 131); `period_cycles = DIV_ROUND_CLOSEST_ULL(state->period * pc->rate, NSEC_PER_SEC)` (line 67), minimum 2 cycles (`PERIOD_MIN`, line 24); duty likewise (line 76); PERIOD then DUTY then CONTROL are written **without stopping the channel** (lines 73-77). Whether the hardware applies a new RNG/DAT at the next period boundary is not stated in the datasheet (HYPOTHESIS: it does; a runt STEP pulse at a period change would at worst cost one microstep = 0.1125 deg = 0.25 px, bench check 13 with a scope if it matters).
- **FACT** sysfs rule: the PWM core rejects `duty_cycle > period`, so when shortening the period write duty first, when lengthening write period first - exactly what motor.py:431-437 does. Keep that in the port.
- **FACT** numbering/permissions: with one PWM controller enabled the Pi 4 exposes `pwmchip0`; the second **controller** (`pwm1: pwm@7e20c800`, `bcm2711.dtsi:275-283`) only reaches GPIO 40/41/45 (audio pins, not on the header). The header pin GPIO 19 is channel 1 of the **first** controller (`pwm0_1_gpio19`, `bcm2711.dtsi:879-885`, enabled by the `pwm-2chan` overlay). udev chgrp's the sysfs tree to `gpio` on export (99-com.rules pwm rule), and install.sh:159 additionally chgrp's the files from a root `ExecStartPre`. Verify the chip name with `ls /sys/class/pwm/` (bench check 5); the Python code already globs it (motor.py:385).
- **ESTIMATE** quantisation at 32,000 steps/s (T = 31,250 ns):
  - 50 MHz: 1562.5 cycles -> `DIV_ROUND_CLOSEST` -> 1563 cycles = 31,260 ns -> 31,989.8 steps/s -> **9.99680 FPS (-0.032 %, +32 us per revolution)**. LSB = 20 ns = 0.064 % = 64 us per revolution.
  - 10 MHz (the task's assumption): 312.5 -> 313 cycles = 31,300 ns -> -0.160 %, +160 us per revolution; LSB 0.32 %.
  - 100 MHz: 3,125 cycles exactly -> 10.00000 FPS; LSB 10 ns = 0.032 % = 32 us per revolution.
  - Exact mean speed at 50 MHz by dithering: alternate 1562 and 1563 cycles (write 31,240 ns and 31,260 ns) in the ratio that averages to 31,250 ns; the motor's inertia filters the 0.064 % step-to-step change completely. Or simply let the LED stream follow the measured period (section 9), in which case a 0.032 % frame-rate error is irrelevant.
  - The PWM clock itself comes from a PLL via the clock manager; a fractional (MASH) divider would add sub-cycle jitter to individual STEP edges but not to the mean rate (HYPOTHESIS; harmless for a stepper).
- Second channel (GPIO 19): useful as a free-running 8 kHz "pixel clock" scope trigger during calibration, or as a hardware-PWM LED dimmer for a non-video test pattern. Not useful for per-pixel modulation from user space (a sysfs write per 125 us is not feasible), and PWM-FIFO serialiser mode with DMA would need `/dev/mem` like pigpio.

---

## 8. Option F - alternatives in brief

- **Microcontroller offload (RP2040/Pico, or ESP32 as bitluni did)**: the MCU reads the index sensor with a hardware-timestamped capture, generates the 800 pixel pulses by PIO/timer (8 ns resolution on RP2040) and can even generate STEP; the Pi sends 800 bytes per revolution plus commands over USB-CDC/UART (8 kB/s). Removes every Linux timing concern; the only latency left is one revolution of video delay. Cost ~5 USD; adds a firmware project and a cable. The right choice if, after bench check 12, B's start-of-frame jitter turns out visible.
- **Pi 5 RP1 PIO**: `piolib` is a user-space clone of the Pico SDK PIO API talking to `/dev/pio0` on the Pi 5 (`SUBSYSTEM=="*-pio", GROUP="gpio"` udev rule suggested) (https://github.com/raspberrypi/utils/tree/master/piolib). Not applicable to the Pi 4 (no PIO), and the Waveshare page for the HDMI-CSI adapter both documents a Pi 5 path and states in its FAQ that it "does not support Raspberry Pi 5" (https://www.waveshare.com/wiki/HDMI_to_CSI_Adapter) - unresolved; not recommended as a path for this project.

---

## 9. Recommended real-time architecture for the C++ port

### 9.1 Threads

| Thread | Policy / priority | CPU | Work |
|---|---|---|---|
| `index+led` (one thread) | SCHED_FIFO 60, `mlockall` | 3 (isolated) | `poll()` on the gpiochip fd (timeout = 1.2 x predicted period); on event: PLL update, build bitstream, `ioctl(SPI_IOC_MESSAGE(1))` (blocks ~99 ms in the kernel for the DMA). Also owns the watchdog (blank if > 350 ms without a real or coasted edge). |
| `motor` | SCHED_OTHER (or FIFO 20) | 0-2 | every 20-50 ms: ramp, slow period trim, sysfs writes; DIR/ENABLE via cdev lines. Nothing time-critical. |
| `capture` | SCHED_OTHER | 0-2 | V4L2 `DQBUF` loop on `/dev/video0` (UYVY 1920x1080), sparse sampling of the mmap'd (uncached) buffer into 32x25 + preview; publishes the latest frame via a seqlock/double buffer. Do not read whole 4 MB frames from uncached memory per frame (ESTIMATE 10-20 ms each at 30 fps); sample ~16 points per cell. |
| `ui` (Qt main) | SCHED_OTHER | 0-2 | 10-30 Hz repaint from the published preview; never touches the LED/index state except through lock-free snapshots. |

Keep the 800-pixel frame hand-off lock-free (two frame buffers + atomic index). Avoid `munmap`/`mprotect` churn while running (TLB-shoot-down IPIs to CPU 3). Note: the index thread does not need an isolated core for correctness; isolation only trims its wake-up latency, which only lengthens the dark gap at the start of the frame.

### 9.2 Boot and OS settings (Pi OS, `/boot/firmware/...`)

- `config.txt`: `dtparam=spi=on`, `dtparam=audio=off`, `dtoverlay=pwm,pin=18,func=2,clock=100000000` (verify the clock; fall back to the default 50 MHz with dithering), `gpio=24=op,dh` (as today), **`core_freq_fixed=1`** (keeps the SPI bit rate constant; alternative `core_freq_min=500`), optionally `arm_boost=1` as shipped. Do not set `hdmi_enable_4kp60`.
- `cmdline.txt` (stock kernel): add `isolcpus=3 irqaffinity=0-2 spidev.bufsiz=65536`. Omit `nohz_full=3 rcu_nocbs=3` (no effect without `NO_HZ_FULL`/`RCU_NOCB_CPU`; harmless but misleading). With the RT kernel (`linux-image-rpi-v8-rt`, needs `arm_64bit=1`) the same line applies; check `zcat /proc/config.gz` before relying on `nohz_full`.
- IRQ steering after boot (root, e.g. a oneshot systemd unit): find the `pinctrl-bcm2835` (GPIO bank 0) and `spi` IRQ numbers in `/proc/interrupts` and write `8` to their `/proc/irq/<n>/smp_affinity` so the edge-IRQ thread and the SPI completion run on CPU 3 next to the index thread (no cross-CPU wake-ups). `irqaffinity=0-2` keeps everything else away.
- cpufreq: `performance` governor on all cores (`/sys/devices/system/cpu/cpufreq/policy0/scaling_governor`); the default is `ondemand` (`CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND=y`). Frequency changes on the Pi go through the firmware mailbox from a kworker; the `performance` governor avoids them.
- RT without root: `/etc/security/limits.d/mechanical-tv.conf` with `@gpio - rtprio 80` and `@gpio - memlock unlimited` (re-login), then `sched_setattr`/`pthread_setschedparam` from the app; or `setcap cap_sys_nice=eip` on the binary (then the loader ignores `LD_LIBRARY_PATH` etc.; prefer limits.d for a Qt app). `kernel.sched_rt_runtime_us=-1` is only needed if the bit-bang fallback (100 % spin) is used.
- `mlockall(MCL_CURRENT | MCL_FUTURE)` at start, after loading Qt (needs the memlock limit above); pre-touch the two bitstream buffers.

### 9.3 Per-revolution algorithm (index + LED)

```
state: T_last, period_pred (initial 1/target_fps), history ring of N=8 periods, L_cal (us), f_spi (effective_speed_hz from spidev)
loop:
  r = poll(gpiochip_fd, timeout = 1.2 * period_pred)
  if r == event:
      read gpio_v2_line_event -> T_edge (CLOCK_MONOTONIC ns, taken in hard IRQ)
      p = T_edge - T_last
      if p < 0.5 * period_pred: continue            # user-space debounce (no kernel DEBOUNCE)
      if |p - period_pred| < 0.02 * period_pred: push p into history   # outlier reject
      period_pred = mean(history) (or median); phase_err = T_edge - (T_last + period_pred)  # logged as "jitter"
      T_last = T_edge; coast = 0
  else (timeout):
      if ++coast > 3: blank LED; continue           # watchdog
      T_edge = T_last + period_pred; T_last = T_edge  # free-wheel one revolution on the prediction
  bits_total = round((period_pred - guard) * f_spi)   # guard ~ 300-500 us
  now = clock_gettime(MONOTONIC)
  skip_bits = round((now - T_edge + L_cal) * f_spi)   # L_cal: measured ioctl->first SCLK latency
  build(stream, frame[800], brightness, bits_total, skip_bits)   # bit-granular; pixel i starts at floor(i*B/800) - skip
  ioctl(spi_fd, SPI_IOC_MESSAGE(1), {tx_buf=stream, len=ceil((bits_total-skip_bits)/8), speed_hz=f_spi})
```

- `build` emits MSB-first bytes; pixel i's on-run length = `lut[value] = round(value/255 * brightness * B/800)` clamped to [0 or >= 10 bits (5 us), span]; the stream ends with >= 1 zero byte.
- `L_cal`: measure once with a scope between the GPIO 4 rising edge and the first SCLK edge (bench check 12) and store the mean; the app can also self-calibrate the *mean* by comparing, over many revolutions, where the user-set phase offset lands (any constant offset is absorbed by the existing `phase_offset` calibration in docs/HARDWARE.md:142, so L_cal's absolute value is not critical - only its jitter matters).
- The motor thread sets the STEP rate open-loop to `target_fps x 3200` (with the period written as 2 dithered values if the PWM clock is 50 MHz) and applies at most a slow integral correction (1 LSB per few seconds) if the measured mean period deviates by > 0.05 %. There is no need for the per-revolution P-trim of the Python app: the frame is phase-anchored to the index edge by construction.

### 9.4 Expected timing error of the recommended design (ESTIMATE, to be confirmed by bench checks 12 and 14)

| Term | Typical | Worst case | Visible effect |
|---|---|---|---|
| Pulse edge placement within the frame | 0.5 us quantisation @ 2 MHz (0.25 us @ 4 MHz); clock drift ~ppm | same | none |
| Frame-start phase (jitter of L) | +/-5-20 us (0.04-0.16 px) | +/-50 us (0.4 px) if an IRQ-off section hits the DMA setup | invisible to barely visible 1-frame horizontal wobble |
| Timestamp accuracy (IRQ-entry latency) | 1-5 us | tens of us | as above, same column |
| Period prediction error at end of frame | < 20 us (0.16 px) | a sudden speed change (missed steps) | picture skews for one frame, self-corrects next edge |
| Dead time at the start of frame (wake-up + L, uncompensated brightness only) | 100-300 us (1-2 px dark at a fixed angle) | 1-2 ms under heavy load (8-16 px dark for that frame) | a short dark tick at the notch angle; never a position error |
| Guard at end of frame | 300-500 us (3-4 px dark at a fixed angle) | same | same |

Compare option A: 2-5 us error on 1 of every 32 pixels plus 50-300 us stalls at random pixels.

---

## 10. Stall / sync facts and estimates that follow from the timing analysis

- **ESTIMATE** disk inertia (solid-disk upper bound): I = 1/2 x 0.080 kg x (0.125 m)^2 = 6.25e-4 kg m^2; ramp 2 FPS/s = 12.57 rad/s^2 => 7.9e-3 N m = **0.79 N cm** (1.08 N cm for the 110 g 2.4 mm disk) against the motor's **17 N cm** holding torque (FACT: STEPPERONLINE 17HE08-1004S: 1.8 deg, 17 N cm, 1.0 A/phase, 3.6 ohm, 4.0 mH +/-20 %; https://www.omc-stepperonline.com/e-series-nema-17-bipolar-1-8deg-17ncm-24-07oz-in-1a-42x42x23mm-4-wires-17he08-1004s). The ramp is not the problem.
- **ESTIMATE** torque at speed: at 600 RPM the phase current is a 500 Hz sinusoid; holding 1 A peak needs L x di/dt = 4 mH x 2 pi x 500 Hz x 1 A = **12.6 V** plus I x R = 3.6 V plus back-EMF, i.e. > 16 V before back-EMF. A 9-12 V supply cannot sustain rated current near 600 RPM, so available torque collapses with speed; the A4988 is rated to 35 V and 24 V is the usual remedy (HYPOTHESIS that this is the operative stall cause; bench check 9 measures the actual supply voltage under load and where in the ramp the stall occurs).
- **HYPOTHESIS** microstep wiring: the first commanded rate is 0.5 FPS x 3,200 = 1,600 steps/s (motor.py:724-738). If MS1-3 are not all high, that is 8 rev/s (full step), 4 rev/s (1/2), 2 rev/s (1/4) or 1 rev/s (1/8) from standstill, and the target becomes 20-160 rev/s - an instant stall in the coarser modes and a mid-ramp stall in 1/8. The A4988's MS pins have internal pull-downs (full step when left open). Bench checks 8-9.
- **HYPOTHESIS** current/thermal: a pot "fully right" may set Vref well above 0.8 V (I = Vref / (8 x Rs); 1 A needs 0.8 V with Rs = 0.1 ohm, 0.4 V with 0.05 ohm), overdriving a 1 A motor and tripping the A4988's thermal shutdown intermittently - another way to get "runs for a second, stops". Bench check 9 (Vref, Rs marking, driver temperature).
- **FACT** PWM quantisation contributes only -0.032 % speed (section 7); it is not a sync-loss cause.

---

## 11. On the user's question about the HDMI adapter and H.264

- **FACT**: the Waveshare adapter is a Toshiba TC358743XBG HDMI-to-CSI-2 bridge; it outputs raw 2-lane MIPI CSI-2 video (RGB888 up to 1080p30; the Pi 4 examples capture UYVY with `yavta ... -f UYVY`). H.264 appears on that page only in host-side commands (`yavta --encode-to=file.h264`, a GStreamer `v4l2h264enc` pipeline, `raspivid`); the adapter itself does no compression (https://www.waveshare.com/wiki/HDMI_to_CSI_Adapter). The page also says "HDMI to CSI camera does not support libcamera driver" (it is a V4L2 subdevice under the `unicam` driver instead, `CONFIG_VIDEO_TC358743=m`, `CONFIG_VIDEO_BCM2835_UNICAM=m` in bcm2711_defconfig).
- Consequence: there is no encoding to bypass and no firmware to write. The app already reads raw UYVY frames via V4L2 mmap (mechanical_tv/v4l2.py, capture.py); the "fast" path is to keep doing that and only sample the 800 cells the disk can show (section 9.1). Encoding would only ever add latency.

---

## 12. Bench checks (commands / measurements)

1. OS/kernel: `uname -r; cat /etc/os-release | head -2; zcat /proc/config.gz | grep -E '^CONFIG_(PREEMPT|NO_HZ|HZ|RCU_NOCB_CPU|CPU_ISOLATION|GPIO_SYSFS|SPI_SPIDEV|PWM_BCM2835)'` - confirms 6.12 vs 6.18, PREEMPT, HZ=250, absence of NO_HZ_FULL.
2. Timer: `dmesg | grep -i arch_timer` (expect `running at 54.00MHz`); `cat /sys/devices/system/clocksource/clocksource0/current_clocksource` (expect `arch_sys_counter`).
3. Clocks: `vcgencmd measure_clock core` at idle and under `stress-ng -c 4` (expect 200 MHz vs 500 MHz without `core_freq_fixed=1`; 500 both with it); `vcgencmd get_config core_freq core_freq_min core_freq_fixed`; after `dtoverlay=pwm,...,clock=100000000`: `vcgencmd measure_clock pwm` (expect ~100,000,000; default ~50,000,000) and `sudo cat /sys/kernel/debug/clk/clk_summary | grep -i pwm`.
4. Devices/permissions: `id; ls -l /dev/gpiochip0 /dev/gpiomem /dev/spidev0.0; cat /sys/module/spidev/parameters/bufsiz` (4096 default; 65536 after the change).
5. PWM: `ls /sys/class/pwm/; echo 0 | sudo tee /sys/class/pwm/pwmchip0/export; cat /sys/class/pwm/pwmchip0/pwm0/period`; write `31250` then read back (sysfs shows the requested ns, so also scope GPIO 18: expect 31.26 us at 50 MHz, 31.25 us at 100 MHz).
6. Python GPIO stack: `python3 -c "import RPi.GPIO as G; print(G.__file__, G.VERSION)"` (a path under `rpi_lgpio`/`RPi/__init__` from rpi-lgpio vs the C extension), `python3 -c "import gpiod; print(gpiod.__version__, hasattr(gpiod.Chip,'get_line'))"` (expect 2.x, False), `dpkg -l | grep -E 'rpi.gpio|rpi-lgpio|lgpio|gpiod|pigpio'`.
7. Python app telemetry: with the disk spinning, read the status JSON (web `/api/status` or the console log) and note `sensor.backend` (`RPi.GPIO`, `gpiod`, or `mock-fallback`) and `jitter_ms` while the LED is on vs. right after `blank`. `mock-fallback` means there is no real sync at all; jitter rising above 2 ms only while frames are being output confirms the GIL limit cycle.
8. Speed ratio: during the ramp compare `measured_fps` (sensor) with `current_fps` (motor); a ratio of 2/4/8/16 means MS1-3 are at 1/8, 1/4, 1/2 or full step. Also `gpiomon --chip gpiochip0 --edges rising 4` (libgpiod 2.x CLI) prints kernel-timestamped edges; the spacing gives the true period.
9. Multimeter on the A4988: MS1/MS2/MS3 to GND (expect logic-high, ~3.3-5 V, on all three for 1/16); Vref at the pot wiper (expect 0.4-0.8 V depending on Rs = R050/R100); VMOT under load (confirm 9-12 V vs 24 V); driver temperature and whether the stall coincides with it cutting out (thermal shutdown) rather than with a speed.
10. Scheduling latency: `sudo apt install rt-tests stress-ng`; baseline `sudo cyclictest -m -S -p90 -i200 -h400 -q -D 10m` while the app, capture and UI run; then with `isolcpus=3 irqaffinity=0-2`: `sudo cyclictest -m -a 3 -t 1 -p90 -i125 -h400 -q -D 10m` under `stress-ng -c 3 --vm 1 --vm-bytes 512M`. Max < 100 us makes option A's stalls tolerable; it is irrelevant to B except for the dark gap.
11. SPI time base: `dtparam=spi=on`, then from Python `spidev` send 25,000 bytes of `0xAA` at 2,000,000 Hz in a loop; scope GPIO 10: bit period must be 0.500 us at idle and under `stress-ng -c 4` (it will be ~1.25 us at idle without `core_freq_fixed=1`); also send a buffer ending in `0xFF` and read the MOSI level with a multimeter afterwards (idle-high => pad zeros).
12. Start latency distribution: two-channel scope, trigger on GPIO 4 rising, measure to the first SCLK edge (GPIO 11) over >= 100 revolutions with the C++ prototype; the mean becomes `L_cal`, the spread is the frame-start phase jitter. Target: spread < 25 us.
13. STEP period change glitch: scope GPIO 18 with persistence while the motor thread dithers 31,240/31,260 ns; any pulse narrower than 1 us high/low would be a glitch.
14. Period stability: log 1,000 index timestamps (`gpiomon` or the C++ prototype), compute the std-dev and max |p - mean| of the period at 10 FPS; this bounds the end-of-frame skew (20 us = 0.16 px).
15. DMA channel bookkeeping (only if pigpio is ever tried): `xxd /proc/device-tree/soc/dma@7e007000/brcm,dma-channel-mask` (expect `0x07f5`) and `cat /sys/module/dma/parameters/dmachans` if present.

---

## 13. Sources

Primary (kernel, `raspberrypi/linux`, branch `rpi-6.12.y`, re-checked on `rpi-6.18.y`):
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/gpio/gpiolib-cdev.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/pinctrl/bcm/pinctrl-bcm2835.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/kernel/irq/manage.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/spi/spi-bcm2835.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/spi/spidev.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/pwm/pwm-bcm2835.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/clocksource/arm_arch_timer.c
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm64/configs/bcm2711_defconfig
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm/boot/dts/broadcom/bcm283x.dtsi , bcm2711.dtsi , bcm270x.dtsi , bcm270x-rpi.dtsi , bcm2711-rpi-4-b.dts
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/arch/arm/boot/dts/overlays/pwm-overlay.dts and overlays/README
- https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/kernel/Kconfig.hz , kernel/rcu/Kconfig , Documentation/admin-guide/kernel-parameters.txt , Documentation/scheduler/sched-rt-group.rst

Primary (hardware, firmware, OS):
- BCM2711 ARM Peripherals: https://datasheets.raspberrypi.com/bcm2711/bcm2711-peripherals.pdf (GPIO register table; SPI CDIV/DLEN; PWM clk_pwm)
- config.txt reference: https://www.raspberrypi.com/documentation/computers/config_txt.html (Pi 4 defaults, `core_freq_fixed`)
- Device tree parameters: https://www.raspberrypi.com/documentation/computers/configuration.html
- raspberrypi/firmware issue 1308 (core clock vs SPI): https://github.com/raspberrypi/firmware/issues/1308
- raspberrypi/linux issue 6037 (RPi.GPIO vs gpio base 512): https://github.com/raspberrypi/linux/issues/6037
- udev rules: https://github.com/RPi-Distro/raspberrypi-sys-mods/blob/master/etc.armhf/udev/rules.d/99-com.rules
- Pi OS Trixie image 2026-10-06 SBOM/.info: https://downloads.raspberrypi.com/raspios_arm64/images/raspios_arm64-2026-10-06/
- Raspberry Pi apt index (RT kernels, pigpio absence): http://archive.raspberrypi.com/debian/dists/trixie/main/binary-arm64/Packages.gz
- RT kernel threads: https://forums.raspberrypi.com/viewtopic.php?t=388298 , https://forums.raspberrypi.com/viewtopic.php?t=396623
- pigpio: https://github.com/joan2937/pigpio (pigpio.h, pigpio.c), https://abyz.me.uk/rpi/pigpio/cif.html , https://abyz.me.uk/rpi/pigpio/pigpiod.html , https://forums.raspberrypi.com/viewtopic.php?t=392438
- lgpio: https://abyz.me.uk/lg/lgpio.html ; RPi.GPIO: https://pypi.org/project/RPi.GPIO/ , https://packages.debian.org/trixie/python3-rpi.gpio , https://metadata.ftp-master.debian.org/changelogs/main/r/rpi.gpio/rpi.gpio_0.7.1~a4-1.2_changelog
- piolib (Pi 5): https://github.com/raspberrypi/utils/tree/master/piolib
- Motor: https://www.omc-stepperonline.com/e-series-nema-17-bipolar-1-8deg-17ncm-24-07oz-in-1a-42x42x23mm-4-wires-17he08-1004s
- HDMI adapter: https://www.waveshare.com/wiki/HDMI_to_CSI_Adapter

Latency measurements (secondary):
- https://lemariva.com/blog/2019/09/raspberry-pi-4b-preempt-rt-kernel-419y-performance-test
- https://github.com/ros-realtime/ros-realtime-rpi4-image
- https://forums.raspberrypi.com/viewtopic.php?t=382231
- https://www.mdpi.com/2079-9292/10/11/1331 (numbers taken from search-result summary; page returned 403)
