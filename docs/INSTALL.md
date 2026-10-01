# HDMI appliance installation

Use Raspberry Pi OS 64-bit with Python 3.11+, a Raspberry Pi 4 and its dedicated power supply. Install using a local keyboard/monitor or SSH over your existing network. Internet is required for package installation, not operation. No AP setup is part of this flow.

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

The installer creates an unprivileged service account, installs Python/FFmpeg/v4l-utils, grants video-device access, copies a versioned release, and enables `mechanical-tv.service`. It does not prompt for a web password or change networking/GPIO. Updating an older installation replaces the web service with capture and preserves its data and existing environment file; obsolete host/port settings are ignored. A manually configured AP is not removed: disable any old hotspot autoconnect from a local terminal or Ethernet connection if you previously configured one.

## Capture device preparation

Connect source HDMI → capture device HDMI input → capture device USB → Pi USB. The Pi's micro-HDMI ports are display outputs, not capture inputs. Confirm your device supports Linux V4L2; its exact model is still pending.

Stop capture while querying device capabilities:

```bash
sudo systemctl stop mechanical-tv
v4l2-ctl --list-devices
v4l2-ctl --device /dev/video0 --list-formats-ext
ls -l /dev/v4l/by-id/
```

Choose the video capture node (some devices expose additional metadata nodes). Prefer its stable `/dev/v4l/by-id/...` path over a changing `/dev/video0` number. Set `/etc/mechanical-tv/environment`:

```text
MTV_DATA=/var/lib/mechanical-tv
MTV_CAPTURE_DEVICE=/dev/video0
MTV_CAPTURE_FIT=fit
```

Optional `MTV_CAPTURE_FORMAT`, `MTV_CAPTURE_SIZE`, and `MTV_CAPTURE_RATE` select an advertised device mode, e.g. `mjpeg`, `1280x720`, `30`. These examples are not guaranteed for your adapter. Omitting them uses device defaults. Fit preserves the image with black padding; `crop` fills the output with a center crop. Output remains 32 × 25 grayscale at 10 fps. [FFmpeg V4L2 documentation](https://ffmpeg.org/ffmpeg-devices.html#video4linux2_002c-v4l2).

```bash
sudo systemctl restart mechanical-tv
systemctl status mechanical-tv --no-pager
journalctl -u mechanical-tv -n 100 --no-pager
sudo cat /var/lib/mechanical-tv/live/capture.json
```

`capturing` means complete frames were received, not that HDMI content or physical synchronization was verified. Some adapters emit black/color bars when HDMI is disconnected. `waiting` means capture failed/stalled and is retrying. Check device path, permissions, supported mode, cable and source. The service blanks the preview after a five-second frame stall or capture EOF. Adapter-generated no-signal frames cannot be distinguished generically.

Copy `/var/lib/mechanical-tv/live/latest.pgm` locally to inspect it in an image viewer. This file is replaced atomically and holds only the latest frame; no video is recorded. Reboot and test capture with networking disconnected. No browser or desktop session is required.

## Maintenance

Update with `git pull --ff-only` and `sudo bash install.sh` from the clone. Configuration and old library data are preserved. Installation checks service startup, not signal availability; inspect capture status separately. Previous release directories are retained for manual rollback. Stop the service before backing up `/var/lib/mechanical-tv` and `/etc/mechanical-tv`.

```bash
sudo systemctl stop mechanical-tv
sudo shutdown -h now
```

Wait for shutdown before removing power. To disable boot capture, use `sudo systemctl disable --now mechanical-tv`.

## Optional web development studio

The installed appliance does not start the web studio. From a development checkout, use `python3 -m mechanical_tv.server init --data ./studio-data`, then `python3 -m mechanical_tv.server serve --data ./studio-data`. Open `http://127.0.0.1:8080`. See [USER_GUIDE.md](USER_GUIDE.md) for this upload simulation tool. It has no live HDMI controls.
