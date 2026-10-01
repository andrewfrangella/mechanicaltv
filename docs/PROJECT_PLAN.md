# Project plan and scope record

## Intended experience

Power on → HDMI source into capture device → USB into Pi 4 → automatic grayscale conversion → future synchronized mechanical display.

No AP connection, browser login, upload, or network is needed to operate the primary flow. Installation and maintenance may use temporary network access. The optional upload studio remains useful for file-based development and runs independently.

## Implementation status

| Feature | Status | Next validation/work |
| --- | --- | --- |
| Standalone HDMI/V4L2 ingestion | Implemented | Confirm capture model, node, supported mode on Pi |
| 32 × 25, 10 fps gray8 conversion | Implemented, shared with file conversion | Measure sustained performance and latency |
| Fit / center crop | Implemented | Optical geometry calibration later |
| Latest-frame preview and local status | Implemented | Inspect source pattern on actual capture |
| Missing-device retry, EOF/stall blanking | Implemented | Unplug/replug and source-loss acceptance |
| Boot service without web/AP dependency | Supplied | Install and reboot Pi with networking disconnected |
| Upload studio and file library | Retained, separate manual tool | Existing regression tests |
| Motor/LED/encoder integration | Not implemented | Confirm components, electrical design and output architecture |
| Adafruit motor HAT | Listed existing part | Check winding-current strategy and measured speed capability |
| Hardware watchdog/emergency stop | Not implemented | Design before powered assembly |
| Calibration, RGB, audio | Pending | After grayscale optical output |

## Milestones

1. **HDMI software preparation:** generated-source conversion, bounded reads, retry/blanking tests, service and configuration docs.
2. **Actual Pi capture acceptance:** enumerate USB device; confirm advertised mode; inspect image; unplug/replug; reboot without network; measure 30-minute resource use and latency.
3. **Hardware bring-up:** confirm motor, HAT, LED circuit and index sensor; power/current design; separately validate motor/index and LED. Keep physical output disabled until a measured design exists.
4. **Static optical image:** synchronized controller and encoder feedback, phase/scan calibration, guarding and emergency stop.
5. **Live optical grayscale:** latest-frame handoff to the synchronized output controller, independent safety watchdog, measured speed and latency.
6. **Optional extensions:** calibration controls, RGB and richer local previews.

## Open decisions

- Exact HDMI capture model, Linux driver, format, size and rate.
- Pi-only timing versus dedicated controller; no GPIO timing is implemented.
- Motor drive strategy compatible with the supplied low-resistance stepper and Adafruit HAT.
- LED current-regulation circuit and sensor voltage/interface.
- Disk geometry, safe mechanical speed, watchdog and emergency stop.
- Repository license and upstream asset distribution.
