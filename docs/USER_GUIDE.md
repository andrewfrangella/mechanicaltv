# User guide

## Everyday use

1. Power on the Pi. The console opens full screen with the desktop.
2. Connect the HDMI source (the MacBook). Within a few seconds the Mac lists a display named
   MechanicalTV and the status line reads `LIVE`.
3. Press **Spin up motor** (or Space). The motor eases up to the target speed over about 5 s; the
   motor pill goes RAMPING, RUNNING, then LOCKED. Once locked, the picture holds still.
4. Press **Stop motor** when done. Quit with Ctrl+Q, then shut down the Pi from the desktop menu
   before removing power. Logging out or shutting down with the console open also stops the motor
   and blanks the LED.

## First-time calibration

1. **Check microstepping** once after wiring the motor driver (see [LOCAL_APP.md](LOCAL_APP.md)).
2. Select **Test Pattern: Aperture Grid & Crosshair** and spin up.
3. Move **Phase offset** until the frame is centred in the viewing window. Twenty-five pixels is one
   hole; the label shows degrees too.
4. If the picture is mirrored, tick **Invert horizontal** and/or **Invert vertical**.
5. Use **SMPTE Grayscale Bars** to set **Gamma** and **Contrast** so all eight bars are distinct, and
   **LED brightness** to taste.
6. **Sweeping Column** and **Sweeping Row** confirm the scan order; **Rotating Phase Sync Bar** shows
   any wobble.

Calibration is not saved between runs yet. Note the values for next time.

## Video input

| Choice | What it shows |
| --- | --- |
| Live HDMI Video | The HDMI adapter's picture, letterboxed (`MTV_HDMI_FIT=crop` fills instead). No signal shows black, never a stale frame |
| Test patterns | SMPTE bars, horizontal and vertical ramps, aperture grid, rotating bar, 1 Hz pulse, checkerboard, sweeping column, sweeping row, moving diagonal |
| Selected Library Clip | The clip chosen with Show on disk, looping at 10 frames per second |

The HDMI status line reads `LIVE`, `BLACK PICTURE (peak n/255)`, `WAITING FOR FRAMES` or
`NO SIGNAL`, with the reason underneath. Unplugging or sleeping the source blanks the disk within
1 s. The app reopens the device by itself when the source returns.

## Motor and speed

Speed runs from 1 to 24 FPS (60 to 1,440 RPM). Above about 20 FPS a NEMA 17 may stall with a printed
disk, and each pixel slot shrinks to 62.5 µs at 20 FPS and 52 µs at 24 FPS. Change speed while running and the motor eases to
the new speed.

**Emergency blank** turns off the LED, stops the motor and leaves free-run. Stop releases the coils,
so the disk coasts down.

## Media library

- **Add video file…** copies an MP4, MOV, MKV, WebM or AVI file into the library and queues its
  conversion to 32 × 25 grayscale at 10 fps. Audio is ignored.
- Limits are 256 MiB, 10 minutes and 1920 pixels on either side, at least 512 MiB of free space
  left, and at most three unfinished jobs.
- Conversion runs only while the motor is stopped, so it never competes with the disk.
- States are QUEUED, PREPARING, READY, or FAILED with the reason. A job interrupted by quitting is
  marked failed; delete it and add it again.
- **Crop to fill** before adding a file fills the 32:25 frame from the centre instead of
  letterboxing.
- **Show on disk** selects a READY clip as the input. **Delete** removes it after confirmation.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| Motor buzzes and does not turn at start | Microstep pins (Check microstepping), Vref, heat, supply voltage, rim rubbing: [DIAGNOSIS.md](DIAGNOSIS.md) section 1 |
| Motor pill reads STALLED | The footer names the reason (no pulses, or wrong steps per revolution) |
| Sync never locks | Measured speed must be within 3 % of the target; read INDEX SENSOR (backend and pulses) and OPTO JITTER; the debug log's `index:` line every 5 s gives the kernel-measured period, jitter and steps per revolution |
| LED never lights, disk not fitted | Expected: frames are clocked by the index pulse. Use Free-run output for a bench test |
| SAFETY WATCHDOG reads TRIPPED | No index pulse for 350 ms: the disk stopped or the sensor lost the notch |
| HDMI says device not found | `dtoverlay=tc358743` missing, or the ribbon in the DISPLAY socket |
| HDMI says no signal | The source must show a second display named MechanicalTV; see INSTALL.md "If the Mac shows no display" |
| HDMI is LIVE but black | The source sends a black picture: wake it or move a window onto that display |
| Hardware note in the footer, SIMULATION pill | Real hardware failed at start; the debug log has the reason |
| Errors flashed by | F12, or `~/.local/share/mechanical-tv/logs/mechanical-tv.log` |
