# Local console (desktop app on the Pi)

Since version 0.2.1 the operator console runs on the Pi's own display as a desktop application:
`python3 -m mechanical_tv.app`. It drives the same engine as the web page (frame pipeline, realtime
controller, library) directly in one process. Nothing an operator does passes through a socket, a
browser or a login, and the preview is painted straight from the controller's latest frame.

> The LED output path never went through the web page. Frames reach the disk through the capture
> backend, the frame pipeline and the index-sensor callback whether the web page or the console is in
> use. The console removes the browser round trip for *controls and the preview*. Capture latency is
> addressed separately: since 0.2.2 the CSI bridge is read in-process (`MTV_HDMI_BACKEND=auto`, see
> INSTALL.md) instead of through FFmpeg, which on a Pi 4 could not keep up with 1080p30 and ran
> seconds behind the source while using two cores.

## The two pictures

The left pane shows the **input as it arrives**: for Live HDMI a 320 × 250 colour preview of the whole
source picture (letterboxed, 15 frames per second on the V4L2 path, 10 on FFmpeg, drawn at whole-pixel
zoom so nothing is resampled), for patterns and library clips the 32 × 25 source frame. The right pane is always the 32 × 25 frame **as sent to the
disk**, after brightness, gamma, contrast and the spiral mapping, so the operator can compare what came
in with what the disk shows. The eyebrow label over the left pane changes to `HDMI INPUT · LIVE PICTURE`
while the preview is live.

## Starting it

`sudo bash install.sh` (default `MTV_UI=local`) installs a menu entry (Sound & Video > Mechanical TV)
and an autostart entry for the account that ran sudo, so the console opens full screen with the
desktop. By hand, from `/opt/mechanical-tv/current` or a clone:

```bash
python3 -m mechanical_tv.app --env-file /etc/mechanical-tv/environment            # window
python3 -m mechanical_tv.app --env-file /etc/mechanical-tv/environment --fullscreen
python3 -m mechanical_tv.app --mock-hardware                                       # simulation anywhere
python3 -m mechanical_tv.app --check                                               # report, no window
```

| Option / variable | Meaning |
| --- | --- |
| `--data DIR` / `MTV_DATA` | Library, settings and `logs/`. Default `~/.local/share/mechanical-tv`. A directory this user cannot write (the web service's `/var/lib/mechanical-tv`) falls back to the default with a log line |
| `--fullscreen` / `MTV_FULLSCREEN=1` | Start full screen. F11 toggles, Esc leaves |
| `--default-source auto\|hdmi\|pattern` | `auto` (default) selects Live HDMI when the capture device exists, else the SMPTE pattern. In simulation `auto` never opens a real device |
| `--free-run` | Start with free-run output on (see below) |
| `--debug-window` / `MTV_DEBUG=1` | Open the debug log window at start (F12 toggles it) |
| `--mock-hardware` / `MTV_MOCK_HARDWARE=1` | Simulated motor, sensor and LED |
| HDMI and pin options | Same names as the web service: `--hdmi-device`, `--hdmi-bridge`, `--hdmi-format`, `--hdmi-edid`, `--hdmi-fit`, `--hdmi-backend`, `--opto-pin`, `--led-pin`, `--spi-led`, `--motor-port` |

If real hardware fails to initialise (for example the PWM channel is missing), the console reports
the reason in the footer and the log, and runs in simulation so the interface can still be used.

Keys: **Space** spins the motor up or stops it (not while a text field has focus), **F11** full
screen, **F12** debug log, **Ctrl+Q** quit (stops the motor and blanks the LED first). Closing the
window does the same.

## Screen

- **Video input**: Live HDMI, the test patterns, or the selected library clip. The status next to it
  reports, for HDMI, the adapter type, resolution and frame rate and one of `LIVE`, `BLACK PICTURE
  (peak n/255)`, `WAITING FOR FRAMES` or `NO SIGNAL`, with the reason underneath.
- **Source video**: the 32 × 25 frame received from the input.
- **Mechanical TV image**: the same frame after gamma, contrast, brightness, phase offset and
  inversion, mapped back from the Nipkow pulse stream: what the disk shows.
- **Telemetry**: target and measured speed, sync lock, jitter, LED duty, watchdog, capture state,
  frames sent to the disk, CPU temperature, and the motor, sensor and LED drivers in use.
- **Motor & LED**: spin up, stop, emergency blank, speed (1 to 24 FPS, 60 to 1440 RPM), LED
  brightness, and **Free-run output**.
- **Nipkow disc calibration**: phase offset, gamma, contrast, flip X / flip Y.
- **Media library**: add a video file from local storage (USB stick, Downloads), convert it in the
  background, show it on the disk, delete it.
- **Header pills**: hardware mode, motor state, sync state (`LOCKED`, `UNLOCKED`, `FREE-RUN`), HDMI
  state and the log counter (`LOG: n ERRORS` turns red; click it for the debug window).

## Free-run output

With no disk fitted, or no index sensor, the LED never fires: the controller clocks every frame from
the sensor's once-per-revolution pulse and the stall watchdog blanks the LED when pulses stop.
**Free-run output** replaces that clock with an internal timer at the target speed. The LED pulses
800 pixels per period, the preview updates and the watchdog is fed by the timer, so the interlock is
bypassed. Index pulses, when present, still trim the motor speed but no longer clock frames.

Use it to prove the video path (capture, scaling, gamma, LED driver) on the bench. Do not leave it on
with a printed disk stopped under the LED: the 3 W LED runs at full duty with nothing moving. Emergency
blank, Ctrl+Q and closing the window all turn it off; stopping the motor does not.

The web interface has the same checkbox under the motor controls (`free_run` action of
`/api/realtime/control`).

## Debug log

F12 (or the LOG pill, or **Debug log** in the header) opens a separate window with every engine
message, operator action and error, errors in red, with an **Errors only** filter and a **Mark**
button that stamps a line so a test can be found later. The same lines go to a rotating file:

```
~/.local/share/mechanical-tv/logs/mechanical-tv.log     (1 MB × 5)
```

Captured there: HDMI capture state changes and FFmpeg exit messages, conversion worker errors,
every control action with its parameters, exceptions in actions, in the window's refresh loop,
in Tk callbacks and in background threads (with tracebacks), and the simulation fallback reason.
The web service writes the same file under its data directory (`/var/lib/mechanical-tv/logs/`)
in addition to journald.

## Hardware access without the service account

The console runs as the desktop user, so `install.sh` adds that account to `gpio`, `i2c`, `video`
and `spi` (log out and in once after the first install). Raspberry Pi OS's udev rules make the
hardware PWM channel group-writable for `gpio` members, so the A4988 driver can export and drive
it without root. The web service stays installed but disabled; `sudo MTV_UI=web bash install.sh`
switches back, disables the console autostart and re-enables the service. Never run both: they
would contend for `/dev/video0`, the GPIOs and the PWM channel.
