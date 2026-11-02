> The Realtime tab (live HDMI, test patterns, motor, calibration) is the primary flow. This guide also covers the media library player. Adapter setup: [INSTALL.md](INSTALL.md#hdmi-input).

# User guide

## Everyday flow

1. Power on the Pi and allow it to boot.
2. Join the same network as the Pi.
3. Open the app's local address and sign in.
4. Open Library, choose a clip and framing, then Upload & prepare.
5. Wait for READY. Select the video.
6. Compare the source proxy with the prepared TV image. Press Play.
7. Stop when finished. Shut down the Pi through the OS before removing power.

Everything stays on the Pi. After an upload finishes, the laptop is no longer the media source. No internet or GitHub connection is required for playback.

## Live HDMI input

Select **Live HDMI Video (capture adapter)** in the Video Input selector on the Realtime tab. The selection is not remembered across a service restart or reboot (the input returns to the SMPTE pattern) unless the operator sets `MTV_DEFAULT_SOURCE=hdmi`; see INSTALL.md. The status line next to the selector (below it on narrow screens) reports the adapter type (CSI or USB), the input resolution and frame rate while frames flow as `LIVE`; `WAITING FOR FRAMES` while the device is open but no frame has arrived in the last 5 s; or `NO SIGNAL`, with the reason in the Source Video panel (device missing, no HDMI source detected, FFmpeg exit message). The Source Video panel shows the same 32 × 25 frame that is sent to the disk; there is no full-resolution browser preview of live HDMI. When frames stop, the last frame is held for at most 5 s and then the disk shows black, never a test pattern, while the service reopens the device every second. The selector follows the server, so a reload or another browser shows the active input.

## Player

The source panel shows a silent H.264 proxy, scaled for efficient browser playback. It is not an untouched full-resolution original. The TV panel shows the prepared grayscale frame, multiplied by the selected simulated brightness. At most about seven status samples per second are sent to this browser; the 10 fps software timeline can advance between samples. The preview is therefore not a physical timing instrument.

| Control | Behavior |
| --- | --- |
| Select | Loads a ready clip, resets position, does not start |
| Play | Starts or resumes the shared Pi-side timeline |
| Pause | Holds the current image and position |
| Stop | Resets position and blanks the simulated TV panel |
| Seek | Changes the position within the selected clip |
| Loop | Repeats at the end; when off, the player stops and blanks |
| Brightness | Scales simulated brightness from 0–100%; does not change an LED |

Closing the tab, signing out, or disconnecting Wi-Fi does not stop playback. Reconnecting reads the current server timeline. Restarting the service or Pi always returns to idle with no selection. All signed-in clients can control the same player; an operator lease is planned but not yet implemented.

In a future physical display, freezing an image may require continuing disk rotation. No physical pause/stop behavior is implemented here.

## Library and uploads

- Start with a short MP4/H.264 video. Audio is ignored.
- Maximum upload: 256 MiB. Maximum duration: 10 minutes.
- Maximum source dimensions: 1920 on either axis. Unknown/invalid duration is rejected.
- Fit preserves the entire image and adds black padding. Crop fills 32:25 from the center.
- One upload at a time, up to three unfinished jobs. One conversion runs at a time.
- An upload progress bar measures transfer only. Processing states do not estimate conversion percentage.
- New conversions wait while the player is playing. An already-running conversion is allowed to finish.
- Cancel applies during upload only; it cannot stop a preparation job once the Pi has accepted the file.

States: **UPLOADING → QUEUED → PREPARING → READY**, or **FAILED** with an explanation. Originals are stored under generated IDs, not user-controlled paths. Untrusted names are rendered as text.

Delete removes the original, preview, prepared frames, and metadata. Stop playback before deleting the selected clip. Wait for active conversion to finish before deleting that job. Changing fit/crop currently requires uploading again; in-place reprocessing is planned.

After a restart, queued jobs resume. Jobs interrupted during upload or preparation become failed rather than pretending to be ready. Delete and upload them again. Files marked ready are made available only after complete conversion outputs are written.

## System

The health page reports version, mode, worker liveness, dependency availability, storage, uptime, and a temperature reading when the OS exposes one. Temperature is the first available Linux thermal-zone reading, not guaranteed to identify the CPU on every development computer.

Download diagnostics returns these basic values as JSON. It does not include passwords, sessions, video contents, file names, or logs. Actual RPM, sync status, output-buffer underruns, and Pi throttling flags are not measured in this version.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Site unreachable | Confirm Wi-Fi/network, IP, port 8080, and service status over SSH |
| `.local` name fails | Use the Pi's IP address; mDNS varies by network/client |
| Login rejected | Use the application password, not Wi-Fi/OS credentials; reset using INSTALL.md |
| Too many login attempts | Wait one minute; rate limit is shared across clients |
| Preparation remains queued | Stop playback; wait for the preceding conversion |
| Preparation failed | Try a short MP4/H.264 clip within limits; inspect service logs |
| Upload rejected for space | Remove unwanted media; leave room for original, proxy, frames, and reserve |
| Page loses connection | Pi may still be playing. Reconnect; do not assume the display stopped |
| Source preview does not play | Try a browser with H.264 support; prepared TV preview is separate |
| Picture looks too small | Try Crop on a new upload; low resolution is intentional |
| HDMI status says device not found | USB dongle unplugged, or `dtoverlay=tc358743` missing for a CSI adapter; see INSTALL.md |
| HDMI status says no HDMI signal on the CSI bridge | The source must show a second display named MechanicalTV. Check the ribbon is in CAMERA, the overlay is loaded, the EDID line appeared in the journal, and follow "If the Mac shows no display" in INSTALL.md (`v4l2-ctl --log-status`) |
| HDMI status stays at WAITING FOR FRAMES | Timings locked but FFmpeg delivers nothing; read `journalctl -u mechanical-tv -f` for `ffmpeg exited` lines and check `MTV_HDMI_FORMAT` |
| Input is back on the test pattern after a restart | Expected: the selection is not persisted. Re-select Live HDMI or set `MTV_DEFAULT_SOURCE=hdmi` |
| HDMI status says ffmpeg exited | Read the quoted FFmpeg error; wrong `MTV_HDMI_FORMAT` is the usual cause (`uyvy422` for CSI, `mjpeg` for USB) |
| HDMI goes black for a moment | Frames stopped for 5 s (source slept or changed mode); the service reopens the device by itself |

RGB output and crop-position adjustment are roadmap features. Live HDMI, calibration, gamma, contrast and motor control are on the Realtime tab.
