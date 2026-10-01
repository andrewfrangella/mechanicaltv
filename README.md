# Mechanical TV

An HDMI-first video converter for a Raspberry Pi 4 mechanical television based on [bitluni's design](https://github.com/bitluni/MechanicalTV).

**Hardware output is not implemented.** The software converts live capture to 32 × 25, 10 fps grayscale and publishes a local preview. It never drives a motor, LED, encoder, or GPIO. Pi and capture-device validation remain pending.

## Primary operating flow

```text
HDMI source → HDMI capture device → USB → Raspberry Pi 4
            → FFmpeg → grayscale frames → preview sink
                                      → future synchronized hardware output
```

Power on, connect the HDMI source, and conversion runs automatically. No AP connection, browser, login, upload, or internet is required for normal operation. The Pi's own micro-HDMI ports are display outputs; the source connects to the separate capture device. The prepared software expects a Linux V4L2 capture device; confirm the exact model and supported modes before assembly.

- Continuous conversion with fit or center crop and no recording/library requirement.
- Bounded pipe reads and latest-frame preview; no accumulating frame archive.
- Missing devices, disconnects, and stalls blank the preview and retry automatically.
- SIGTERM/SIGINT stops FFmpeg and blanks the preview.
- Explicit generated test source for development before hardware arrives.
- Standalone systemd service with video-device access and no web listener.
- Previous authenticated upload studio retained as a separately launched development tool.

## Develop without hardware

Requires Python 3.11+ and FFmpeg. No Python packages or frontend build are needed.

```bash
python3 -m mechanical_tv.capture --test-pattern --data ./data
```

Inspect `data/capture/latest.pgm` with a PGM-capable image viewer and `data/capture/capture.json` for state. These files are atomically replaced; reopen the image to refresh. Stop with Ctrl-C. The preview goes black on exit. For a finite smoke run, add `--frames 10`.

To capture on Linux after confirming the device modes:

```bash
python3 -m mechanical_tv.capture --device /dev/video0 --input-size 640x480 --input-fps 30 --data ./data
```

Use `--input-format mjpeg` only if your capture device advertises it. `--fit crop` fills the mechanical profile from the center; default `fit` preserves the whole source image.

## Install on Raspberry Pi OS

Use Pi OS 64-bit with Python 3.11+. Initial installation requires package access; runtime needs no network.

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

The installer enables standalone capture at boot. It does not prompt for a web password, create an AP, or enable GPIO/I2C. An existing installation's media, credentials, and environment file are preserved; its primary service switches from the web studio to capture. See [installation](docs/INSTALL.md) for device selection, permissions, migration, and maintenance.

## Documentation

| Document | Contents |
| --- | --- |
| [Installation](docs/INSTALL.md) | Capture device discovery, offline operation, service setup, recovery |
| [Architecture](docs/ARCHITECTURE.md) | Live pipeline, sink boundary, retained studio API |
| [Hardware](docs/HARDWARE.md) | Supplied parts and preparation for Pi 4 / Adafruit HAT |
| [Project plan](docs/PROJECT_PLAN.md) | HDMI-first milestones and pending hardware |
| [Validation](docs/VALIDATION.md) | Automated checks and actual-Pi acceptance |
| [Studio user guide](docs/USER_GUIDE.md) | Optional upload studio workflow |

## Tests

```bash
python3 -m unittest discover -s tests -v
bash -n install.sh packaging/make-test-clip.sh
node --check mechanical_tv/static/app.js
git diff --check
```

Tests cover capture framing, pipe fragmentation/EOF, stall/stop handling, retry blanking, and real generated-source conversion, alongside the retained studio tests. They never touch GPIO or a real capture device. FFmpeg-dependent checks skip when FFmpeg is absent.

## Optional upload studio

For file-based development, launch separately:

```bash
python3 -m mechanical_tv.server init --data ./data
python3 -m mechanical_tv.server serve --data ./data
```

Open `http://127.0.0.1:8080`. This is an independent simulated player and library; it does not control the HDMI service. FFprobe and the FFmpeg `libx264` encoder are needed for uploads. LAN exposure requires an explicit `--host`; an AP is never required.

## Limits

Capture compatibility, latency, and sustained performance have not been measured on the Pi. Receiving frames does not prove that HDMI signal is present: some capture devices generate their own no-signal frames. The stall watchdog detects absence of decoded frames only. The preview sink is not a physical timing engine, and requested 10 fps is not measured disk speed. Audio, RGB, physical calibration, motor controls, and synchronization remain pending.

## Attribution and licensing

Mechanical concept: [bitluni/MechanicalTV](https://github.com/bitluni/MechanicalTV). This repository contains independently written Pi software, without upstream firmware, models, or sample content. Review upstream permissions before redistributing those assets. No software license has been selected for this repository yet.
