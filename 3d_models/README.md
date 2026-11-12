# 3D models

## Nipkow disk, one piece, 1.75 mm plate

| File | Purpose |
| --- | --- |
| `nipkow_disk.scad` | Parametric source (OpenSCAD). Change `plate_thickness` here. |
| `nipkow_disk_1.75mm_clamp.stl` | **Current.** Ready to slice: flat face down, hub up, 250 mm diameter, 11.1 mm tall. Same disk as `nipkow_disk_1.75mm.stl` with the clamping hub (below). |
| `hub_test.stl` | Only the clamping hub and the plate out to r = 16 mm (5 g): print it first to check the fit on your motor shaft. |
| `nipkow_disk_1.75mm.stl` | Previous version with bitluni's press-fit cone hub and round 5 mm bore (wears when the disk is taken on and off). |
| `nipkow_disk_2.4mm.stl` | Earlier, thicker version of the same model (11.4 mm tall), kept as an alternative. |
| `nipkow_disk_2.4mm.3mf` | 3MF of the 2.4 mm disk. |
| `nipkow_disk_preview.png` | Render of the disk (the thickness is not visible at this scale). |

![Thick Nipkow disk](nipkow_disk_preview.png)

The geometry is bitluni's `mechanical tv disk.stl` ([bitluni/MechanicalTV](https://github.com/bitluni/MechanicalTV), `Models/`) rebuilt as a parametric model so the plate can be thicker. Everything the software and the index sensor depend on is unchanged, verified against the original STL:

| Feature | bitluni original | This model (1.75 mm) |
| --- | --- | --- |
| Plate thickness | 0.6 mm | **1.75 mm** (parameter) |
| Where the thickness goes | – | 0.575 mm added on each face, so the rim stays centred in the sensor slot |
| Image holes | 32 × 1.0 mm square, same spiral (r 84.5–115.5 mm, 11.25° per hole) | identical positions (within 0.001 mm) and size; opened out at 45° on both faces (1.95 mm at the surface, 1.0 mm through a 0.8 mm web) so the thicker plate does not narrow the light cone |
| Sync notch | 1.0 mm wide, r 120 mm to rim, at −168.75° | identical |
| Hub | cone r 5 → 10 mm, 10 mm long, plain 5.0 mm bore, pressed on | **clamping hub**, same length, see below. Everything outside r = 12 mm is identical to the previous file (0 mm³ difference) |
| Spokes and cutouts | two 10 mm spokes, cutouts r 15–70 mm | identical |
| Stiffener ring r 73–75 mm | 0.9 mm tall on the hub side | mostly absorbed into the plate; 0.3 mm of it still stands proud of the hub-side face. The hub side faces **away from the motor**, toward the screen holder |
| Mass at 100 % infill (PLA) | ≈30 g | ≈84 g with the clamping hub, ≈81 g with the press hub (2.4 mm version: ≈110 g) |

### Before printing: check the index sensor slot

The rim passes through the slotted opto-interrupter. The plate must be at least **1.5 mm thinner than the slot** (0.75 mm clearance per side):

| Sensor slot width | Maximum `plate_thickness` |
| --- | --- |
| 3.1 mm (TCST2103 class) | 1.6 mm. The 1.75 mm disk leaves only 0.675 mm per side, so check the slot first. |
| 5 mm (most Arduino/Pi slot modules) | 3.5 mm, so 1.75 mm (and 2.4 mm) are fine |
| 10 mm | 8.5 mm |

To regenerate at another thickness, open `nipkow_disk.scad` in OpenSCAD, change `plate_thickness`, press F6 and export an STL, or run (this is how `nipkow_disk_2.4mm.stl` was made):

```bash
/Applications/OpenSCAD-2021.01.app/Contents/MacOS/OpenSCAD \
    -o 3d_models/nipkow_disk_2.4mm.stl -D plate_thickness=2.4 3d_models/nipkow_disk.scad
```

