# Installation on the Raspberry Pi

Use Raspberry Pi OS 64-bit (Debian Bookworm or newer) with Python 3.11+ on a Raspberry Pi 4 powered by its own official USB-C supply. Install over SSH or with a local keyboard and monitor. Internet is needed for package installation only.

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

## Interface

`install.sh` installs the **local console** by default (`MTV_UI=local`): the operator window runs on the Pi's display as the account that ran `sudo`, starts with the desktop, and the web service is disabled. `sudo MTV_UI=web bash install.sh` installs the password-protected web service on port 8080 instead and removes the console autostart. The two must not run together. Details of the console: [LOCAL_APP.md](LOCAL_APP.md). `MTV_AUTOSTART=0` skips the autostart entry (the desktop icon and menu entry are still installed).

The installer installs Python, FFmpeg, v4l-utils, lgpio, Tk and I2C tools, enables I2C, creates the unprivileged `mechanical-tv` service account (groups `i2c`, `gpio`, `video`), asks for the web application password on first install, copies a versioned release to `/opt/mechanical-tv/releases/` (symlinked from `/opt/mechanical-tv/current`), writes `/etc/mechanical-tv/environment` if it does not exist, writes the systemd drop-in `/etc/systemd/system/mechanical-tv.service.d/10-hardware.conf` for the motor driver, enables `mechanical-tv.service`, and checks `/healthz`. It prints a notice if the HDMI-to-CSI kernel overlay is missing from the boot configuration; that overlay is never added automatically, because the adapter cannot be detected before the overlay is loaded.

## Motor driver

`install.sh` defaults to an A4988-style STEP/DIR driver (A4988, DRV8825, TMC2209) clocked by the Pi's hardware PWM: STEP on GPIO 18, DIR on GPIO 23, ENABLE on GPIO 24, 16 microsteps. `MTV_DIR_GPIO`, `MTV_ENABLE_GPIO`, `MTV_MICROSTEPS` and `MTV_MOTOR_DIRECTION` in the environment file override these. On the first install it appends `dtparam=audio=off`, `dtoverlay=pwm,pin=18,func=2` and `gpio=24=op,dh` to the boot configuration (backup next to it as `config.txt.mechanical-tv.bak`), stops without starting the service, and asks for a reboot; the service starts automatically afterwards. Run `sudo MTV_MOTOR_DRIVER=hat bash install.sh` for the Adafruit DC & Stepper Motor HAT wiring described in [HARDWARE.md](HARDWARE.md); no boot settings are added in that case.

`MTV_MOTOR_DRIVER` is written to `/etc/mechanical-tv/environment` so the console and the service use the same driver.

In web mode, open `http://<pi-hostname>.local:8080` or `http://<pi-ip>:8080` and sign in with the application password.

## HDMI input

Two kinds of adapter work. Use the section that matches yours.

**When capture runs.** The local console selects Live HDMI at start whenever the capture device exists (`MTV_DEFAULT_SOURCE=pattern` to start on the test pattern instead). The web service behaves as follows: after every boot or service restart the input is the SMPTE test pattern. Live HDMI capture, including the EDID load that an HDMI source depends on to detect a display, starts when **Live HDMI Video (capture adapter)** is selected on the Realtime tab, and the choice is not remembered. To start capture at boot without a browser, set `MTV_DEFAULT_SOURCE=hdmi` in `/etc/mechanical-tv/environment` and restart the service.

### HDMI-to-CSI-2 adapter (GeeekPi/52Pi, Geekworm C790/X1301, Waveshare, Auvidea B101)

These boards use the Toshiba TC358743 and plug into the Pi's **CAMERA** ribbon connector, not USB. The Pi 4 has two identical 15-pin connectors: CAMERA sits between the micro-HDMI ports and the audio jack; DISPLAY sits next to the microSD end. Use the 15-pin cable (the 22-pin one is for CM4/Pi 5), contacts facing the right way at both ends. The red LED on the adapter only shows it is powered from the ribbon; it says nothing about the driver, the EDID or the signal.

1. **Enable the kernel overlay.** Edit `/boot/firmware/config.txt` (older images: `/boot/config.txt`). Change the existing `camera_auto_detect=1` line to `0`, add the overlay, and reboot.

   ```
   camera_auto_detect=0
   dtoverlay=tc358743
   ```

2. **Confirm the bridge was found.**

   ```bash
   sudo dmesg | grep -i tc358743    # expect a "tc358743 found" line
   v4l2-ctl --list-devices          # expect a unicam entry owning /dev/video0
   ```

