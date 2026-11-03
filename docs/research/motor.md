# Stepper drive diagnosis: NEMA 17 (17HE08-1004S) + A4988 + 1.75 mm Nipkow disk

Date: 2026-11-03. Scope: why the motor stalls at spin-up after the disk was reprinted at 1.75 mm (about 80 g), what the A4988 current setting really is, whether the microstep wiring matches the software, what ramp the C++ port should use, and which bench checks settle each hypothesis. Repo read-only: the repository (Python version, main at 4c64391) (code cited as `path:line`). Disk STLs and the 3D-model README are in the other working copy, `3d_models/ (kept with the 3D model files) ` (untracked in git).

Legend used throughout: **FACT** = verified from a datasheet, kernel source, vendor page or the repo code. **ESTIMATE** = my calculation, arithmetic shown, inputs are FACTs or stated assumptions. **HYPOTHESIS** = unverified; a bench check is given for each.

---

## 0. Summary

1. **The motor is not short of torque in the steady sense.** Spinning the 1.75 mm disk up at the software's 2 rev/s² needs about 1 N·cm; air and bearing drag at 600 RPM are a few tenths of a N·cm. The motor holds 17 N·cm and, by a back-EMF/inductance estimate, still has about 9.7 N·cm at 600 RPM on 12 V (4.2 N·cm on 9 V, the full 17 N·cm on 24 V). Acceleration torque and torque-at-speed are both fine *if the driver actually delivers the rated 1 A*.

2. **What changed with the reprint is the dynamics, not the torque budget.** The disk's moment of inertia went from 2 663 g·cm² (bitluni's 0.6 mm disk, 30 g) to 7 611 g·cm² (1.75 mm, 81 g): 2.9× more, and 346× the rotor's 22 g·cm². The software starts the motor with an instantaneous jump from 0 to 0.5 rev/s (1 600 microsteps/s) at the very moment it enables the driver (`motor.py:722-735`, `motor.py:439-441`). That velocity step makes the rotor swing behind the commanded position; a nonlinear simulation with the real inertias (section 5) shows the swing stays inside the 3.6° loss-of-synchronism window with the light disk down to about 0.35 A of drive current, but with the 1.75 mm disk it needs at least about 0.65-0.7 A, and with the 2.4 mm disk essentially the full 1 A. Anything that trims the real current (wrong Vref, voltage-limited chopper, a driver in thermal shutdown) now stalls the start that used to work. `MTV_RAMP_RATE` does **not** help with this: it changes the slope after the jump, not the jump (`motor.py:722`, `motor.py:741`).

3. **"Pot all the way to the right" tells us nothing about the current until Vref is measured.** On most boards clockwise raises Vref, so "all the way right" is probably *maximum*: on a 0.1 Ω clone with 5 V logic that is roughly 2 A (above the motor's 1 A rating and at the A4988's limit), which overheats a bare driver into thermal shutdown within seconds; on a 0.05/0.068 Ω board it is well beyond the 2.1 A over-current trip. The A4988 then disables its outputs until it cools by 15 °C, restarts at the Home microstep with the disk coasting, and the motor stalls with a buzz. If the pot on that particular board is reversed, "all the way right" is *minimum* current and the result is the same stall. Either way the fix is the same: measure Vref and set it for 0.7-1.0 A (table in section 3).

4. **Microstep wiring is the third thing to rule out** before anything else is tuned: the software assumes 1/16 (`motor.py:365`, `install.sh` / `docs/INSTALL.md:21`), which on an A4988 means MS1=MS2=MS3 HIGH. The A4988 has internal pull-downs, so unconnected pins give full-step and every commanded speed is 16× too fast: the 0.5 rev/s "soft start" becomes 480 RPM from standstill.

5. **For the C++ port:** start from 0 rev/s (not 0.5), jerk-limited ramp, enable the driver and let the rotor settle before the first step, detect stalls by comparing index pulses with commanded revolutions, back off and retry (or catch the coasting flywheel), and run a microstep self-test at start-up. Hardware: set Vref by measurement, put a heatsink on the A4988, consider 24 V on the motor rail (A4988 is rated to 35 V; the L298N LED rail must then be separated or re-resistored), and check the rim-in-slot clearance.

Ranked hypotheses are in section 8; bench checks in section 10.

---

## 1. Motor: STEPPERONLINE 17HE08-1004S

**FACTS** from the manufacturer's one-page datasheet (downloaded 2026-11-03 from <https://files.omc-stepperonline.com/17HE08-1004S_Full_Datasheet.pdf>; the vendor's HTML product pages 403 for non-browser clients, the UK mirror <https://www.stepperonline.co.uk/e-series-nema-17-bipolar-1-8deg-17ncm-24-07oz-in-1a-42x42x23mm-4-wires-17he08-1004s.html> and <https://www.oyostepper.com/goods-987-E-Series-Nema-17-Stepper-Motor-Bipolar-18deg-17Ncm-2407ozin-1A-42x42x20mm-4-Wires.html> print the same table):

| Item | Datasheet value |
| --- | --- |
| Type | Bipolar, 4 leads, 1.8° ±5 % (200 full steps/rev) |
| Rated current | **1.00 A/phase** |
| Resistance/phase @25 °C | **3.60 Ω ±10 %** |
| Inductance/phase @1 kHz | **4.00 mH ±20 %** |
| Holding torque | **0.17 N·m** [1.42 lb-in] (= 17 N·cm, 24.07 oz·in) |
| Rotor inertia | **22.00 g·cm²** (= 2.2 × 10⁻⁶ kg·m²) |
| Body length | 23 mm max (42.3 × 42.3 mm frame) |
| Shaft | Ø5 mm (0/−0.012), 20 ± 1 mm long, D-flat 17 ± 0.25 mm long, 4.5 ± 0.1 mm across the flat |
| Loads | radial 21 N max at 20 mm shaft length, axial 10 N max |
| Insulation | class B, 130 °C; ambient −10 to 50 °C; temperature rise 80 °C max |
| Life test | "TMBF 6000 h or more @ 24 V / 300 RPM" |
| Torque-speed curve | **not in the datasheet** (single page: spec table, wiring, dimensions) |
| Weight | not printed (the table cell is blank) |

So `docs/HARDWARE.md:50` ("1 A/phase, 3.6 Ω, 4 mH") is correct, but the docs omit the two numbers that matter for the stall question: 17 N·cm holding torque and 22 g·cm² rotor inertia. Lead colours: the PDF text layer is scrambled; `docs/HARDWARE.md:76-77` (Black/Green = phase A, Red/Blue = phase B) matches STEPPERONLINE's usual convention but should be confirmed with an ohmmeter (3.6 Ω between the two wires of a phase, open circuit between phases).

