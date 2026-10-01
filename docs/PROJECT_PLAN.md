# Project Plan and Scope Record

## Intended Experience

Power on → join Wi-Fi → open local page → sign in → choose input (Live HDMI / Test Pattern / Library) → calibrate phase & optics → spin up motor → watch synchronized mechanical video → safe stop / shut down.

---

## Implementation Status

| Feature | Version 0.1.0 | Version 0.2.0 (Realtime First) | Follow-up |
| --- | --- | --- | --- |
| Password-protected local GUI | Implemented | Implemented | HTTPS deployment if requested |
| Live HDMI video capture | Not implemented | **Implemented (V4L2 `/dev/video0`)** | Auto-resolution negotiation |
| Procedural calibration patterns | Not implemented | **Implemented (SMPTE, Ramp, Grid, Rotating Bar, Pulse)** | Custom SVG/vector patterns |
| Nipkow coordinate serializer | Not implemented | **Implemented (bitluni spiral mapping $32 \times 25$)** | Custom disc spiral profiles |
| Motor control & ramping | Not implemented | **Implemented (Adafruit Motor HAT + NEMA 17)** | Direct STEP/DIR driver option |
| Closed-loop opto sync (PLL) | Not implemented | **Implemented (GPIO 4 index edge + speed trim)** | Microsecond hardware timestamping |
| High-power LED pulse modulation | Not implemented | **Implemented (GPIO 21 & SPI MOSI DMA)** | 3-channel RGB sequencing |
| Thermal & stall safety watchdog | Not implemented | **Implemented (350 ms timeout + stall cut)** | Hardware watchdog timer |
| Optical calibration & framing hold | Not implemented | **Implemented (phase offset, invert X/Y, gamma)** | Auto-centering via photodiode |
| Upload & persistent library | Implemented | Implemented | Resume upload, rename |
| Source and prepared previews | Implemented | **Implemented (Live HUD + TV canvas monitor)** | Low-latency WebRTC stream |
| Automatic systemd startup | Simulation unit | **Updated with hardware permissions (I2C/GPIO/V4L2)** | Validate on physical Pi OS |
| Diagnostics & Telemetry | Basic | **Full telemetry (RPM, target, sync lock, jitter, duty)** | Long-term jitter logging |

---

## Milestones

1. **Local Simulation (Completed):** Validated authentication, media preparation, previews, controls, persistence, and error recovery.
2. **Realtime Architecture & Drivers (Completed):**
   - Implemented V4L2 HDMI video ingest and procedural test patterns.
   - Built Adafruit Motor HAT (PCA9685/TB6612) driver with velocity ramping to 600 RPM.
   - Built optical index sensor edge timing and closed-loop PLL.
   - Built 800-pixel LED pulse modulator matching bitluni's geometry with thermal watchdog protection.
   - Updated web studio with real-time HUD telemetry, motor controls, and optical calibration.
3. **Physical Hardware Bring-Up (Current Step):**
   - Wire Adafruit Motor HAT, NEMA 17 motor, optical sensor, and L298N LED driver as specified in `docs/HARDWARE.md`.
   - Run installer on Raspberry Pi 4 Model B and verify I2C detection (`i2cdetect -y 1`).
   - Spin up motor with disk attached and observe velocity ramp to 600 RPM.
4. **Optical Calibration & Static Image:**
   - Display SMPTE bars and aperture grid test pattern.
   - Dial phase offset slider to center the frame in the aperture.
   - Verify scan direction (toggle Invert Horizontal/Vertical if necessary).
5. **Live HDMI Video Playback:**
   - Connect HDMI video source to USB capture card.
   - Select "Live HDMI Video" in web studio.
   - Enjoy real-time mechanical television display.
6. **Optional Enhancements:**
   - Sequential RGB 3-color flashing for color mechanical TV.
   - Hardware kiosk mode and automatic captive portal hotspot.
