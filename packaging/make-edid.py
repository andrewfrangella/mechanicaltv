#!/usr/bin/env python3
"""Generate the EDID files shipped in edid/.

A TC358743 HDMI-to-CSI-2 bridge (GeeekPi/52Pi, Geekworm, Waveshare, Auvidea B101)
has no EDID of its own. Until one is loaded with v4l2-ctl the HDMI source sees no
display and sends nothing, so a MacBook never lists a second display. These EDIDs
advertise only modes the Pi 4's 2-lane CSI port carries in UYVY: 1080p30 and 720p60.
1080p60 is deliberately absent (max pixel clock is capped at 80 MHz) because the
2-lane link cannot carry it.

Usage:
  python3 packaging/make-edid.py            # rewrite edid/*.txt
  python3 packaging/make-edid.py --check    # verify the shipped files match
  python3 packaging/make-edid.py --print 1080p30
"""
import argparse
from pathlib import Path
import sys

EDID_DIR = Path(__file__).resolve().parent.parent / "edid"


def checksum(block: bytes) -> int:
    return (256 - sum(block[:127]) % 256) % 256


def dtd(pclk_khz, hact, hblank, hfp, hsync, vact, vblank, vfp, vsync, hsize_mm=480, vsize_mm=270):
    """18-byte EDID detailed timing descriptor, digital separate sync, +H +V."""
    pc = pclk_khz // 10
    b = bytearray(18)
    b[0], b[1] = pc & 0xFF, pc >> 8
    b[2], b[3], b[4] = hact & 0xFF, hblank & 0xFF, ((hact >> 8) << 4) | (hblank >> 8)
    b[5], b[6], b[7] = vact & 0xFF, vblank & 0xFF, ((vact >> 8) << 4) | (vblank >> 8)
    b[8], b[9] = hfp & 0xFF, hsync & 0xFF
    b[10] = ((vfp & 0xF) << 4) | (vsync & 0xF)
    b[11] = ((hfp >> 8) << 6) | ((hsync >> 8) << 4) | ((vfp >> 4) << 2) | (vsync >> 4)
    b[12], b[13], b[14] = hsize_mm & 0xFF, vsize_mm & 0xFF, ((hsize_mm >> 8) << 4) | (vsize_mm >> 8)
    b[17] = 0x1E
    return bytes(b)


# CEA-861 timings. VIC 34 = 1920x1080p30, VIC 4 = 1280x720p60, both 74.25 MHz.
DTD_1080P30 = dtd(74250, 1920, 280, 88, 44, 1080, 45, 4, 5)
DTD_720P60 = dtd(74250, 1280, 370, 110, 40, 720, 30, 5, 5)


def base_block(preferred: bytes) -> bytes:
    b = bytearray(128)
    b[0:8] = b"\x00\xff\xff\xff\xff\xff\xff\x00"
    b[8:10] = (0x3696).to_bytes(2, "big")        # manufacturer "MTV"
    b[10:12] = (1).to_bytes(2, "little")         # product code
    b[12:16] = (1).to_bytes(4, "little")         # serial
    b[16], b[17] = 1, 36                         # week 1, 2026
    b[18], b[19] = 1, 3                          # EDID 1.3
    b[20] = 0x80                                 # digital input
    b[21], b[22] = 0x30, 0x1B                    # 48 x 27 cm
    b[23] = 0x78                                 # gamma 2.2
    b[24] = 0x02                                 # RGB 4:4:4, first DTD is preferred
    b[25:35] = bytes.fromhex("EE91A3544C99260F5054")  # sRGB chromaticity
    b[38:54] = b"\x01\x01" * 8                   # no standard timings
    b[54:72] = preferred
    # Range limits: 23-61 Hz vertical, 30-80 kHz horizontal, 80 MHz max pixel clock.
    b[72:90] = bytes([0, 0, 0, 0xFD, 0, 23, 61, 30, 80, 8, 0, 0x0A]) + b"\x20" * 6
    b[90:108] = bytes([0, 0, 0, 0xFC, 0]) + b"MechanicalTV\n"
    b[108:126] = bytes([0, 0, 0, 0x10, 0]) + bytes(13)
    b[126] = 1                                   # one CEA extension block
    b[127] = checksum(b)
    return bytes(b)


def cea_block(native_vic: int, vics, dtds) -> bytes:
    b = bytearray(128)
    b[0], b[1] = 0x02, 0x03
    svds = bytes([(0x80 | v) if v == native_vic else v for v in vics])
    data = bytes([0x40 | len(svds)]) + svds                 # video data block
    data += bytes([0x65, 0x03, 0x0C, 0x00, 0x10, 0x00])     # HDMI VSDB, address 1.0.0.0
    b[2] = 4 + len(data)
    b[3] = 0x81                                             # underscan, one native DTD
    b[4:4 + len(data)] = data
    offset = 4 + len(data)
    for d in dtds:
        b[offset:offset + 18] = d
        offset += 18
    b[127] = checksum(b)
    return bytes(b)


PROFILES = {
    "1080p30": base_block(DTD_1080P30) + cea_block(34, [34, 4], [DTD_1080P30, DTD_720P60]),
    "720p60": base_block(DTD_720P60) + cea_block(4, [4, 34], [DTD_720P60, DTD_1080P30]),
}


def render(edid: bytes) -> str:
    lines = [" ".join(f"{x:02x}" for x in edid[i:i + 16]) for i in range(0, len(edid), 16)]
    return "\n".join(lines) + "\n"


def parse(text: str) -> bytes:
    return bytes.fromhex("".join(text.split()))


def validate(edid: bytes) -> None:
    if len(edid) != 256:
        raise ValueError(f"expected 256 bytes, got {len(edid)}")
    if edid[:8] != b"\x00\xff\xff\xff\xff\xff\xff\x00":
        raise ValueError("bad EDID header")
    for n in range(2):
        if sum(edid[n * 128:(n + 1) * 128]) % 256:
            raise ValueError(f"block {n} checksum mismatch")
    if edid[126] != 1 or edid[128] != 0x02:
        raise ValueError("expected exactly one CEA extension block")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify shipped files instead of writing")
    parser.add_argument("--print", dest="show", choices=sorted(PROFILES), help="print one profile to stdout")
    args = parser.parse_args()
    for edid in PROFILES.values():
        validate(edid)
    if args.show:
        sys.stdout.write(render(PROFILES[args.show]))
        return 0
    status = 0
    EDID_DIR.mkdir(parents=True, exist_ok=True)
    for name, edid in PROFILES.items():
        path = EDID_DIR / f"{name}.txt"
        if args.check:
            if not path.exists() or parse(path.read_text()) != edid:
                print(f"{path}: out of date, run packaging/make-edid.py", file=sys.stderr)
                status = 1
        else:
            path.write_text(render(edid))
            print(f"wrote {path}")
    return status


if __name__ == "__main__":
    raise SystemExit(main())
