# Project plan and scope record

## Intended experience

Power on → receive HDMI through USB capture → convert continuously → preview latest frame → future synchronized mechanical output → shut down.

No access point, browser, login or upload is required. The original web upload studio is retained for development only. HDMI ingestion is now the primary software path; hardware output has not started.

## Implementation status

| Feature | Version 0.1.0 | Follow-up |
| --- | --- | --- |
| Password-protected local GUI | Implemented | HTTPS deployment if needed |
| Upload and persistent library | Implemented | Resume upload, thumbnails, rename |
| Fit / center crop | Implemented before upload | Interactive crop positioning and reprepare |
| 32 × 25 grayscale conversion | Implemented, fixed 10 fps | Profiles, gamma, contrast, orientation |
| Source and prepared previews | Implemented | Improved timeline synchronization |
| Play/pause/stop/seek/loop/brightness | Simulation implemented | Physical output adapter |
| Automatic startup | Installer and unit supplied | Validate on Pi OS |
| HDMI capture | Headless V4L2 conversion and retry implemented | Validate actual adapter and Pi |
| Health and diagnostics | Basic implementation | Throttling, memory, buffer/encoder telemetry |
| Self-test | CLI dependency check and automated tests | Browser-initiated full appliance self-test |
| Multiple browsers | Shared state; last command wins | Explicit operator ownership |
| Update/recovery | Installer preserves data and previous code | Release tooling and migration-aware rollback |
| Safe shutdown | OS/SSH instructions | Narrow privileged helper and UI action |
| Motor/LED/encoder integration | Not implemented | Hardware selection, wiring, measurement |
| Calibration page | Not implemented | Test patterns, phase, scan direction, limits |
| RGB, audio | Not implemented | After grayscale physical playback |

There is no fake motor speed readout, simulated “sync lock,” or working-looking hardware control in the UI. The simulation label remains visible.

## Milestones

1. **Local simulation:** validate authentication, uploads, real FFmpeg conversion, previews, controls, persistence and failure handling.
2. **Pi software acceptance:** run installer on the actual Pi, reboot, test offline HDMI capture, run an extended loop, verify temperature/resource behavior, test interrupted work.
3. **Hardware bring-up:** confirm component compatibility and wiring; test motor/index separately from LED; measure timing. Select a driver architecture based on evidence.
4. **Static optical image:** bars/gradient, scan direction, phase, brightness and distortion calibration.
5. **Physical grayscale video:** bounded buffering, measured synchronization, fault handling, realistic frame-rate/brightness limits.
6. **Optional features:** richer calibration, RGB, kiosk and appliance packaging.

## Decisions still open

- Pi-only timing versus a dedicated controller.
- Final motor driver and LED current-limiting circuit.
- Exact disk geometry and physical safe operating range.
- Hardware watchdog and loss-of-sync response.
- Whether physical Pause holds an image while the disk spins.
- Repository license, release policy, and distribution of any upstream assets.

The first release's reduced service structure and absent setup wizard are intentional documented scope changes from the initial concept, not completed features. Future work should update this table and validation evidence together.
