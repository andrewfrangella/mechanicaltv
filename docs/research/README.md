# Research notes (2026-11-03)

Background research for the 0.3 C++ port and the diagnosis in [../DIAGNOSIS.md](../DIAGNOSIS.md).
Each report separates FACTS (with sources), ESTIMATES (with the arithmetic) and HYPOTHESES (with
the bench check that would settle them). Code references point at the Python app as it was on
`main` at commit 4c64391, before the port.

| Report | Question | Review |
| --- | --- | --- |
| [motor.md](motor.md) | Why the motor stalls at start after the 1.75 mm reprint | Key claims checked by three independent reviewers; corrections applied in DIAGNOSIS.md (pot ceiling is about VDD/3, not 1.5-2 V; torque at 600 RPM about 7-11 N·cm at 12 V) |
| [hdmi.md](hdmi.md) | What the TC358743 adapter does, whether anything is encoded, CPU and latency budget | Key claims checked by three reviewers each; latency estimate revised to about 0.1 s |
| [bitluni-reference.md](bitluni-reference.md) | How bitluni's ESP32 firmware times the disk | Key claims checked by three reviewers each; all held |
| [python-diagnosis.md](python-diagnosis.md) | Why sync never held in the Python app | Not independently reviewed |
| [realtime.md](realtime.md) | How to time 800 pixel pulses per revolution on a Pi 4 from user space | Not independently reviewed; its design was implemented and its bench checks are in VALIDATION.md |

The reports are working documents. Where one disagrees with DIAGNOSIS.md or the code, those win.