Other parameters: `hole_web` (0 for plain straight holes), `hub_side_fraction` (1.0 puts all the extra thickness on the hub side, 0.0 all on the motor side), `hub_style` (`"press"` rebuilds bitluni's cone hub exactly), the clamping hub's dimensions (`bore_clearance`, `screw_z`, nut and screw sizes), `hub_test` (true renders only the hub test piece) and `print_orientation` (false gives bitluni's original frame).

### Print settings (Elegoo Centauri 2, 256 × 256 mm bed)

- Print the STL as exported: flat face on the bed, hub up. No supports needed: the hole flares and the hub's tip chamfer are 45°, the nut slot opens at the top, and the side screw hole is small enough to bridge.
- Centre it on the plate. There is only 3 mm of margin per side, so turn the brim off and keep any skirt within 2 mm of the part or turn it off too.
- Black filament (PLA or PETG) so the plate is opaque. 0.2 mm layers, 4 walls, 100 % infill (at 1.75 mm the part is all top and bottom layers anyway).
- The 1 mm holes print best with the slicer's thin-feature handling on (Arachne / "detect thin walls").
- The disk is about 2.8× heavier than the 0.6 mm one (≈84 g with the clamping hub). If the stepper stalls during the soft-start ramp, lower `ramp_rate_fps` (default 2.0 FPS per second) in `mechanical_tv/hardware/motor.py` rather than the target speed.

### Clamping hub

bitluni's hub is a round 5 mm bore pressed onto the shaft. Each time the disk comes off, the PLA bore wears a little until the motor slips. The new hub slides on and is clamped instead, so nothing is forced and nothing wears:

| | |
| --- | --- |
| Bore | D-shaped to match the 17HE08-1004S shaft (Ø5.0, 4.5 mm across the flat): Ø5.2 with the flat 2.1 mm from the centre, i.e. 0.1 mm clearance, 0.5 mm lead-in chamfer at both ends. The flat keys the disk to the shaft: with the screw loose it has about ±4° of play but cannot spin on the shaft |
| Screw | one radial M3 onto the shaft's flat, threaded into a **captive M3 nut** (plain, 5.5 × 2.4 mm; a nyloc nut is too thick). Use an **M3×8 set screw, cup point** (1.5 mm hex key; M3×6 also works), or an **M3×10 socket head** (2.5 mm key), which seats on the hub surface and stands about 3 mm proud. Shorter socket heads do not reach the shaft. Behind the nut is 4.7 mm of solid hub |
| Nut | drops into the slot in the hub's end face, next to the bore, and is held there by the screw. Do not unscrew more than about two turns or the nut can fall out |
| Position | screw axis 5.0 mm below the hub tip (4.3 mm from the disk's hub-side face). With base v2 it is 3.3 / 4.3 / 5.3 mm from the end of a 19 / 20 / 21 mm shaft, so it always lands on the flat. With bitluni's base the shaft passes right through the hub |
| Hub | Ø22 mm cylinder, same 11.1 mm overall height, about 2 g heavier than the cone |

**Fit test first.** Print `hub_test.stl` with the same filament and settings as the disk. Drop the nut in (it should fall to the bottom of its slot) and check the screw starts. Then try it on the bare motor shaft: it should slide on by hand, and once the screw is snug the test piece's edge should not wobble when you turn the shaft (0.1 mm of wobble at its edge is about 0.8 mm at the rim, against 1.1 mm of clearance at the sensor). If it will not go on, raise `bore_clearance` in `nipkow_disk.scad` to 0.15 or 0.2; if it still rocks with the screw snug, lower it to 0.05. Re-render the test piece, try again, then render the disk with the same file:

```bash
OPENSCAD=/Applications/OpenSCAD-2021.01.app/Contents/MacOS/OpenSCAD
$OPENSCAD -o 3d_models/hub_test.stl -D hub_test=true 3d_models/nipkow_disk.scad
$OPENSCAD -o 3d_models/nipkow_disk_1.75mm_clamp.stl 3d_models/nipkow_disk.scad
```

(about 15 s each). `-D 'hub_style="press"'` (note the quoting) rebuilds the old press-fit disk.

**Tightening.** Snug, not hard: about a quarter turn after the tip touches the flat, turning the key's short end between finger and thumb. The nut is steel, but the hub is plastic, and the motor's heat (50–70 °C at 1 A) reaches it through the shaft. Check the screw after the first long warm run. No thread-locker is needed; if the screw works loose, put a drop of medium (blue) on the screw threads only, kept off the plastic. PETG holds up better than PLA here.

**Taking the disk off.** On a bare motor: loosen the screw two turns and slide the disk off. In base v2 the screen holder and sensor seat block the axial path, so: disable the driver, turn the disk until the hub screw points up and loosen it two turns, remove the four motor screws through the disk's cut-outs (2.5 mm key), slide the motor straight back along the cradle while holding the hub, and lift the disk out of the gap. Refit with steps 4–7 of the base's assembly order.

**Another motor.** For a round shaft set `shaft_flat = shaft_d`. In base v2 the shaft needs to be at least about 18 mm long and its flat at least about 11 mm long.

## Base, version 2: thicker plates, motor support, wider disk gap

| File | Purpose |
| --- | --- |
| `base_v2.py` | Generator (Python, trimesh + manifold3d). Parameters at the top. Reads bitluni's `mechanical tv base.stl` from `~/Downloads/Existing Folders/MechanicalTV-main 2/Models/` (path in `ORIG`). |
| `base_v2_print.stl` | Ready to slice: spine (long bottom strip) on the bed, 176 × 40.5 × 63 mm. Copy in `~/Downloads/mechanical_tv_print_files/base_v2_print_ready.stl`. |
| `base_v2.stl` | Same part in bitluni's assembly frame, for checking fits. |

Built from bitluni's base, so every feature that was not deliberately changed is in exactly the same place. Checked in software (not yet on a printed part) against the 1.75 mm disk, a 17HE08-1004S motor envelope, bitluni's LED fixture and `mechanical_tv_deck_v3.stl`: no intersections.

| Feature | bitluni | Version 2 |
| --- | --- | --- |
| Disk gap, motor/LED side | face at z +2 | **z +4.5**: 3.6 mm from the 1.75 mm disk (was 1.1 mm) |
| Disk gap, screen side | face at z −2 | **z −2.5**: 1.6 mm from the disk face, 1.3 mm from its small stiffener ring. Kept close on purpose: a screen further from the disk gives a softer picture |
| Index sensor seat | pockets above and below the gap | **identical**, same place relative to the disk (0 mm³ difference). Where the rim passes the sensor it still has 1.1 mm per side to the seat's faces, as in bitluni's base, plus whatever the sensor's own slot allows. This is the tightest point and limits how much disk wobble is tolerated |
| LED plate | 7 mm | same plate and fixture pocket, moved 2.5 mm away from the disk |
| Screen holder | 4 mm | same plate, window and screen recess, moved 0.5 mm away from the disk |
| Spine (bottom strip) | 4 mm | **6 mm**, grown inward; underside unchanged; 4 mm longer on the motor end (x 22 → 26) to carry the second gusset |
| Deck screws | 4 × M3 at (x, z) = (18, 9), (18, 30), (−146, 13), (−146, 30) | same positions, same M3×10 (socket head). Three have a 2 mm counterbore, so the screw length is unchanged. (18, 9) is under the motor plate now, so it goes in **from under the deck** into an M3 nut in a slot in the spine's end face |
| Motor plate | 3.5 mm, 2 screws | **6 mm, 4 screws** (M3×6, socket head), heads counterbored flush on the disk side |
| Motor support | one side gusset | gussets on **both** sides, a 6 mm plate and four screws carry the motor. A two-rail **cradle** stands 0.3 mm under the motor body as a backstop if it sags (it is not in contact); the cable runs between the rails |
| Motor face to disk | 5.5 mm | 10.5 mm. The 20 mm shaft (datasheet ±1 mm) reaches 9.4–11.4 mm into the disk's 11 mm bore |
| Volume | 59.5 cm³ | 82.0 cm³ |

### Hardware

- Deck: 3 × M3×10 socket head from above, with nuts in the deck's hex pockets; 1 × M3×10 socket head from below (its head sits in the deck's hex pocket, 0.4 mm proud) plus 1 M3 nut in the spine's end slot. No washers: the counterbores are 6.2 mm wide.
- Disk hub: 1 plain M3 nut (not nyloc) and 1 × M3×8 set screw, cup point (or 1 × M3×10 socket head).
- Motor: 4 × M3×6 socket head. Do not go longer: the screw passes 2.8 mm of plate and NEMA 17 threads are only about 4.5 mm deep.
- Sensor: as on bitluni's base (two 3.0 mm holes at x −126, y −5.75 and −18.25). The LED plate now stands 2.5 mm above the seat there, so each hole has a 6.2 mm counterbore down to bitluni's face. The hole at y −5.75 reaches to 0.4 mm inside the disk's radius, so its screw must not stick out of the seat into the gap.

### Assembly order

The disk can only go in edgewise: lowering it into the gap from above is the one way in, because the screen holder and the sensor seat block the axial path. So it goes in before the motor.

1. Slide an M3 nut into the slot in the spine's end face (motor end), flats facing the sides, and push it in until it stops.
2. Bolt the base to the deck: three screws from above, then (18, 9) from below into the slot nut. That one is 6 mm from the deck's front edge and may be above a leg's foot, so fit it before the legs or use an L-key.
3. Fit the index sensor in its seat.
4. Put the M3 nut in the disk hub's slot and start the hub screw a few turns, tip clear of the bore. Lower the disk into the gap from above, hub pointing **away from the motor** (toward the screen holder), with the rim through the sensor slot.
5. Slide the motor in from behind between the gussets and over the cradle, cable side down into the channel. Turn the shaft so its flat lines up with the flat in the hub's bore (the side with the screw), and feed it through the plate into the hub, holding the hub from the front.
6. Screw the motor on with the four M3×6, reaching through the disk's cut-outs from the front. After this, the (18, 30) deck screw is under the motor: to take the base off the deck, remove the motor first.
7. Set the disk along the shaft so the rim runs centred in the sensor slot (the disk's flat motor-side face then sits 3.6 mm from the motor plate; a 3.5 mm drill shank makes a handy feeler). Turn the disk until the hub screw points up and tighten it from above, just in front of the disk (see "Tightening" above). Check the rim at the sensor for wobble by turning the disk by hand. Turn it through a full revolution with the driver disabled and make sure it touches nothing.

### Print

Spine down, no supports: the seat stubs have 45° chamfers, and the nut slot (5.8 mm) and the LED and screen windows bridge as in bitluni's part. No brim needed: about 5,600 mm² is on the bed. 0.2 mm layers, 4 walls, 30 % infill or more. PETG is a better choice than PLA for this part, because the motor sits on it and a NEMA 17 at 1 A can run at 50–70 °C, where PLA softens.

### Changing it

Edit the parameters at the top of `base_v2.py` and run `python3 base_v2.py`.

- `GROW_HI` and `GROW_LO` move the motor/LED-side and screen-side faces. Every extra millimetre of `GROW_HI` or `MOTOR_T` costs 1 mm of shaft in the hub. The script refuses a `GROW_HI` that would break the gap into the (18, 9) nut slot (about 3.1 mm).
- Neither setting widens the gap at the sensor: the seat is kept exactly as bitluni made it.

### Older files

`disk_left.stl`, `disk_right.stl` and `disk_joiner.stl` are the two-part disk for a 220 mm bed (Ender 3). `base_modified.stl` is bitluni's base with an added plate for the Raspberry Pi, L298N and HDMI adapter. `build_models.py`, `build_base.py` and `mechanical_tv_printable.scad` expect bitluni's original STLs at the path hard-coded inside them.