3. **Select Live HDMI Video** in the web page (or set `MTV_DEFAULT_SOURCE=hdmi`). The service then does the rest itself:
   - loads the packaged EDID once per service run (`mechanical_tv/edid/1080p30.txt`: 1080p30 preferred, 720p60 alternate). Until an EDID is loaded the source sees no display, so a MacBook lists nothing in Displays and sends nothing;
   - before every open, waits for a source and locks its timings (`v4l2-ctl --set-dv-bt-timings query`), retrying every second;
   - captures UYVY straight from the V4L2 device in-process (`MTV_HDMI_BACKEND=auto`): four driver buffers, each frame handed back untouched except the 15 per second that are sampled with numpy into the 32 × 25 disk frame and a 320 × 250 colour preview. That costs about 8 ms per converted frame on a Pi 4 (roughly a tenth of one core) and holds at most one frame of delay. FFmpeg (10 fps, two cores at 1080p30, and a seconds-long backlog on a Pi 4) remains the fallback and the USB MJPEG path.

   Within a few seconds the Mac's Displays settings should list a display named **MechanicalTV** at 1920 × 1080, 30 Hz. If it does not, hold Option in Displays and click **Detect Displays**, then work through "If the Mac shows no display" below.

4. **Read the status line** next to the input selector (it wraps below it on narrow screens):
   - `HDMI /dev/video0 · CSI · 1920x1080 @ 30 FPS · LIVE`: frames are flowing.
   - `… · NO SIGNAL`: no source detected, or the device could not be opened. The Source Video panel shows the reason.
   - `… · WAITING FOR FRAMES`: source detected and timings locked, device open, no complete frame in the last 5 s. Normal for the first second or two; if it persists through repeated 5 s reopen cycles, run `journalctl -u mechanical-tv -f` and look for `V4L2 read error` or `ffmpeg exited` lines (a wrong `MTV_HDMI_FORMAT` is the usual cause on the FFmpeg path). The status line names the backend in use (`v4l2`, `ffmpeg` or `opencv`).

   The journal shows every transition with the prefix `HDMI capture:` (device opened, EDID loaded, no signal, ffmpeg exited, stall). The EDID path logged there is the release directory, `/opt/mechanical-tv/releases/<stamp>/mechanical_tv/edid/1080p30.txt`, which is what `/opt/mechanical-tv/current` points to.

**Limits.** The Pi 4 CSI port has two lanes. They carry 1080p at 30 fps (50 fps in UYVY) and 720p60, not 1080p60. The packaged EDID advertises only modes that fit and caps the pixel clock at 80 MHz so the source cannot pick 1080p60. If the source still negotiates something the link cannot carry, the journal says so.

#### If the Mac shows no display

Check in this order, with the service running and Live HDMI selected:

1. **Is capture running?** The status line must not read `ACTIVE PATTERN`. Live HDMI must be selected, or `MTV_DEFAULT_SOURCE=hdmi` set.
2. **Is v4l2-ctl installed?** The Source Video panel says `v4l2-ctl missing` if not: `sudo apt install v4l-utils`.
3. **What does the bridge see?** `sudo v4l2-ctl --log-status && sudo dmesg | tail -40` prints the chip state. `Cable detected (+5V power)` missing means the HDMI cable or USB-C adapter is not delivering +5 V; without it the bridge never raises hot-plug and the Mac never asks for the EDID. Try another USB-C-to-HDMI adapter or cable. `Hotplug enabled` with `No video detected` means the Mac sees the display but is not sending; re-detect displays or replug. `TMDS signal detected` and `Stable sync signal` mean video is arriving and the problem is downstream (format or FFmpeg).
4. **Is the EDID in the chip?** The journal must contain `HDMI capture: EDID … loaded`. If it shows `EDID load failed`, the device is probably not the bridge (a Pi camera also appears as unicam) or the ribbon is in the DISPLAY socket.

**Manual check** with the service stopped (the service holds the device while Live HDMI is selected):

```bash
sudo systemctl stop mechanical-tv
v4l2-ctl --set-edid=file=/opt/mechanical-tv/current/mechanical_tv/edid/1080p30.txt
v4l2-ctl --query-dv-timings
v4l2-ctl --set-dv-bt-timings query
v4l2-ctl -v pixelformat=UYVY
v4l2-ctl --stream-mmap --stream-count=30 --stream-to=/tmp/test.uyvy
sudo systemctl start mechanical-tv      # then re-select Live HDMI unless MTV_DEFAULT_SOURCE=hdmi
```

