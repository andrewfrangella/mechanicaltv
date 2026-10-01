# Hardware record — integration pending

This file records the owner's supplied parts and the discussion so far. It is **not a verified wiring diagram or authorization to energize the assembly**. The primary software path is now standalone HDMI capture; motor and optical output remain pending.

| Item | Owner-supplied description |
| --- | --- |
| Main computer | Raspberry Pi 4 Model B kit, case, dedicated supply, HDMI output adapter |
| Mechanical reference | bitluni MechanicalTV design and printed models |
| Motor | STEPPERONLINE NEMA 17 pancake, 1 A, 17 Ncm, 1.8°, four leads, 23 mm body |
| Motor HAT | Adafruit DC & Stepper Motor HAT for Raspberry Pi — Mini Kit |
| LED switching board | L298N, identified by owner as used in the reference |
| Motor/lighting supply | Minidodoca adjustable 3–24 V, 3 A maximum, advertised 72 W |
| Light sources | 3 W white LEDs; 3 W RGB LEDs for a later stage |
| Feedback | Photoelectric index sensor; exact model pending |
| Other | Switches, potentiometers, wire, solder, black PLA, printer supplies |
| Primary source | Owner has an HDMI capture/input device; exact model and USB/V4L2 support pending |

## Findings to carry into integration

The motor description matches [STEPPERONLINE 17HE08-1004S](https://www.omc-stepperonline.com/e-series-nema-17-bipolar-1-8deg-17ncm-24-07oz-in-1a-42x42x23mm-4-wires-17he08-1004s): 1 A/phase, 3.6 Ω, 4 mH, 200 full steps/rev. Confirm the label; this is a likely identification, not a verified delivered part.

The [Adafruit HAT](https://learn.adafruit.com/adafruit-dc-and-stepper-motor-hat-for-raspberry-pi/powering-motors) is not an adjustable winding-current-regulating stepper driver. Its current rating is not an automatic current limit. This low-resistance motor needs an appropriate drive strategy; connecting the HAT/motor to 12 V by assumption is not acceptable, and the HAT is not a 24 V supply input. A current-regulated STEP/DIR driver is the recommended direction pending confirmation.

The supply's 3 A label describes maximum capacity, not winding current control. 72 W is 24 V × 3 A; it does not imply 72 W at every output voltage. Keep the Pi on its dedicated supply.

The [L298](https://www.st.com/en/motor-drivers/l298.html) can switch loads but does not itself provide the required LED current regulation. The complete reference LED circuit has not been verified. Exact LED forward voltage/current, current-limiting components, thermal mounting and modulation requirements remain open. A previous blanket instruction to replace it was premature; reproducing a working circuit requires all its supporting components, not just the board.

At one frame per revolution, 10 fps means 600 RPM. With a 200-step motor that means 2,000 full steps/s. These are arithmetic requirements, not a tested or safe operating point for the motor, driver or printed disk. Adafruit's `onestep()` example is a low-speed demonstration, not a television timing engine.

The [reference firmware](https://github.com/bitluni/MechanicalTV/blob/main/MechanicalTvESP32/MechanicalTvESP32.ino) targets ESP32-S3, uses 32 × 25 grayscale frames and dedicates timing work to a core separate from networking. It cannot run directly as a Pi OS program. The software here independently implements media preparation and simulated playback.

## Before physical integration

Confirm exact motor, LED, sensor, driver and supply models; obtain a complete circuit; decide output architecture; verify logic levels and power distribution; establish mechanical mounting, balance and guarding; measure encoder timing and LED modulation. The current software never accesses GPIO and must not be presented as proof of hardware compatibility.

References: [project repository](https://github.com/bitluni/MechanicalTV), [printed models](https://github.com/bitluni/MechanicalTV/tree/main/Models), [owner's video reference](https://www.youtube.com/watch?v=R-wbfP1pmVw&t=375s). Video-only details were not independently verified during this build.

## HDMI-first connection preparation

Prepare the signal chain as **source HDMI OUT → capture HDMI IN → capture USB → Pi 4 USB**. The Pi 4 micro-HDMI ports are outputs, not capture inputs ([Pi documentation](https://projects.raspberrypi.org/en/projects/raspberry-pi-getting-started/8)). Confirm whether the supplied input device is actually USB/UVC/V4L2; an HDMI adapter or other capture interface cannot be assumed to work with this path. Record its label/model, USB ID, video node and advertised capture modes before configuring it.

Software can be tested with `python3 -m mechanical_tv.capture --test-pattern` before assembly. Leave the motor supply, HAT motor outputs, LED circuit, and sensor disconnected during capture-only acceptance. No physical pin assignments have been selected and no I2C/GPIO access is enabled by the service.

For later HAT bring-up, document board revision/address, Pi header mounting/clearance, winding pairs, supply setting, and the current-management strategy first. Adafruit specifies 5–12 V motor power and recommends separate Pi/motor supplies ([power guide](https://learn.adafruit.com/adafruit-dc-and-stepper-motor-hat-for-raspberry-pi/powering-motors)). This does not establish compatibility with the listed 1 A/phase, low-resistance stepper. Do not energize it from the adjustable supply merely because its setting falls inside the HAT range. Retain the HAT as an available part until current and timing feasibility are measured.

Record sensor output voltage/type before any GPIO connection; the Pi interface needs a verified level-compatible circuit. Complete LED current regulation and thermal mounting before modulation tests. The future controller must consume latest grayscale frames independently of source timing and disk encoder timing. HDMI frame arrival is not an index pulse or speed feedback.
