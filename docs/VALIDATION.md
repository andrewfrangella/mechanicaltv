# Validation and acceptance

## Automated checks

Run from the repository root:

```bash
python3 -m unittest discover -s tests -v
bash -n install.sh packaging/make-test-clip.sh
node --check mechanical_tv/static/app.js
git diff --check
```

Python tests use temporary data and loopback sockets. FFmpeg and FFprobe are required for integration tests; without them the conversion tests are skipped, not passed. A sandbox that disallows local sockets must grant access before the integration suite can run.

Coverage includes:

- Password authentication, session logout, unauthenticated rejection, cross-origin POST rejection, and invalid numeric commands.
- Upload through HTTP, actual FFmpeg grayscale conversion, expected frame count/data shape, and authenticated MP4 byte-range responses.
- Play, pause, seek, loop, end-of-clip blanking, deletion protection and removal.
- Monotonic-clock simulation with deterministic test time.
- Recovery of interrupted uploads; invalid IDs and damaged prepared data.
- Corrupt video failure without stopping the conversion worker.

No test drives a motor or LED. No hardware or real-time behavior is verified by these tests.

## Local validation record

Development machine: Linux ARM64, Python 3.14.7, FFmpeg 9.0.1.

Recorded during the initial build:

- **Passed:** seven Python unit/integration tests, including actual FFmpeg conversion and HTTP playback.
- **Passed:** Bash syntax, JavaScript syntax, and Git whitespace checks.
- **Passed:** headless Chromium browser smoke test: sign in, upload, preparation, select, play, pause, reload with persistent session/player state, stop, health page, sign out.
- **Passed:** 1280-pixel desktop and 390-pixel mobile layouts; mobile has no horizontal overflow; no JavaScript page errors in the smoke test.
- **Inspected:** screenshots of both layouts with a generated test pattern. A desktop capture is included in `docs/images/player-desktop.png`.

GitHub Actions separately targets Python 3.11 on Ubuntu and its available FFmpeg. A workflow being present is not evidence that a hosted run has passed.

Pi OS installer, systemd hardening/resource limits, capture-device configuration, boot recovery, thermal behavior and sustained playback require validation on the actual Pi. The development environment is not Raspberry Pi OS.

## Browser acceptance

An optional repeatable smoke test is provided in `tests/browser_smoke.cjs`. Install Playwright as a development-only dependency outside the repository, then point `PLAYWRIGHT_MODULE` at it and `CHROMIUM_PATH` at an installed Chromium executable:

```bash
npm install --prefix /tmp/mechanical-tv-browser-check playwright
PLAYWRIGHT_MODULE=/tmp/mechanical-tv-browser-check/node_modules/playwright CHROMIUM_PATH=/usr/bin/chromium node tests/browser_smoke.cjs
```

The script creates temporary credentials/media, starts its own localhost server, and prints the screenshot directory. It stops the server afterward. Neither npm nor Playwright is needed on the appliance. Manual checks below remain useful for the actual Pi and intended browsers.

- [ ] Login and wrong-password message.
- [ ] Desktop and narrow phone layout.
- [ ] Upload a generated five-second clip.
- [ ] Watch queued/preparing/ready transitions.
- [ ] Select, compare source and TV image, play, pause, seek, loop and stop.
- [ ] Delete after stopping and confirm library removal.
- [ ] Refresh/reopen while playing and verify state comes from the Pi.
- [ ] Sign out and verify protected media/API access stops.
- [ ] Inspect health and download diagnostics.
- [ ] Confirm browser console has no application errors.

## Optional studio Pi acceptance — not yet performed

- [ ] Install on a fresh supported 64-bit Pi OS image.
- [ ] Confirm source preview H.264 playback in intended client browsers.
- [ ] Start the studio manually and verify idle state.
- [ ] Boot and capture with networking disconnected; no AP or browser dependency.
- [ ] Run a 30-minute loop, monitor memory, temperature, service stability, and browser behavior.
- [ ] Upload/convert while idle; verify queue deferral during playback.
- [ ] Interrupt an upload, restart during preparation, and verify recovery.
- [ ] Confirm configured disk reserve rejects oversized work without filling storage.
- [ ] Exercise service failure/restart and inspect logs.
- [ ] Test update, backup, password reset and recovery procedures.
- [ ] Shut down through the OS before disconnecting power.

## Hardware acceptance — future stage

Requires actual motor speed/index measurements, LED current and pulse timing measurements, calibration images, loaded-disk tests, loss-of-sync behavior and independently verified stop behavior. Browser simulation must never substitute for these checks.

## HDMI capture acceptance — hardware pending

Automated tests in `tests/test_capture.py` cover real generated-source conversion (fit and crop), exact frame shape, fragmented pipe reads, EOF, timeout, interruption, startup-failure retry blanking, and shutdown blanking. They do not verify V4L2 or physical output.

- [ ] Confirm exact capture device model/USB ID and Linux driver.
- [ ] Select actual video node and an advertised input mode.
- [ ] Install and inspect `capture.json` and `latest.pgm` with motor/LED disconnected.
- [ ] Reboot with network disconnected and no browser; conversion starts automatically.
- [ ] Start with capture device absent, attach it, and confirm recovery.
- [ ] Unplug during capture: preview blanks; replug: frames resume.
- [ ] Remove HDMI signal with USB attached; record whether device freezes, stalls or generates no-signal graphics. Frame watchdog cannot detect all these cases.
- [ ] Stop service and confirm preview blanking and FFmpeg termination.
- [ ] Run 30 minutes, measure memory/temperature/resource use and source-to-preview latency.
- [ ] Check configured device access under the unprivileged service account.
- [ ] Separately validate motor/current, sensor, LED circuitry and synchronized output before any optical video claim.

## HDMI preparation validation record

During this change, all 13 Python unit/integration tests passed, including generated-source HDMI pipeline tests and existing studio conversion/HTTP regressions. Bash syntax, JavaScript syntax, and Git whitespace checks passed. Actual Pi OS installation, systemd device permissions, USB/V4L2 capture, and physical timing remain untested.
