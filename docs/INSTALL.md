# Install and operate the HDMI appliance

The default appliance converts HDMI capture to a local preview. Motor, LED, encoder, and GPIO output remain disabled. No Wi-Fi AP or browser connection is part of operation.

## Prepare the Pi 4

Use Raspberry Pi OS 64-bit and Python 3.11+, with the Pi's dedicated supply. Lite is sufficient. A local keyboard/display or temporary Ethernet/SSH connection can be used for installation and maintenance. Internet is needed to download dependencies, not during capture. See [official Pi setup](https://www.raspberrypi.com/documentation/computers/getting-started.html).

Connect the HDMI source to the capture device's HDMI **input**, and its USB connector to the Pi. Do not connect the source to the Pi's micro-HDMI display outputs. The software expects a V4L2 USB capture device; a different capture interface needs confirmed driver support before use.

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

The installer installs Python, FFmpeg and v4l-utils, creates an unprivileged account, copies versioned application files, and enables `mechanical-tv.service`. No password or interactive terminal is needed. It changes no networking, hostname, GPIO, or I2C settings.

## Select the capture node and mode

```bash
v4l2-ctl --list-devices
ls -l /dev/v4l/by-id/
v4l2-ctl --device /dev/video0 --list-formats-ext
```

Some devices expose multiple nodes, including metadata rather than video. Choose the video capture node. Prefer the corresponding `/dev/v4l/by-id/...-video-index0` path when available, since `/dev/video0` numbering may change. Check the advertised format, size and frame rate. FFmpeg can also list V4L2 modes with `ffmpeg -f v4l2 -list_formats all -i /dev/video0` ([device documentation](https://ffmpeg.org/ffmpeg-devices.html#video4linux2_002c-v4l2)).

Edit `/etc/mechanical-tv/environment` to match an advertised mode:

```text
MTV_DATA=/var/lib/mechanical-tv
MTV_CAPTURE_DEVICE=/dev/video0
MTV_CAPTURE_SIZE=640x480
MTV_CAPTURE_FPS=30
MTV_CAPTURE_FORMAT=
MTV_CAPTURE_FIT=fit
```

The sample settings are starting defaults, not verified device capabilities. An empty format lets V4L2 negotiate; set `mjpeg` only if supported. Input fps is the capture mode; conversion output is fixed at 10 fps. `MTV_CAPTURE_FIT` accepts `fit` or `crop`.

```bash
sudo systemctl restart mechanical-tv
systemctl status mechanical-tv --no-pager
journalctl -u mechanical-tv -n 100 --no-pager
sudo cat /var/lib/mechanical-tv/capture/capture.json
```

`receiving` means complete converted frames are arriving. `waiting` means missing device, EOF, or a five-second frame stall; the preview is blanked and capture retries every two seconds. A device that supplies no-signal graphics may still report `receiving`. Service startup success is not capture health or physical-output validation.

The service joins the `video` group and allows video4linux character devices. It does not grant GPIO/I2C access. Verify the actual node permissions with `ls -l /dev/video0`. If your OS uses a different device group, adjust the service group through `systemctl edit mechanical-tv` rather than running capture as root.

Preview files are private to the service account under `capture/`: `latest.pgm` (32 × 25 PGM) and `capture.json`. Copy the image with appropriate privileges for inspection; there is no automatic local display or web viewer. Stop blanks the image. No frame archive is retained.

## Before hardware is assembled

```bash
python3 -m mechanical_tv.capture --test-pattern --frames 30 --data ./data
```

This explicit test mode bypasses USB and exercises real FFmpeg conversion. It is never used as an automatic fallback, and its success does not verify the capture device. For continuously updating previews omit `--frames`; exit with Ctrl-C.

## Existing web-server installations

Rerunning the installer replaces the main service with standalone HDMI capture and closes its web listener. Existing library files, studio credentials and environment settings are preserved; add capture settings as above. `MTV_HOST`/`MTV_PORT` have no effect on capture. The service no longer depends on network startup.

The old installer did not create AP profiles. If you previously followed the manual hotspot guide, use a local terminal or Ethernet to inspect `nmcli connection show` and disable the specific hotspot's autoconnect, then bring it down. The installer does not delete network profiles or interrupt an existing SSH connection.

The optional studio runs separately: initialize credentials if needed, then `python3 -m mechanical_tv.server serve --data ./data`. It binds to localhost by default, owns its own file player, and cannot control HDMI capture. See [studio user guide](USER_GUIDE.md).

## Maintenance and shutdown

Update from the cloned repository with `git pull --ff-only` and `sudo bash install.sh`. The installer preserves data and backs up the stopped library database. Its check verifies service startup only. Previous application directories are retained; backups of SQLite alone do not include media. A previous web release needs its previous service unit restored as well as its code to roll back operating mode.

Back up `/var/lib/mechanical-tv` and `/etc/mechanical-tv` with the service stopped. A different data directory needs an accompanying `ReadWritePaths` unit override.

```bash
sudo systemctl stop mechanical-tv
sudo shutdown -h now
```

Stop terminates capture and blanks its preview. Wait for OS shutdown before disconnecting power. To disable boot startup while preserving files: `sudo systemctl disable --now mechanical-tv`.