**ESTIMATE, derived constants** (standard two-phase hybrid stepper model, e.g. Acarnley, *Stepping Motors: a guide to theory and practice*):
- Holding torque is specified with both phases at rated current, so the per-phase torque constant is K_t = T_hold / (√2 · I) = 0.17 / (1.414 × 1.0) = **0.120 N·m/A**.
- The back-EMF constant equals K_t in SI units: K_e = **0.120 V·s/rad** (mechanical). Peak phase back-EMF at 600 RPM (62.8 rad/s) = 0.120 × 62.8 = **7.5 V**. This is why 12 V is marginal at 600 RPM (section 2.3).
- Electrical time constant L/R = 4.0 / 3.6 = **1.1 ms**.
- A 1.8° motor has 50 rotor teeth (N = 50): one electrical cycle = 4 full steps = 7.2°. Torque vs. lag angle δ is T = T_max · sin(50 δ): peak at δ = 1.8° (one full step), zero and unstable at δ = 3.6° (two full steps). **Loss of synchronism = lag exceeding 3.6°.** Torque stiffness at the equilibrium: K_θ = 50 × T_max = 50 × 0.17 = **8.5 N·m/rad** at 1 A.

---

## 2. The load: inertia, torque budget, torque at speed

### 2.1 Moment of inertia (FACT from the meshes, with one assumption)