About EDID checksums: the shipped files already have correct checksums, so no checksum flag is needed. v4l-utils 1.22 (Bookworm) still accepts `--fix-edid-checksums` for files with bad checksums; newer v4l-utils releases fix checksums by default and dropped that flag (`--keep-edid-checksums` is its replacement with the opposite meaning), which is why it is reported as unknown there. Regenerate the files with `python3 packaging/make-edid.py`. Pi 5 (rp1-cfe) is detected but untested.

### USB HDMI capture dongle (MacroSilicon MS2109 class, UVC)

Plug it into a USB 3.0 port. No overlay or EDID is needed; the service opens it with FFmpeg in MJPEG. Confirm the node with `v4l2-ctl --list-devices` and prefer a stable `/dev/v4l/by-id/...` path in the settings below.

### Settings

`/etc/mechanical-tv/environment`, one `KEY=value` per line; restart the service after editing. Existing installations need no changes: every variable has a default.

| Variable | Default | Meaning |
| --- | --- | --- |
| `MTV_DEFAULT_SOURCE` | `pattern` | `hdmi` selects Live HDMI at service start, so capture and the EDID load run without a browser |
| `MTV_HDMI_DEVICE` | `/dev/video0` | V4L2 capture node |
| `MTV_HDMI_BRIDGE` | `auto` | `csi` (TC358743) or `usb` (UVC) if auto-detection from `/sys/class/video4linux` is wrong |
| `MTV_HDMI_FORMAT` | `auto` | FFmpeg `-input_format`: `uyvy422` for CSI, `mjpeg` for USB, `none` to let FFmpeg negotiate |
| `MTV_HDMI_EDID` | `auto` | EDID hex file for CSI bridges: the packaged 1080p30, `none` to skip, or an absolute path such as `/opt/mechanical-tv/current/mechanical_tv/edid/720p60.txt` |
| `MTV_HDMI_FIT` | `fit` | `fit` letterboxes the source; `crop` fills 32 × 25 from the centre |
| `MTV_HDMI_BACKEND` | `auto` | `auto` uses the in-process V4L2 reader (needs python3-numpy) whenever the input format is `uyvy422` and falls back to FFmpeg with a journal line if it cannot open the device; `v4l2` insists on it and reports failures instead; `ffmpeg` forces FFmpeg; `opencv` only if python3-opencv is installed |

The `doctor` command prints the detected adapter type, input format, EDID path and default source. The data directory is readable by root only, and the service's environment file is not read automatically, so run:

```bash
cd /opt/mechanical-tv/current && sudo python3 -m mechanical_tv.server doctor --data /var/lib/mechanical-tv --env-file /etc/mechanical-tv/environment
```

## Status and logs

Local console: the LOG pill / F12 window, and `~/.local/share/mechanical-tv/logs/mechanical-tv.log` (rotating, 1 MB × 5). `python3 -m mechanical_tv.app --check --env-file /etc/mechanical-tv/environment` prints the environment report without opening a window.

Web service:

```bash
systemctl status mechanical-tv --no-pager
journalctl -u mechanical-tv -n 100 --no-pager
curl -s http://127.0.0.1:8080/healthz
sudo tail -n 100 /var/lib/mechanical-tv/logs/mechanical-tv.log
```

The System tab of the web page lists FFmpeg, v4l2-ctl and the HDMI input; `/api/health` returns the same values as JSON once signed in.

## Maintenance

Update with `git pull --ff-only` followed by `sudo bash install.sh` from the clone. Settings, the password and library data are preserved; previous releases stay under `/opt/mechanical-tv/releases/` for manual rollback. To reset the password run `cd /opt/mechanical-tv/current && sudo python3 -m mechanical_tv.server reset-password --data /var/lib/mechanical-tv`, then restart the service.

```bash
sudo systemctl stop mechanical-tv
sudo shutdown -h now
```

Wait for shutdown before removing power. `sudo systemctl disable --now mechanical-tv` stops it from starting at boot.

## Development computer

```bash
python3 -m mechanical_tv.server init --data ./data
python3 -m mechanical_tv.server serve --data ./data --mock-hardware
```

Open `http://127.0.0.1:8080`. Motor, sensor and LED are emulated; Live HDMI reports the missing capture device.
