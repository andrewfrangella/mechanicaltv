# Mechanical TV

An HDMI-first Raspberry Pi mechanical television project based on [bitluni's design](https://github.com/bitluni/MechanicalTV).

**Version 0.1.0 is a simulation release. It does not drive a motor, LED, encoder, or any GPIO.** It implements the media workflow before the physical output system is finalized. No Raspberry Pi or assembled-TV validation is claimed.

![Player showing a generated source test pattern and its 32 by 25 grayscale preview](docs/images/player-desktop.png)

## Primary operating flow

HDMI source → USB HDMI capture device → Raspberry Pi 4 / FFmpeg → 32 × 25 gray8 frames at 10 fps → latest-frame preview. Motor/LED output is a future integration step.

The installer now starts unattended HDMI capture, without an AP connection, HTTP server, uploads, or application login. It retries missing/disconnected capture devices and blanks the preview on failure. No GPIO is accessed. See [installation](docs/INSTALL.md) for device selection and [hardware](docs/HARDWARE.md) for the parts record.

The existing web studio remains an optional development tool for prerecorded test clips. Its features and screenshots describe that secondary simulation path.

## Start on a development computer

Requires Python **3.11+**, FFmpeg and FFprobe with the `libx264` encoder. No third-party Python packages, npm installation, or frontend build is needed.

```bash
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
python3 -m mechanical_tv.server init --data ./data
python3 -m mechanical_tv.server serve --data ./data
```

Open **http://127.0.0.1:8080** and sign in using the application password you just created. The password is separate from your Pi OS account. Initialization prompts privately and stores a salted scrypt hash.

Create a five-second sample clip in another terminal:

```bash
bash packaging/make-test-clip.sh /tmp/mechanical-tv-test.mp4
```

Upload it through **Library**, wait for **READY**, select it, then press **Play**. The server binds to localhost by default. To access a development server from your LAN, explicitly add `--host 0.0.0.0`.

## Install on Raspberry Pi OS

Use Raspberry Pi OS 64-bit, Python 3.11 or later. Lite and Desktop are both supported by the design. The installer is intended for Pi OS/Debian; it has not yet been executed on a Pi.

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

The installer installs capture dependencies and enables the HDMI service at boot. Inspect `/var/lib/mechanical-tv/live/capture.json` for capture state and `latest.pgm` for the converted frame. Initial installation needs internet; operation needs no network connection. Hardware output remains disabled.

## Documentation

| Document | Contents |
| --- | --- |
| [Installation](docs/INSTALL.md) | Pi OS, HDMI capture setup, updates, shutdown |
| [User guide](docs/USER_GUIDE.md) | Screens, uploads, playback behavior, limits, troubleshooting |
| [Architecture](docs/ARCHITECTURE.md) | Components, API, storage, processing, security boundaries |
| [Project plan](docs/PROJECT_PLAN.md) | Full intended flow and implemented/pending feature matrix |
| [Hardware notes](docs/HARDWARE.md) | Supplied parts, compatibility questions, future output boundary |
| [Validation](docs/VALIDATION.md) | Test commands, evidence, manual Pi acceptance checklist |

## Tests

```bash
python3 -m unittest discover -s tests -v
bash -n install.sh packaging/make-test-clip.sh
node --check mechanical_tv/static/app.js
```

The tests start a local HTTP server and generate their own short video. They do not touch GPIO or the real media library. Node is optional and used only for JavaScript syntax validation. GitHub Actions runs these checks on push and pull request.

## Deliberate limits

- One managed service with a background worker and FFmpeg child processes. The earlier three-service architecture is a future option, not this release's deployment.
- Maximum upload 256 MiB; maximum duration 10 minutes; maximum width and height 1920 pixels each. A 512 MiB disk reserve plus preparation headroom is enforced.
- Conversion shows queued/preparing/ready/failed states, not a precise preparation percentage. Upload progress is measured.
- Fixed profile: 32 × 25 at 10 fps. No hardware timing guarantees, RGB, audio, calibration, or motor controls. HDMI capture requires Pi/device validation.
- No automatic network wizard, browser shutdown, operator-control lease, automatic updater, or resumable uploads yet.
- Signed-in browsers share one player; latest command wins. Closing a browser does not stop playback. Restarting the service returns to idle.
- HTTP is intended for a trusted local network only. Do not port-forward this server to the internet. HTTPS and hardened public hosting are outside this release.

## Attribution and licensing

Mechanical concept and reference: [bitluni/MechanicalTV](https://github.com/bitluni/MechanicalTV), including its ESP32-S3 firmware and models. This repository contains independently written Pi application code; it does not include bitluni's firmware, STL files, or sample video content. Review upstream permissions before redistributing upstream assets.

No software license has been selected for this repository yet. The repository owner should choose one before inviting reuse or distributing licensed releases. Being visible on GitHub is not itself an open-source license.