Computed with trimesh 3.23.5 directly from the printed STLs (script: `(working notes)/scripts/inertia.py`). Assumption: solid PLA at 1.24 g/cm³ (100 % infill as the README specifies; the README's "≈80 g" agrees).

| Disk | Mass | I about the axis | Radius of gyration | Ratio to rotor (22 g·cm²) |
| --- | --- | --- | --- | --- |
| bitluni original, 0.6 mm plate (`mechanical tv disk.stl`, mesh not watertight, ±few %) | 30.3 g | **2 663 g·cm²** = 2.66 × 10⁻⁴ kg·m² | 93.8 mm | 121 : 1 |
| **1.75 mm plate** (`nipkow_disk_1.75mm.stl`, current) | **81.4 g** | **7 611 g·cm²** = 7.61 × 10⁻⁴ kg·m² | 96.7 mm | **346 : 1** |
| 2.4 mm plate (`nipkow_disk_2.4mm.stl`) | 110.3 g | **10 400 g·cm²** = 1.04 × 10⁻³ kg·m² | 97.1 mm | 473 : 1 |

Sanity check (ESTIMATE): the mass sits mostly in the annulus outside the r = 15-70 mm cut-outs; a uniform annulus 70-125 mm of 80 g has I = ½·m·(r₁² + r₂²) = 0.5 × 0.080 × (0.0049 + 0.0156) = 8.2 × 10⁻⁴ kg·m², close to the mesh value.

Context (FACT): Oriental Motor's sizing guidance for open-loop steppers in 42-85 mm frames is a load/rotor inertia ratio of **10:1 or less**, and its FAQ gives **30:1** as the permissible maximum for all of its steppers ("if these values are exceeded, the motor may miss steps, stall, or vibrate") — <https://blog.orientalmotor.com/motor-sizing-basics-part-2-load-inertia>, <https://faq.orientalmotor.com/what-is-the-allowable-inertia-ratio-for-your-stepper-motors>. This build is at 346:1 (was 121:1). It can work — the load is pure inertia with no sudden external torque, and plenty of DIY Nipkow disks run this way — but it leaves no tolerance for velocity steps, resonance, or lost current, and every transient rings for a long time because there is almost no damping.

### 2.2 Torque to accelerate, and drag (ESTIMATE)

J_total = J_disk + J_rotor = 7.611 × 10⁻⁴ + 0.022 × 10⁻⁴ = 7.633 × 10⁻⁴ kg·m² (1.75 mm disk).

| Ramp | α | T_accel = J·α |
| --- | --- | --- |
| software default 2 rev/s² (`motor.py:23`) | 12.57 rad/s² | 7.633e-4 × 12.57 = 9.6 × 10⁻³ N·m = **0.96 N·cm** |
| documented "heavy disk" 0.5 rev/s² (`docs/INSTALL.md:106`) | 3.14 rad/s² | **0.24 N·cm** |
| 5 rev/s² (2 s spin-up) | 31.4 rad/s² | **2.4 N·cm** |
| 2.4 mm disk at 2 rev/s² | 12.57 rad/s² | 1.31 N·cm |

Air drag of a free disk at 600 RPM: Re = ω R²/ν = 62.8 × 0.125² / 1.5 × 10⁻⁵ = 65 000, laminar (transition is near 3 × 10⁵). Laminar free-disk moment coefficient (von Kármán/Cochran, both faces, Schlichting *Boundary-Layer Theory*): C_M = 3.87 / √Re = 0.0151. Torque = C_M · ½ ρ ω² R⁵ = 0.0151 × 0.5 × 1.2 × 62.8² × 0.125⁵ = 1.1 × 10⁻³ N·m = **0.11 N·cm**. The 32 holes, the notch and the spoke cut-outs make the real figure higher; a NEMA 17's bearings add about 0.1-0.3 N·cm. **Running load ≈ 0.3-0.6 N·cm**; ramp + running load < 1.6 N·cm at 2 rev/s². (I could not verify the 3.87 constant online this session; even a 5× error leaves drag under 1 N·cm, so the conclusion is insensitive to it.)

Stored energy at 600 RPM: ½ J ω² = 0.5 × 7.633e-4 × 62.8² = 1.5 J. With only air drag the disk would take roughly **40 s to coast from 600 to 300 RPM** (J/(k·ω) with k = M_air/ω²); bearing friction shortens that, but the point stands: after a stall or a driver dropout the disk is still turning for tens of seconds. That matters for restart logic (section 9).

### 2.3 Pull-out torque at speed: 9 V, 12 V, 24 V (ESTIMATE)

Model: a chopper driver is a current source until the supply can no longer push the sinusoidal phase current through the winding against inductance and back-EMF; beyond that "corner speed" the phase current is set by the phasor relation V² = (I·R + E)² + (I·X_L)², with E = K_e·ω_m (in phase with the current for maximum torque) and X_L = ω_e·L, ω_e = 50·ω_m. Torque ≈ T_hold × I/I_rated. This ignores iron losses, detent torque and chopper ripple, so treat the numbers as optimistic by perhaps 20-30 %; the trend is robust. (Script: `scratchpad/scripts/numbers.py`.)

At 600 RPM: ω_m = 62.8 rad/s, ω_e = 3 142 rad/s, X_L = 3 142 × 0.004 = 12.6 Ω, E = 7.5 V peak.

| Supply | Corner speed (full 1 A still reachable) | Current at 600 RPM | Pull-out torque at 600 RPM | At 450 RPM |
| --- | --- | --- | --- | --- |
| 9 V (bottom of the documented 9-12 V range, `docs/HARDWARE.md:55`) | **270 RPM** | 0.25 A | **≈ 4.2 N·cm** | 8.9 N·cm |
| 12 V | **399 RPM** | 0.57 A | **≈ 9.7 N·cm** | 14.7 N·cm |
| 24 V | **898 RPM** | 1.0 A (clamped by the chopper) | **17 N·cm (full)** | 17 N·cm |

Worked example at 12 V: (3.6 I + 7.55)² + (12.57 I)² = 144 → 171 I² + 54.4 I − 87 = 0 → I = 0.57 A → 0.57 × 17 = 9.7 N·cm.

**Answer to "acceleration torque or torque-at-speed?"**: at 12 V, neither is the binding constraint in steady state: 9.7 N·cm available versus about 1.5 N·cm needed is a 6× margin (at 24 V, 11×). At 9 V the margin shrinks to about 3× and the voltage-limited region starts at 270 RPM. The actual limiting factors are *dynamic*: (a) the instantaneous 0 → 0.5 rev/s velocity step at start (section 5.1), and (b) stability of a 346:1 inertia load in the voltage-limited ("constant power") region above the corner speed (section 5.2). Both are made worse by any shortfall in the real phase current (section 3).

---

## 3. A4988 current limit, trimpot, thermal shutdown

### 3.1 Formula and sense resistors (FACT)

- Allegro A4988 datasheet (read from the mirror <https://cdn.shopify.com/s/files/1/0903/0218/5736/files/A4988.pdf>; official product page <https://www.allegromicro.com/en/products/motor-drivers/brush-dc-motor-drivers/a4988>): "ITripMAX = VREF / (8 × RS)", VREF range 0-4 V, trip-level error ±5 % at the 70.71 % and 100 % steps, ±15 % at the 38.27 % step. Maximum rating on SENSE pins 0.5 V. Over-current protection threshold 2.1 A (min).
- Pololu A4988 carrier (<https://www.pololu.com/product/1182>): "all units manufactured since 2017 have 0.068 Ω current sense resistors (units made prior to this had green 0.050 Ω sense resistors)"; "can deliver up to approximately 1 A per phase without a heat sink or forced air flow (it is rated for 2 A per coil with sufficient additional cooling)"; the VREF pin "is accessible on a via that is circled on the bottom silkscreen"; in full-step mode "the current you are measuring is only 70% of the actual current limit setting". The pot is powered from VDD (Pololu forum staff, <https://forum.pololu.com/t/a4988-potentiometer-issue/19958>).
- Unbranded clones: sense resistors marked **R100** (0.1 Ω) are the most common; **R050** / **R068** (or "68L0") also exist; some carry R200 (0.2 Ω). Read the two small resistors next to the chip; their marking is the only reliable way to know which formula applies.

**Target for this motor**: 0.7-0.8 A for cool continuous running with margin, 1.0 A (the rating) as the maximum. Vref = 8 × I × R_S:

| Board / R_S | 0.7 A | 0.8 A | 1.0 A | What Vref = 1.0 V gives | What Vref = 1.6 V gives |
| --- | --- | --- | --- | --- | --- |
| Pololu pre-2017, green **0.050 Ω** (I = 2.5 × Vref) | 0.28 V | 0.32 V | 0.40 V | 2.5 A (over-current trip) | 4.0 A (trip) |
| Pololu 2017+, **0.068 Ω** (I = 1.84 × Vref) | 0.38 V | 0.44 V | 0.54 V | 1.84 A | 2.9 A (trip) |
| Clone **0.100 Ω** (I = 1.25 × Vref) | 0.56 V | 0.64 V | 0.80 V | 1.25 A | 2.0 A |
| Clone **0.200 Ω** (I = 0.625 × Vref) | 1.12 V | 1.28 V | 1.60 V | 0.62 A | 1.0 A |

Measure Vref between the pot wiper (or the VREF via) and GND with logic and motor supply on; the motor need not be connected. Note the current measured in full-step mode is 70.7 % of ITripMAX (datasheet Figure 9: both phases at 70.71 %), so do not set the limit by measuring coil current in full-step.

### 3.2 Which way is "up"? (FACT where cited, otherwise HYPOTHESIS)

- Several tutorials state clockwise raises Vref on Pololu-style boards (<https://zbotic.in/stepper-motor-current-setting-a4988-vref-calculation-guide/>; <https://www.the-diy-life.com/how-to-correctly-set-the-motor-current-limit-on-an-a4988-stepper-motor-driver>), one of them hedged as "typically". On the Pololu forum a user's walkthrough says "Turn the trim pot all the way counter clockwise (minimum value)" (<https://forum.pololu.com/t/a4983-and-a4988-getting-started-guide/2789?page=4>). Pololu's own product page gives no direction.
- Boards exist where the pot's end terminals are swapped (reverse sense); no primary source confirms a specific clone family, so this stays a HYPOTHESIS: **the direction is board-specific and must be verified with the meter while turning.**
- The pot's upper end depends on the logic supply the board gets: it is a divider from VDD. With VDD = 5 V, typical clones reach roughly 1.5-2 V; with the Pi's 3.3 V on VDD, proportionally less (about 1.0-1.3 V). ESTIMATE, board-dependent.

What "all the way to the right" could mean, numerically (ESTIMATE):
- 0.1 Ω clone, VDD 5 V, Vref ≈ 1.6-2 V → **2-2.5 A per phase**: twice the motor rating, at/over the A4988's 2 A rating and near the 2.1 A OCP threshold.
- 0.068 Ω or 0.05 Ω board at the same Vref → 3-5 A → the over-current protection trips immediately and repeatedly (the outputs are disabled until the fault clears, the translator is reset to Home).
- Reversed pot → Vref ≈ 0 → essentially **no torque**; the motor hums and does not turn or stalls at the first disturbance.

### 3.3 Thermal shutdown behaviour (FACT) and what it looks like (ESTIMATE)

Datasheet: TTSD 165 °C typical, hysteresis 15 °C. "In the event of a fault, overtemperature (excess TJ) or an undervoltage (on VCP), the FET outputs of the A4988 are disabled until the fault condition is removed." "When the device is powered down, or reset due to TSD or an over current event the translator is set to the home position." RθJA 32 °C/W on a JEDEC four-layer board; the breakout carriers are small two-layer boards, so 50-70 °C/W is realistic (ESTIMATE). RDS(on) 320 mΩ typical source and sink at 1.5 A, 25 °C.

Dissipation (ESTIMATE: two sinusoidal phases, I_rms = I_trip/√2, conduction through source + sink FETs with a 1.3× hot-RDS(on) factor, plus ~0.3 W switching/diode):

| I_trip | Dissipation | Temperature rise at 32 °C/W (JEDEC) … 60 °C/W (small carrier) | Outcome |
| --- | --- | --- | --- |
| 0.7 A | ≈ 0.7 W | 23 … 42 °C | fine bare |
| 1.0 A | ≈ 1.1 W | 36 … 68 °C | fine bare in a 25 °C room (matches Pololu's "about 1 A without heat sink") |
| 1.4 A | ≈ 1.9 W | 62 … 116 °C | heatsink needed |
| 2.0 A | ≈ 3.6 W | 116 … 218 °C | **bare driver reaches 165 °C within seconds** |

Sequence when the limit is set to ~2 A on a bare driver: the ramp starts at full current (at low speed the chopper regulates at the full I_trip, so dissipation is at its maximum during the first seconds of the ramp), the junction hits 165 °C after a few seconds, the outputs drop, the 7 600 g·cm² disk coasts, the junction cools 15 °C (a second or two), the outputs re-enable with the translator at Home while STEP pulses are still arriving at whatever rate the ramp has reached, and the motor cannot pull the coasting disk back into synchronism at that rate: **it stalls with a buzz, a few seconds after start — which is exactly "stalls when starting up even with the pot at max".** The software never notices: the watchdog only blanks the LED (`controller.py:252-255`) and the ramp carries on to 10 rev/s. Note also that at 600 RPM on 12 V the chopper is voltage-limited to ~0.57 A, so the driver runs *cooler at speed than during the ramp*: thermal cycling would preferentially bite during spin-up. (Whether this is happening is HYPOTHESIS until Vref and the chip temperature are measured; section 10 has the checks.)

### 3.4 Logic-level and pin facts that can also produce a "stall" (FACT)

- Logic inputs: VIN(1) ≥ 0.7 × VDD, VIN(0) ≤ 0.3 × VDD, VDD range 3.0-5.5 V. With VDD on 5 V the STEP/DIR/ENABLE threshold is 3.5 V, above the Pi's 3.3 V: **VDD must be wired to the Pi's 3.3 V pin, not 5 V**, or steps are recognised only by luck (hysteresis 11 % typical).
- RESET and SLEEP have no pull-ups on the chip; on carriers they are usually bridged. A floating RESET holds the translator reset ("All STEP inputs are ignored until the RESET input is set to high").
- STEP minimum high/low 1 µs each (`motor.py:430` enforces a ≥ 2 µs period at 50 % duty: fine). After leaving sleep, wait 1 ms before stepping.
- At power-on/enable the A4988 "drives the motor to the Home microstep position": the rotor snaps up to a full step when ENABLE goes low. The software enables the driver and starts the 1 600 step/s pulse train in the same call (`motor.py:439-441`), so the Home snap and the velocity step coincide.
- Decay: mixed decay (31.25 % fast) automatically on falling current; ROSC to GND forces mixed decay always, which the datasheet recommends when "microstepping at very low speeds" causes missed steps. Not a stall cause at 1 600 steps/s and above.

---

## 4. Microstep pin wiring

**FACTS**: A4988 truth table (datasheet Table 1): MS1/MS2/MS3 = LLL full, HLL half, LHL quarter, HHL eighth, **HHH sixteenth**. "The MS1 and MS3 pins have a 100 kΩ pull-down resistance, and the MS2 pin has a 50 kΩ pull-down resistance" — unconnected pins read LOW, so an unjumpered A4988 full-steps. The software's default is 16 microsteps (`motor.py:365-366`, `motor.py:371-372`, env `MTV_MICROSTEPS` at `motor.py:489`, documented at `docs/INSTALL.md:21`), and the step rate it commands is `steps_per_rev × fps` (`motor.py:728-731`). Other drivers the code names (`motor.py:357`) use different pins: DRV8825 M0/M1/M2 = HHL for 1/16 (and offers 1/32); TMC2209 MS1/MS2 = HH for 1/16 (LL is 1/8). A wrong jumper is a silent 2-16× speed error.

**ESTIMATE** (arithmetic) if configured 1/16 but wired otherwise:

| Actual mode | Commanded 0.5 rev/s start becomes | Commanded 10 rev/s becomes |
| --- | --- | --- |
| full step | **8 rev/s = 480 RPM from standstill → instant stall, loud buzz** | 9 600 RPM |
| half | 4 rev/s = 240 RPM at start → stall | 4 800 RPM |
| quarter | 2 rev/s = 120 RPM at start (may start, stalls on the ramp) | 2 400 RPM |
| eighth | 1 rev/s = 60 RPM at start | 1 200 RPM (stalls well before) |

**How the user can tell**
- *Sound:* at 1/16 and 1 600 µsteps/s the motor makes a smooth, fairly quiet whine and the disk visibly turns at 30 RPM (one turn per 2 s). In full-step with the same pulse train it rattles/buzzes hard and the disk does not turn, or jerks.
- *Count:* set the target to 1 FPS (60 RPM; the UI floor is 1 FPS, `controller.py:166`) and count revolutions of the notch against a clock for 10 s: 10 turns means 1/16 is right. Or compare the app's telemetry: `sensor.pulse_count` versus `motor.step_count / steps_per_rev` (`motor.py:470-472`, `sensor.py:287`) — the ratio must be 1.0.
- *Meter:* with power on, measure MS1, MS2, MS3 to GND: all three ≈ VDD (3.3 V) for 1/16; anything near 0 V is a wrong or missing jumper.

**What the software should do** (also section 9): make the microstep count a configuration item that the documentation ties to a jumper table per driver; verify it at start-up with a self-test that commands a pulse rate that is harmless in every mode and counts index pulses; refuse to spin up on a mismatch; and keep checking the ratio of index pulses to commanded revolutions while running.

---

## 5. The ramp, resonance and decay mode

### 5.1 The instantaneous 0 → 0.5 rev/s start (ESTIMATE, simulated)

Code (FACT): `_pwm_loop` starts with `cur_fps = 0.5` and immediately calls `set_rate(desired_rate(0.5))` (`motor.py:722-735`); `set_rate` enables the driver and starts the PWM in the same call (`motor.py:439-441`); the ramp then adds `ramp_rate × dt` every 20 ms (`motor.py:736-745`), with no S-curve. `MTV_RAMP_RATE` (`motor.py:23-42`, `docs/INSTALL.md:106`) scales the slope only; the 0.5 rev/s step is a constant.

Physics: a stepper with a pure inertia load is a mass-spring system (Gecko Drive, <https://www.geckodrive.com/?p=4419>: "the motor is highly resonant because it is a mass-spring system", mass = rotor and load inertia, spring = the magnetic restoring torque). Undamped natural frequency with the 1.75 mm disk: ω_n = √(K_θ/J) = √(8.5 / 7.633 × 10⁻⁴) = 105.5 rad/s → **f_n = 16.8 Hz** (0.6 mm disk: 28.3 Hz; 2.4 mm: 14.4 Hz; bare rotor: ~310 Hz). A velocity step Δω applied to an undamped oscillator at rest gives a lag swing of amplitude Δω/ω_n: 3.14 / 105.5 = 0.030 rad = **1.7°** (linear), versus 1.0° with the light disk. But 1.7° is already near the 1.8° torque peak, where the "spring" softens, so the real swing is larger. I integrated J·θ̈ = T_max·sin(50·(θ_cmd − θ)) − b·θ̇ with θ_cmd = ω₀t + ½αt² (script: `scratchpad/scripts/startsim.py`; b = 2 × 10⁻⁵ N·m·s, essentially undamped), flagging loss of synchronism when the lag passes 3.6°:

| Disk | T_max available at start (≈ current) | Peak lag | Result |
| --- | --- | --- | --- |
| 0.6 mm (2 663 g·cm²) | 17 N·cm (1.0 A) | 1.07° | OK |
| 0.6 mm | 6 N·cm (0.35 A) | 2.04° | OK |
| **1.75 mm (7 611 g·cm²)** | 17 N·cm (1.0 A) | 2.04° | OK |
| 1.75 mm | 12 N·cm (0.71 A) | 2.80° | OK, marginal |
| 1.75 mm | **10 N·cm (0.59 A)** | > 3.6° | **STALL** |
| 2.4 mm (10 400 g·cm²) | 17 N·cm (1.0 A) | 2.68° | OK, marginal |
| 2.4 mm | 14 N·cm (0.82 A) | > 3.6° | **STALL** |

And with the velocity step removed (ramp from standstill, 1.75 mm disk, only 10 N·cm available): 2 rev/s² → peak lag 0.22°; 5 rev/s² → 0.57°; 10 rev/s² → 1.2°; all fine. Starting with a small step then ramping (10 N·cm): 0.1 rev/s → 0.58°, 0.2 rev/s → 1.05°, 0.3 rev/s → 1.58°, 0.5 rev/s → stall.

Reading: **the reprint moved the instant start from "tolerates a 65 % current shortfall" to "needs at least ~65-70 % of rated current, with the Home snap and any rubbing on top".** It is the one mechanism that is fully consistent with "it worked with the 0.6 mm disk, stalls at start with the 1.75 mm one", and it is a software fix (start from 0). Caveats: the model has no electrical lag (at 25 Hz electrical the current follows easily, L/R = 1.1 ms) and no detent torque; real margins are somewhat worse than shown.

### 5.2 Mid-band (mid-frequency) instability (HYPOTHESIS, with ESTIMATEs)

FACT (Gecko Drive page above): the oscillation "takes from half a second to 10 seconds to build" and leads to a stall; it occurs in the constant-power region where the drive behaves as a voltage source; "it is possible to accelerate through the resonant region" but "not possible to operate the motor continuously in the speed band without mid-band resonance compensation"; the cure is "second-order, or viscous, damping". No manufacturer source I found gives a universal RPM band; practitioners quote roughly 5-15 rev/s (300-900 RPM) for small motors, while the 100-300 RPM figure in the brief is the *low-frequency* (primary) resonance region of typical unloaded NEMA 17s. Both move with inertia and supply voltage.

For this build (ESTIMATE): the constant-power region begins at the corner speed of section 2.3: **≥ 270 RPM at 9 V, ≥ 400 RPM at 12 V, ≥ 900 RPM at 24 V.** On 12 V the last third of the ramp and the 600 RPM operating point sit inside it, with a 346:1 inertia ratio and a PLA disk that provides essentially no viscous damping — the textbook recipe for a slowly building oscillation. On 24 V the whole 0-600 RPM range is in the current-regulated region, which removes the voltage-source phase lag that drives the instability. Primary (low-speed) resonance is not an issue here: with the disk, f_n = 16.8 Hz corresponds to a full-step rate of 17 steps/s (0.08 rev/s), far below any commanded speed, and 1/16 microstepping suppresses the step-ripple excitation.

Presentation: a stall several seconds after the ramp passes ~400 RPM, or at 600 RPM, after visible/audible hunting. Mitigation: 24 V; do not dwell between ~400 and 600 RPM; keep the ramp continuous; a little real damping (even the air drag of the holes helps); a driver with better current-loop behaviour (TMC2209 spreadCycle) or a closed-loop stepper.

### 5.3 Other ramp details

- The 20 ms rate updates (`motor.py:736`) are velocity steps of 0.04 rev/s at 2 rev/s² (lag swing 0.25/105.5 rad = 0.14°): harmless. GIL stalls that delay an update to 100 ms make 0.2 rev/s steps (0.7°): still fine. **Python timing is not the stall cause on the A4988 path**; the pulse train is in hardware.
- PLL slew limit (`motor.py:750-752`): up to 2 × ramp_rate × steps_per_rev × dt = 256 µsteps/s per 20 ms = 4 rev/s² equivalent → 3 N·cm at the 1.75 mm inertia; acceptable at 12 V (9.7 N·cm available), tight at 9 V. Trim clamp ±5 000 µs/rev (`motor.py:714`) is ±5 % of the period, far more than the lock window; the slew limit is what protects the motor.
- The PLL only acts above 80 % of target (`controller.py:203-209`) and is P-only with k_p = 0.05 (`controller.py:67`), so it cannot remove a steady offset; a steady 1 % speed error is 1 ms of period error, half the 2 ms jitter window of `sensor.py:189-196`.
- Decay mode: at 1 600-32 000 µsteps/s with tOFF = 30 µs the A4988's automatic mixed decay is appropriate; forcing 100 % mixed decay (ROSC to GND) only matters for very slow microstepping. Not a stall cause.
- PWM period quantisation (FACT, kernel): `drivers/pwm/pwm-bcm2835.c:67` sets the period to `DIV_ROUND_CLOSEST_ULL(period_ns × clock_rate, 1e9)` clock cycles. The step period at 600 RPM is 31 250 ns; with a 100 MHz PWM clock that is 3 125 cycles (0.03 % speed resolution), with a 10 MHz clock 312.5 cycles (0.3 % resolution, which the P-only PLL cannot settle between). The overlay's `clock` parameter is "informational" and "the clock must have been enabled and configured by other means" (<https://github.com/raspberrypi/linux/blob/rpi-6.12.y/arch/arm/boot/dts/overlays/README>, entry `pwm`). The actual rate on this Pi is unverified: `vcgencmd measure_clock pwm` reports it (section 10).

### 5.4 Recommended ramp profile for 80 g at 125 mm (ESTIMATE)

- First STEP only after ENABLE has been low for ≥ 200 ms (Home snap settles; the datasheet asks 1 ms after sleep, the rotor ring-down needs far more).
- Start at 0 rev/s; accelerate at **2 rev/s²** by default (5 s to 10 rev/s, torque 0.96 N·cm), configurable 0.25-6 rev/s². Jerk-limit it: ramp the acceleration in over 0.25-0.5 s (S-curve) so the lag never sees a step larger than ~0.3°.
- Update the step rate every 2-5 ms (a 50 Hz staircase is fine, a 200-500 Hz one is cleaner), computing the *actually achievable* period from the PWM clock.
- Do not pause between ~300 and 600 RPM; if the target changes mid-ramp, re-plan the profile continuously.
- At 12 V the budget at 600 RPM is ~9.7 N·cm against ~1.5 N·cm needed; at 9 V it is 4.2 N·cm, so hold the supply at 12 V minimum, preferably 24 V.

---

## 6. Mechanical

**FACTS**
- The rim passes through the slotted interrupter; the repo's rule is plate ≤ slot − 1.5 mm (`3d_models/ (kept with the 3D model files) README.md`, "Before printing: check the index sensor slot"), and the README's own table says the 1.75 mm plate leaves "only 0.675 mm per side" in a 3.1 mm slot. Slot widths of common parts: Omron EE-SX671 **5 mm** (<https://www.fa.omron.co.jp/product/item/EE-SX671/en/pdf/>); Vishay TCST2103 **3.1 mm** per distributor data (<https://uk.farnell.com/vishay/tcst2103/sensor-optical-phototransistor/dp/1470060>); Everlight ITR9608 **5.0 mm** (Everlight datasheet via <https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/Everlight-Elec-ITR9606_C264271.pdf> family); the "LM393 speed sensor" Arduino modules use 5-6 mm slots.
- The hub is a 5.0 mm bore pressed on a Ø5 h-tolerance shaft with a 4.5 mm D-flat (motor datasheet); no set screw. The user has broken two 0.6 mm disks pressing them on and off (memory note).

**ESTIMATES / HYPOTHESES**
- Axial run-out of a 250 mm printed disk on a press-fit cone hub of 0.5-1 mm at the rim is ordinary. In a 3.1 mm slot that is the entire clearance: the rim **rubs once per revolution**, adding a periodic drag torque and, if it catches, an outright stall; it also modulates the index timing. In a 5 mm slot (1.6 mm per side) it is a non-issue.
- Hub slip: the acceleration torque is only ~1 N·cm, which a sound press fit holds easily; but a cracked or widened bore (from the earlier breakages) slips under the Home snap or under a stall jolt, after which the notch no longer tells the truth about rotor position. Check with a pen mark across hub and shaft.
- Imbalance: 1 mm of mass eccentricity at 600 RPM is F = m·e·ω² = 0.08 × 0.001 × 3 944 = 0.32 N on the bearings (rated 21 N radial): mechanically harmless, but it shakes the sensor bracket and the Pi, and a disk that is badly out of plane also wobbles in the slot. Static-balance with tape on the rim if the motor visibly shakes.
- Thermal: a PLA disk near a 3 W LED on a star heatsink — PLA softens from ~60 °C; a warped rim both rubs and wobbles.

---

## 7. Supply voltage: what 24 V buys

FACT: A4988 VBB 8-35 V operating (datasheet); the 12 V cap in `docs/HARDWARE.md:81` applies to the Adafruit HAT's TB6612 and does not apply to the A4988 (the docs say so at `docs/HARDWARE.md:157`). The L298N LED board shares the 9-12 V rail (`docs/HARDWARE.md:93`); the LED's 2.7-3.3 Ω series resistor is sized for that rail.

ESTIMATE (section 2.3): raising the motor rail from 12 V to 24 V moves the corner speed from ~400 to ~900 RPM and the pull-out torque at 600 RPM from ~9.7 to the full 17 N·cm (×1.75), takes the whole operating range out of the voltage-limited region where mid-band instability lives, and makes the current rise time at each microstep 2× faster (better waveform fidelity at 32 kHz µsteps). Costs: slightly higher A4988 switching losses and ripple (minor), and the LED rail must be separated (keep the L298N/LED on 12 V, or recompute the resistor / use a constant-current LED driver). Going from 9 V to 12 V alone already doubles torque at 600 RPM (4.2 → 9.7 N·cm), so if the Minidodoca supply is sitting at 9 V, that is the first thing to turn up.

---

## 8. Ranked hypotheses for "stalls at start-up even with the pot at max"

| Rank | Hypothesis | Status | Why it fits | How it presents | Decisive check (section 10) |
| --- | --- | --- | --- | --- | --- |
| 1 | **Instant 0.5 rev/s start + 2.9× inertia + less than ~0.7 A of real drive** (`motor.py:722-735`, `439-441`) | ESTIMATE/sim | Only mechanism that turns a working light-disk start into a failing heavy-disk start without any wiring change; needs just a 30 % current shortfall or the Home snap on top | Stall in the first revolution, immediately on "Spin Up" | B1-B2 (Vref), B9 (hand-start test: nudge the disk to ~30 RPM before enabling; if it then ramps, this is it), B13 (temporary Python patch: `cur_fps = 0.05`) |
| 2 | **Current limit is not what the user thinks**: pot at maximum ≈ 2 A on a 0.1 Ω clone (bare driver → TSD cycling in seconds; outputs off, Home reset, stall), or beyond the 2.1 A OCP on 0.05/0.068 Ω boards, or pot reversed (≈ 0 A) | FACT (A4988/Pololu behaviour) + HYPOTHESIS (which case) | "All the way to the right" without a meter reading; A4988 is typically run bare on DIY builds; heavy disk makes any shortfall fatal (rank 1) | Over-current case: motor runs a few seconds then stops with a buzz, chip too hot to touch, repeats after cooling. Reversed case: hum, no rotation | B1, B2, B3, B4, B6 (heatsink + fan A/B test) |
| 3 | **MS pins not at 1/16** → 16× speed, 480 RPM from standstill | FACT (pull-downs) + HYPOTHESIS | The A4988 path is new; `docs/HARDWARE.md` still describes the HAT, so the jumper requirement may never have been met; a float gives full-step | Instant loud buzz, zero rotation, every time, independent of disk | B5 (MS pin voltages), B10 (count turns at 1 FPS) |
| 4 | **Rim rubbing in the sensor slot / wobble** (1.75 mm plate in a ~3 mm slot) | HYPOTHESIS | README itself flags 0.675 mm/side; printed disks wobble | Periodic scrape, stall when it catches; also perturbs index timing (lock/unlock) | B11 (slot width, run-out, free spin with driver released) |
| 5 | **Mid-band instability** above the corner speed (~400 RPM at 12 V, ~270 at 9 V) with 346:1 inertia and no damping | HYPOTHESIS (Gecko mechanism FACT) | Fits a stall late in the ramp or at 600 RPM after hunting; the lock/unlock symptom could be its onset | Stall 0.5-10 s after reaching ~400+ RPM, audible growl before | B12 (log commanded vs measured speed; stall speed repeatable?), B7 (24 V trial) |
| 6 | **Logic wiring**: VDD on 5 V (VIH 3.5 V > 3.3 V STEP), RESET/SLEEP floating, ENABLE polarity | FACT (thresholds) + HYPOTHESIS | Common A4988 mistakes; marginal thresholds give random missed steps | Erratic: sometimes starts, sometimes not; jittery | B4 |
| 7 | **Motor rail at 9 V or sagging** (Minidodoca adjustable 3-24 V; UVLO at 8 V; LED rail shared) | HYPOTHESIS | Docs allow 9 V; at 9 V torque at 600 RPM is ~4 N·cm and the voltage-limited region starts at 270 RPM | Stall late in ramp; worse when the LED is lit | B7 |
| 8 | Coil pairing wrong / hub slipping / cracked bore / imbalance | HYPOTHESIS | Would mostly have failed before the reprint too (except a bore damaged while pressing the new disk on) | Buzz without rotation (pairing); notch timing drifts vs. commanded (slip) | B8, B11 |

Not a cause of the start-up stall: Python's speed (the pulse train is hardware PWM; rate updates are 50 Hz and a late update is a 0.7° perturbation at worst). It remains relevant to the LED and sync quality, which is outside this report.

---

## 9. Recommendations for the C++ port (motor control)

Software, in priority order:

1. **Start from standstill.** First STEP at ≤ 0.05 rev/s (160 µsteps/s at 1/16) after ENABLE has been low for ≥ 200 ms; then a jerk-limited ramp (acceleration rising over 0.25-0.5 s to the configured α, default 2 rev/s², range 0.25-6). Never apply a velocity step larger than ~0.1 rev/s (lag < 0.6° at 10 N·cm). Replaces `motor.py:722-735`.
2. **Configurable start/accel/decel/jerk and supply-aware limits**, with the inertia and holding torque as named parameters so the limits can be reasoned about (defaults for this build: J = 7.6 × 10⁻⁴ kg·m², T_hold = 0.17 N·m).
3. **Stall detection from index pulses vs commanded revolutions.** Maintain commanded position by integrating the applied step rate (or count STEP edges with a second GPIO/PWM counter if available). Declare a stall when (a) commanded speed ≥ 0.3 rev/s and no index pulse arrives within 2× the commanded period after the first pulse has been seen, or (b) the measured period differs from the commanded one by > 8 % for 3 consecutive revolutions, or (c) index pulses / commanded revolutions drifts from 1.00 by > 2 % over 10 revolutions (catches wrong microstep settings and hub slip as well).
4. **Back-off and restart.** On stall: ENABLE high, blank the LED, log the commanded and measured speed at failure. Then either wait until the index period shows < 0.2 rev/s (the flywheel coasts for tens of seconds: do not restart blind) and retry with α halved, at most N retries; or "catch on the fly": measure the disk speed from the index period, enable the driver with the step rate set to that speed (within 2-3 %), and resume the ramp — the high inertia makes the capture reliable and saves the long coast-down. Report the failure speed in telemetry; a stall always at the same speed points to resonance or rubbing, one always at t = 0 points to the start or the current.
5. **Microstep/wiring self-test at start-up** (opt-out by config): with the driver enabled, command 200 µsteps/s for 20 s (0.0625 rev/s if 1/16, 1 rev/s if full-step: harmless in every mode) and count index pulses: 1 (±1) → 1/16, 2-3 → 1/8, 5 → 1/4, 10 → 1/2, 20 → full. Refuse to spin up on a mismatch and name the jumper table for the configured driver (A4988 HHH, DRV8825 HHL, TMC2209 HH). Zero pulses → sensor or driver fault, say so.
6. **PLL**: proportional-integral on the period error with anti-windup, acting only above ~80 % of target as now, with the slew limit retained; compute the real achievable period from the PWM clock (`pwm-bcm2835.c:67` rounding) and, if the resolution is coarser than ~0.05 %, dither between the two adjacent periods revolution by revolution. Verify the PWM clock at start-up (`vcgencmd measure_clock pwm`) and log it.
7. **Rate-update thread**: a real-time thread (SCHED_FIFO, CPU affinity) writing the sysfs period/duty (or the PWM registers through a kernel-sanctioned interface) at 2-5 ms; keep the write order that respects duty ≤ period (`motor.py:431-437` has it right).
8. **Diagnostics**: a CSV trace of commanded speed, applied step rate, measured period and lock state for the first 60 s after each spin-up, plus counters for stalls, retries, detected microstep mode, and the pulse/revolution ratio. This is the tool that distinguishes hypotheses 1, 2, 4 and 5 in one run.
9. **Driver-specific profiles**: if the hardware moves to a TMC2209, use its UART for run/hold current and StallGuard as a hardware stall input; the enable/home-snap wait and the self-test still apply.

Hardware checks and changes (details in section 10):

- Set Vref by measurement for 0.7-0.8 A (1.0 A max); identify R_S first (high priority).
- Heatsink and a little airflow on the A4988 regardless of the setting; or a TMC2209 (2 A, UART current setting, quieter) (medium).
- Motor rail 12 V now, 24 V when the LED rail has been separated; the A4988 accepts it, the Adafruit HAT does not (medium).
- Verify VDD = 3.3 V, RESET/SLEEP tied high, MS1-3 high, coil pairs by ohmmeter (high: five minutes with a meter).
- Measure the sensor slot and the rim run-out; use a 5 mm-slot interrupter if it is a 3.1 mm part (medium).
- Mechanically secure the disk hub to the D-shaft (flat-matched bore or a metal flange hub) and static-balance the disk (low).

---

## 10. Bench checks (what to measure, exact)

Power off between re-wiring steps; the A4988 dies if the motor is disconnected while powered.

- **B1 Sense resistors**: read the marking on the two small resistors beside the A4988 (R100 = 0.1 Ω, R068/68L0 = 0.068 Ω, R050 = 0.05 Ω, R200 = 0.2 Ω). Pick the formula row in section 3.1.
- **B2 Vref**: Pi on (so VDD is present), motor supply on, multimeter DC volts: black lead on a GND pin of the carrier, red lead on the pot's wiper (metal screw head on most clones) or Pololu's circled VREF via. Record the reading at the current pot position, then at both ends of travel. I = Vref / (8 R_S). Target 0.56-0.64 V (0.1 Ω), 0.38-0.44 V (0.068 Ω), 0.28-0.32 V (0.05 Ω) for 0.7-0.8 A.
- **B3 Pot direction**: while watching the meter in B2, turn a quarter turn each way; note which way raises Vref. Do not assume.
- **B4 Logic wiring**: measure VDD pin to GND = 3.3 V (not 5 V); RESET and SLEEP pins = VDD (bridged or jumpered); ENABLE pin = 3.3 V while the app is idle and 0 V after "Spin Up" (`gpio=24=op,dh` in `install.sh:81` and `motor.py:383`, `440`); STEP pin shows ~1.65 V average (50 % duty) when running. `pinctrl get 18,23,24` (or `raspi-gpio get 18,23,24`) shows GPIO 18 as `a5`/PWM0, 23 and 24 as outputs.
- **B5 Microstep pins**: with power on, MS1, MS2, MS3 to GND must each read ≈ 3.3 V. Any ≈ 0 V = that jumper is missing.
- **B6 Thermal A/B**: run the spin-up with a finger near (not on) the A4988 or an IR thermometer; note the seconds to the stall and whether the chip is painfully hot. Repeat with a clip-on heatsink and a desk fan on the driver. If the stall time lengthens or vanishes, it is thermal shutdown (hypothesis 2); fix Vref anyway.
- **B7 Supply**: set the Minidodoca to 12.0 V (then 24 V later with the LED rail separated). Measure VMOT at the A4988's VMOT/GND pins *during* spin-up with the LED lit: it must stay > 8.5 V with no dips (UVLO at 8 V). Confirm a ≥ 47 µF electrolytic across VMOT/GND at the driver.
- **B8 Coil pairs**: motor disconnected, ohmmeter across pairs: 3.6 Ω (±10 %) for Black-Green and Red-Blue, open across any other pair. Each pair goes to one driver output pair (1A/1B, 2A/2B).
- **B9 Start transient test (hypothesis 1)**: with the current set per B2, start the app normally and observe whether the stall is instantaneous (first revolution) or after seconds. Then: flick the disk by hand to roughly 30 RPM (half a turn per second) and press "Spin Up" while it turns; if it now ramps up and locks, the instantaneous 0 → 0.5 rev/s start is the problem.
- **B10 Count turns**: set target 1 FPS; count notch passes in 10 s against a stopwatch; 10 ± 1 = 1/16 correct. Or read `motor.step_count / steps_per_rev` against `sensor.pulse_count` in the status JSON after a minute; ratio must be 1.00.
- **B11 Mechanical**: measure the interrupter slot with calipers; with ENABLE high (motor released), turn the disk slowly by hand through a full turn and watch the rim in the slot and listen for scraping; measure run-out at the rim with a feeler gauge against a fixed reference through a full turn (> 0.6 mm at the rim with a 3.1 mm slot is rubbing territory). Mark hub and shaft with a pen and check the mark after a stall.
- **B12 Trace a spin-up**: `journalctl -u mechanical-tv -f` or the console's F12 log while polling the status endpoint once per 100 ms (`curl -s http://127.0.0.1:8080/api/status` on the web build) into a file; plot `motor.current_fps` versus `sensor.measured_fps`. The speed at which they diverge (t = 0 / ~400-600 RPM / random) separates hypotheses 1, 5 and 2/4.
- **B13 Temporary Python experiment** (not a repo change, on the Pi only): set `cur_fps = 0.05` at `motor.py:722` and insert `time.sleep(0.3)` between the driver enable and the first `set_rate`. If the spin-up then succeeds with the same Vref, hypothesis 1 is confirmed and the C++ ramp spec in section 9 is validated.
- **B14 PWM clock**: `vcgencmd measure_clock pwm`; and while running, `cat /sys/class/pwm/pwmchip0/pwm0/period` (31 250 at 10 FPS/1/16). `sudo cat /sys/kernel/debug/clk/clk_summary | grep -i pwm` shows the kernel's view. Also `uname -r` and `cat /etc/os-release` to settle the OS/kernel version assumption in the brief.
- **B15 OS/version**: `uname -r; cat /etc/os-release | head -3; v4l2-ctl --version; python3 -c "import RPi.GPIO,sys;print(RPi.GPIO.VERSION)"` — not motor-specific, but the brief asks for the kernel/OS to be verified rather than assumed.

---

## Sources

Primary
- STEPPERONLINE 17HE08-1004S datasheet (PDF, 1 page): <https://files.omc-stepperonline.com/17HE08-1004S_Full_Datasheet.pdf>; product pages <https://www.stepperonline.co.uk/e-series-nema-17-bipolar-1-8deg-17ncm-24-07oz-in-1a-42x42x23mm-4-wires-17he08-1004s.html>, <https://www.oyostepper.com/goods-987-E-Series-Nema-17-Stepper-Motor-Bipolar-18deg-17Ncm-2407ozin-1A-42x42x20mm-4-Wires.html>
- Allegro A4988 datasheet (mirror read: <https://cdn.shopify.com/s/files/1/0903/0218/5736/files/A4988.pdf>; product page <https://www.allegromicro.com/en/products/motor-drivers/brush-dc-motor-drivers/a4988>): ITripMAX formula, Table 1, electrical characteristics (VBB, VDD, VIN, RMS1-3, tOFF, VREF, OCP, TTSD, hysteresis, RθJA), Shutdown and Sleep paragraphs, mixed decay.
- Pololu A4988 carrier page: <https://www.pololu.com/product/1182>; Pololu forum threads <https://forum.pololu.com/t/a4988-potentiometer-issue/19958>, <https://forum.pololu.com/t/a4983-and-a4988-getting-started-guide/2789?page=4>
- Raspberry Pi kernel: `arch/arm/boot/dts/overlays/README` (pwm entry) and `arch/arm/boot/dts/overlays/pwm-overlay.dts`, `drivers/pwm/pwm-bcm2835.c` (rpi-6.12.y): <https://github.com/raspberrypi/linux/tree/rpi-6.12.y>
- Gecko Drive on mid-band resonance: <https://www.geckodrive.com/?p=4419>
- Oriental Motor inertia-ratio guidance: <https://blog.orientalmotor.com/motor-sizing-basics-part-2-load-inertia>, <https://faq.orientalmotor.com/what-is-the-allowable-inertia-ratio-for-your-stepper-motors>
- Slot widths: Omron EE-SX671 <https://www.fa.omron.co.jp/product/item/EE-SX671/en/pdf/>; Vishay TCST2103 (distributor data) <https://uk.farnell.com/vishay/tcst2103/sensor-optical-phototransistor/dp/1470060>; Everlight ITR9608 family <https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/Everlight-Elec-ITR9606_C264271.pdf>
- Pot direction tutorials (secondary): <https://zbotic.in/stepper-motor-current-setting-a4988-vref-calculation-guide/>, <https://www.the-diy-life.com/how-to-correctly-set-the-motor-current-limit-on-an-a4988-stepper-motor-driver>
- Theory: P. P. Acarnley, *Stepping Motors: a guide to theory and practice* (IET), torque-angle and resonance; H. Schlichting, *Boundary-Layer Theory*, rotating-disk friction (laminar C_M = 3.87 Re^-1/2) — constants quoted from memory of the texts, not re-verified online this session.

Repo (read-only)
- `mechanical_tv/hardware/motor.py`: 23-24 (ramp default/clamp), 356-474 (`A4988PWMDriver`: 365-372 microsteps, 430 period clamp, 431-437 write order, 439-441 enable+start, 455-459 release), 486-491 env, 711-714 trim clamp, 716-755 (`_pwm_loop`: 722 start at 0.5, 734-735 first rate, 736 20 ms, 741-745 linear ramp, 750-752 slew)
- `mechanical_tv/hardware/controller.py`: 67 (k_p), 166 (1-24 FPS clamp), 203-209 (PLL gate), 246-249 (state), 252-255 (watchdog blanks LED only)
- `mechanical_tv/hardware/sensor.py`: 177 (20 ms debounce), 189-196 (lock: 3 %, 2 ms, ≥ 5 periods), 221-222 (RPi.GPIO bouncetime 20)
- `install.sh`: 36-47 (driver default), 72-90 (boot lines: audio off, pwm overlay, gpio 24 high), 156-160 (PWM export drop-in)
- `docs/HARDWARE.md`: 50 (motor spec), 55 (supply 9-12 V), 76-78 (coil colours), 81 (12 V caution, HAT), 123 (slot rule), 157 (ramp/Vref note); `docs/INSTALL.md`: 21 (A4988 defaults), 106 (`MTV_RAMP_RATE`)
- `3d_models/ (kept with the 3D model files) README.md` and `nipkow_disk_{1.75mm,2.4mm}.stl`; bitluni's `mechanical tv disk.stl` at a local copy of bitluni's Models/ folder

Scripts (reproducible): `(working notes)/scripts/inertia.py`, `startsim.py`, `numbers.py`; extracted datasheet text at `.../scratchpad/downloads/A4988.txt` and `.../scratchpad/downloads/stepperonline/ds.txt`.
