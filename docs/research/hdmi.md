# HDMI input path: TC358743 bridge, Pi 4 Unicam, kernel 6.12/6.18, and what the C++ port should do

Date: 2026-11-03. Scope: research for the C++ port of `mechanical_tv`; no repo edits.
Repo paths are relative to the repository (Python version, main at 4c64391). Kernel sources were
downloaded from `raspberrypi/linux` branches `rpi-6.12.y` and `rpi-6.18.y` on 2026-11-03 and
grepped locally (copies in `(working notes)/dl/`).

Legend used throughout: **FACT** = verified in a primary source quoted/cited here.
**ESTIMATE** = my arithmetic, shown. **HYPOTHESIS** = plausible, not verified.

---

## 0. Short answers

1. **The adapter does not encode anything.** The TC358743 is an HDMI receiver that re-emits the
   *same uncompressed pixels* as MIPI CSI-2 packets (16-bit YCbCr 4:2:2 "UYVY" or 24-bit RGB).
   Every H.264 you see on the Waveshare wiki is produced by the Pi's own VideoCore encoder *after*
   capture, only because those examples want to write a file. `mechanical_tv/v4l2.py` already
   receives the raw UYVY frames; there is no decode step and nothing to bypass. The chip has no
   user-loadable firmware: the Linux driver `drivers/media/i2c/tc358743.c` programs its registers
   over I2C, and `v4l2-ctl --set-edid` writes the EDID into the chip's EDID RAM over I2C.
2. **Kernel/driver:** on both `rpi-6.12.y` and `rpi-6.18.y`, `dtoverlay=tc358743` on a Pi 0-4
   switches the CSI node to `compatible = "brcm,bcm2835-unicam-legacy"`, which binds the
   *downstream legacy* Unicam driver in its non-media-controller mode. `/dev/video0` therefore
   still accepts `--set-edid`, `--set-dv-bt-timings query` and `-v pixelformat=UYVY` directly,
   exactly as `capture.py:663-697` does. The mainline media-controller driver is compiled but
   binds only to `"brcm,bcm2835-unicam-upstream"`, a compatible no Raspberry Pi DT uses.
   **The kernel is probably not 6.12:** Raspberry Pi OS Trixie images moved to 6.18 in June 2026;
   check `uname -r`. Both branches behave identically for this chip.
3. **Bandwidth:** Pi 4 = 2 lanes at 972 Mbit/s each (driver default). 1080p30 UYVY uses 51% of
   the link, 1080p50 UYVY 85%, 1080p60 UYVY does not fit (needs 3 lanes); 720p60 UYVY fits on a
   single lane (91% of one lane) and the driver will in fact enable only one lane for it.
4. **Colour:** with UYVY the chip converts to **BT.601 limited-range** YCbCr (driver setting),
   not BT.709; the disk luma path is unaffected, the preview chroma tables in `v4l2.py:318` are
   slightly wrong for HD. RGB888 is possible (BGR24) but costs 1.5x the bytes and gains nothing
   for a luma display on 2 lanes.
5. **CPU:** the real cost is reading *uncached* DMA buffers (about 450 MB/s on a Pi 4, measured
   by a Raspberry Pi engineer). A full 1080p UYVY frame costs about 9 ms just to touch; a
   sparse 200-row read costs about 1.7 ms. In C++ the per-frame cost is about 2 ms (sparse) or
   about 10 ms (full box filter), i.e. 6% or 30% of one core at 30 fps; Python's 8 ms x 15 fps is
   12% plus GIL contention. 1080p30 vs 720p60 is nearly a wash in CPU (124 vs 111 MB/s); 720p60
   only buys about 17 ms of average latency. Keep 1080p30 for bring-up.
6. **Latency:** about 33 ms (frame transfer) + about 1 ms (IRQ/wake) + about 2 ms (convert) +
   0-100 ms (wait for the index pulse) + 100 ms (one revolution to paint). The 10 FPS disk
   dominates; the HDMI side is a rounding error. Design: dequeue every frame, convert every frame
   (cheap), swap a lock-free "latest frame" pointer, never copy the whole buffer.

---

## 1. What the chip, Unicam and the Pi actually do (and the "H.264" question)

### 1.1 TC358743 = HDMI receiver + CSI-2 transmitter, uncompressed

**FACT (Raspberry Pi documentation, "Camera software" page, TC358743 section,
https://www.raspberrypi.com/documentation/computers/camera_software.html):**

> "The Toshiba TC358743 is an HDMI to CSI-2 bridge chip, capable of converting video data at up to 1080p60."
> "The chip supports incoming HDMI signals as either RGB888, YUV444, or YUV422, at up to 1080p60."
> "It can forward RGB888, or convert it to YUV444 or YUV422, and convert either way between YUV444 and YUV422."
> "Only RGB888 and YUV422 support has been tested."
> "When using two CSI-2 lanes, the maximum rates that can be supported are 1080p30 as RGB888, or 1080p50 as YUV422."
> "When using four lanes on a Compute Module, 1080p60 can be received in either format."

**FACT (Toshiba product page, https://toshiba.semicon-storage.com/us/semiconductor/product/interface-bridge-ics-for-mobile-peripheral-devices/hdmir-interface-bridge-ics/detail.TC358743XBG.html):**
"HDMI Interface to MIPI CSI-2 / HDMI Video format: Up to FHD (1920 x 1080, 60 fps, 24 bpp)". The words
"H.264", "compression", "encoder", "scaler" and "firmware" do not appear on the page.

**FACT (kernel driver `drivers/media/i2c/tc358743.c`, rpi-6.12.y):** the only output media-bus codes the
driver enumerates are `MEDIA_BUS_FMT_BGR888_1X24`, `MEDIA_BUS_FMT_RGB888_1X24` and
`MEDIA_BUS_FMT_UYVY8_1X16` (lines 1695-1705). Lines 650-690 program the chip's output colour mode
per format (`VOUT_SET2`, `VI_REP`, `CONFCTL`); there is no scaler, no compression and no firmware
load anywhere in the file (`grep -n firmware tc358743.c` finds nothing). Line 2020-2021:
"The CSI bps per lane must be between 62.5 Mbps and 1 Gbps. The default is 594 Mbps for 4-lane 1080p60
or 2-lane 720p60. 972 Mbps allows 1080P50 UYVY over 2-lane."

**FACT (same file, `tc358743_s_edid`, lines 1807-1855):** the EDID the user loads is written straight into
the chip's EDID RAM over I2C:

```c
for (i = 0; i < edid_len; i += EDID_BLOCK_SIZE)
        i2c_wr(sd, EDID_RAM + i, edid->edid + i, EDID_BLOCK_SIZE);
```

So "firmware" for this adapter *is* the kernel driver plus the EDID you hand it; there is nothing
else to write.

### 1.2 Unicam = DMA engine for raw CSI-2 packets

**FACT (Raspberry Pi documentation, same page):** "this kernel module, called `bcm2835-unicam`, interfaces with
V4L2 subdevice drivers to deliver raw frames." The Unicam peripheral unpacks CSI-2 packets and
DMA-writes lines into the V4L2 buffer as they arrive; the legacy driver raises a frame-start
interrupt (timestamp taken there) and a frame-end interrupt (buffer handed back):

`drivers/media/platform/bcm2835/bcm2835-unicam.c` (rpi-6.12.y) lines 1004-1021:
```c
if (ista & UNICAM_FSI) {
        /*
         * Timestamp is to be when the first data byte was captured,
         * aka frame start.
         */
        ts = ktime_get_ns();
        ...
        unicam->node[i].cur_frm->vb.vb2_buf.timestamp = ts;
```
and lines 871-878 / 1025-1031: when no buffer is queued the next frame is steered into a one-page
dummy buffer (`unicam_schedule_dummy_buffer`), i.e. frames are *dropped*, never backlogged.

**Consequence (FACT by construction):** the Pi does *no* decoding and *no* ISP work for this input.
The only pixel work is whatever the application chooses: in this app, the 32x25 downscale and the
320x250 preview (`mechanical_tv/v4l2.py:337-369`).

### 1.3 The Waveshare wiki, line by line

Fetched https://www.waveshare.com/wiki/HDMI_to_CSI_Adapter on 2026-11-03. Quotes are verbatim.

Specification: "Controller: TC358743XBG", "Video: HDMI1.4, RGB888, up to 1080p@30fps support",
"Video: 2-lane MIP CSI-2", "Audio: I2S audio output, LPCM audio stream format".
FAQ: "Does it come with HDCP?" -> "No." "Note: HDMI to CSI camera does not support libcamera driver."

The three examples that mention H.264, and what each really does:

1. `./yavta --capture=1000 -n 3 --encode-to=file.h264 -f UYVY -m -T /dev/video0`
   Wiki text: "The demo will capture 1000 frames of images and save them as a file.h264 file".
   **FACT:** `-f UYVY` requests *raw UYVY* from `/dev/video0`; `--encode-to` is an option of 6by9's
   yavta fork that pipes those raw frames into the VideoCore encoder. In the fork's source
   (https://raw.githubusercontent.com/6by9/yavta/master/yavta.c): line 1970
   `mmal_component_create("vc.ril.video_encode", &dev->encoder)`, line 2086
   `encoder_output->format->encoding = MMAL_ENCODING_H264;`, line 2712
   `"-E, --encode-to [file]  Set filename to write to. Default of file.h264."`. The encoding runs on the
   Pi's GPU, after capture, to make a file.
2. `gst-launch-1.0 -vvv v4l2src ! "video/x-raw,framerate=30/1,format=UYVY" ! v4l2h264enc ... ! filesink location=file.mkv`
   **FACT:** `v4l2src` pulls `video/x-raw ... format=UYVY` (raw) and `v4l2h264enc` is the Pi's own
   `bcm2835-codec` H.264 encoder (`/dev/video11`, "Video encode" in the Pi docs' V4L2 node table).
   Remove `v4l2h264enc` and you have raw frames; that is what `v4l2-ctl --stream-mmap` does.
3. `raspivid -t 0` under "Extend Computer Screen to Raspberry Pi": the wiki says "you need to delete
   dtoverlay=tc358743 ... Legacy Camera -> Yes" and "Currently, it only supports 720p". This is the old
   firmware (MMAL) camera stack treating the bridge as a camera; `raspivid -t 0` previews, it does
   not encode unless `-o` is given. Irrelevant to the V4L2 path this app uses.

The wiki's own raw-capture commands prove the point: Pi 5 section
`v4l2-ctl --stream-mmap=3 --stream-count=1000 --stream-to=/dev/null` and
`v4l2-ctl ... --set-fmt-video=width=1280,height=720,pixelformat='RGB3' --stream-mmap=4 --stream-skip=3 --stream-count=2 --stream-to=hc1xx.rgb`
followed by `ffplay -f rawvideo -video_size 1280x720 -pixel_format bgr24 hc1xx.rgb`: a *raw* RGB
dump played back as raw video.

Also from the wiki, the EDID step is just `v4l2-ctl -d /dev/v4l-subdev2 --set-edid=file=720p60edid` (Pi 5) or the
`set_edid.sh` script in their zip (Pi 0-4), and the audio is separate I2S wiring
("WFS to PIN35 (GPIO19), SCK to PIN12 (GPIO18), SD to PIN38 (GPIO20)" — note GPIO18 is this project's STEP
PWM pin, so the audio overlay must never be enabled on this build).

**Bottom line for the user's question:** the format you get is already the fastest one the chip
can produce (uncompressed 16-bit UYVY, or 24-bit RGB which is *more* bytes). Nothing in the
adapter can be reprogrammed to produce something smaller: it has no scaler and no firmware. The
levers are on the Pi side: which mode the EDID advertises (section 5), and how few bytes the CPU
touches per frame (section 5/6).

---

## 2. Pi 4 CSI-2 bandwidth

### FACTS
- Pi docs (same page): "The first instance of Unicam supports two CSI-2 data lanes, while the second supports four."
  "Compute Modules and Raspberry Pi 5 route out all lanes from both peripherals." "Other models prior to Raspberry Pi 5
  only expose the second instance, routing out only two of the data lanes to the camera connector."
- Overlay README (`arch/arm/boot/dts/overlays/README`, rpi-6.12.y line 5271 ff., identical in 6.18 line 5592 ff.):
  ```
  Name:   tc358743
  Params: 4lane                   Use 4 lanes (only applicable to Compute Modules CAM1 connector).
          link-frequency          Set the link frequency. Only values of 297000000
                                  (574Mbit/s) and 486000000 (972Mbit/s - default)
                                  are supported by the driver.
          media-controller        Configure use of Media Controller API for
                                  configuring the sensor (default off)
          cam0                    Adopt the default configuration for CAM0 on a
                                  Compute Module (CSI0, i2c_vc, and cam0_reg).
  ```
  (The "574Mbit/s" in the README is a typo for 594; the driver uses `bps_pr_lane = 2 * link_frequency`.)
- `tc358743.dtsi` (both branches): `link-frequencies = /bits/ 64 <486000000>;`, `data-lanes = <1 2>;` by
  default, `<1 2 3 4>` only with `4lane`; `clock-frequency = <27000000>` reference clock.
- Driver lane rule (`tc358743.c:694-703`):
  ```c
  u32 bits_pr_pixel = (state->mbus_fmt_code == MEDIA_BUS_FMT_UYVY8_1X16) ?  16 : 24;
  u32 bps = bt->width * bt->height * fps(bt) * bits_pr_pixel;
  u32 bps_pr_lane = (pdata->refclk_hz / pdata->pll_prd) * pdata->pll_fbd;
  return DIV_ROUND_UP(bps, bps_pr_lane);
  ```
  and lines 711-725 disable the lanes it does not need (`if (lanes < 2) i2c_wr32(sd, D1W_CNTRL, MASK_D1W_LANEDISABLE)`).
- 6by9 (Raspberry Pi engineer), install thread first post, 2020-08-05
  (https://forums.raspberrypi.com/viewtopic.php?t=281972): "Over the normal 2 CSI-2 lanes the data rate is such that
  BGR3 can run at a maximum of 1080p30, whilst UYVY will go up to 1080p50."
- Pi 5 product page (https://www.raspberrypi.com/products/raspberry-pi-5/): "2 x 4-lane MIPI camera/display transceivers".

### ESTIMATES (active-pixel bit rate / 972 Mbit/s per lane; this is exactly the driver's formula)

| Mode | bpp | bits/s | lanes needed (ceil) | share of 2 lanes (1.944 Gbit/s) | Pi 4 (2 lanes) |
|---|---|---|---|---|---|
| 640x480p60 UYVY | 16 | 294.9 M | 1 | 15% | yes (1 lane) |
| 720p30 UYVY | 16 | 442.4 M | 1 | 23% | yes (1 lane) |
| 720p60 UYVY | 16 | 884.7 M | 1 (91% of one lane) | 46% | yes, on **one** lane |
| 1080p30 UYVY | 16 | 995.3 M | 2 | 51% | yes |
| 1080p50 UYVY | 16 | 1,658.9 M | 2 | 85% | yes (6by9's stated max) |
| 1080p60 UYVY | 16 | 1,990.7 M | 3 | 102% | **no** |
| 720p60 RGB888 | 24 | 1,327.1 M | 2 | 68% | yes |
| 1080p30 RGB888 | 24 | 1,493.0 M | 2 | 77% | yes (docs' max for RGB) |
| 1080p50 RGB888 | 24 | 2,488.3 M | 3 | 128% | no |
| 1080p60 RGB888 | 24 | 2,986.0 M | 4 (of 4 x 972 = 3.888 G) | — | CM4 CAM1 `4lane=1` only |

Arithmetic example: 1920 x 1080 x 30 x 16 = 995,328,000 bit/s; / 972,000,000 = 1.024 -> 2 lanes.
720p60: 1280 x 720 x 60 x 16 = 884,736,000; / 972,000,000 = 0.910 -> 1 lane.

Peak-line check for the two candidate modes (the chip has a line FIFO, `fifo_level = 374`,
`tc358743.c:1986-1996`): 1080p30 line = 3840 bytes arriving in 25.9 us, sendable on 2 lanes in
15.8 us -> no backlog. 720p60 on the single lane the driver picks: 2560 bytes arrive in 17.2 us,
take 21.1 us to send, line period 22.2 us -> fits, but with a 470-byte backlog each line.
**HYPOTHESIS:** 720p60 UYVY on one lane at 972 Mbit/s is tighter than 1080p30 on two; it is the
mode PiKVM V3 ships by default (https://docs.pikvm.org/edid/: "on V3, the default mode is 720p"),
so it works in the field, but the user should verify there are no `V4L2_BUF_FLAG_ERROR` frames
(bench check B6). `link-frequency=297000000` (594 Mbit/s, 2 lanes = 74%) is the fallback the
driver comment explicitly lists as tested for 720p60.

Memory traffic (bytes per frame x fps): 1080p30 UYVY 4,147,200 B x 30 = **124.4 MB/s**;
720p60 UYVY 1,843,200 x 60 = **110.6 MB/s**; 720p30 = 55.3 MB/s; 640x480p60 614,400 x 60 = 36.9 MB/s;
1080p30 RGB888 6,220,800 x 30 = 186.6 MB/s; 720p60 RGB888 = 165.9 MB/s. Against a Pi 4 memcpy
bandwidth of about 2.7 GB/s (tinymembench, https://github.com/geerlingguy/sbc-reviews/issues/4:
"memcpy ... about 2737 MB/s") the DMA load is 4-7% of the bus: not a constraint.

### CM4 and Pi 5
- CM4: CAM1 has 4 lanes; `dtoverlay=tc358743,4lane=1` (README above). 4 x 972 = 3.888 Gbit/s -> 1080p60
  in either format (docs quote above).
- Pi 5: no Unicam. **FACT** (6by9, 2025-03-24, https://forums.raspberrypi.com/viewtopic.php?t=385709):
  "Unicam has nothing to do with Pi5 - it's been superceded by rp1-cfe." and 2025-04-15: "Media Controller is
  mandatory for Pi5". Both 22-pin connectors are 4-lane. The RP1 CFE (mainline
  `Documentation/admin-guide/media/raspberrypi-rp1-cfe.rst`): "The PiSP Camera Front End (CFE) is a module which
  combines a CSI-2 receiver with a simple ISP, called the Front End (FE)." "The CFE has four DMA engines".
  Pi 5 needs `media-ctl` link/format setup and the EDID/timings go to the subdev (section 3.4).

---

## 3. Which kernel driver, on which OS, and the exact command sequences

### 3.1 What the user is probably running (FACTS + one inference)

- Raspberry Pi OS release notes (https://downloads.raspberrypi.com/raspios_arm64/release_notes.txt, fetched 2026-11-03):
  ```
  2025-10-01: * Based on Debian Trixie release   * Linux kernel 6.12.47
  2025-11-24: * Linux kernel 6.12.47
  2025-12-04: * Linux kernel 6.12.47
  2026-04-13: * Linux kernel 6.12.75
  2026-06-18: * Linux kernel 6.18.34
  2026-09-15: * Linux kernel 6.18.50
  2026-10-06: * Linux kernel 6.18.50
  ```
  Download page (https://www.raspberrypi.com/software/operating-systems/): Raspberry Pi OS (64-bit) "Release date: 6 Oct 2026",
  "Kernel version: 6.18", "Debian version: 13 (trixie)"; Raspberry Pi OS (Legacy, 64-bit): "Kernel version: 6.12",
  "Debian version: 12 (bookworm)".
- Debian package versions (https://packages.debian.org, `v4l-utils`): bookworm `1.22.1-5+b2`, trixie `1.30.1-1`.
  `--fix-edid-checksums` was replaced by `--keep-edid-checksums` upstream (commit "v4l2-ctl: replace --fix-edid-checksums
  by --keep-edid-checksums", shipped in 1.28); the Debian trixie `v4l2-ctl` is 1.30.1 and its usage text
  (`utils/v4l2-ctl/v4l2-ctl-edid.cpp:234`) reads `--keep-edid-checksums  If specified then any checksum errors will be kept.`
- **Inference:** the user's `v4l2-ctl` rejects `--fix-edid-checksums`, so v4l-utils >= 1.28 -> Trixie (1.30.1), not
  Bookworm (1.22.1). The kernel is then 6.12.x if the image predates June 2026 and has not been `apt full-upgrade`d
  since, otherwise **6.18.x**. The task's "assume 6.12" is therefore unsafe; `uname -r` decides (bench check B1).
  I verified both branches below; the answer is the same on both.

### 3.2 The driver situation on rpi-6.12.y and rpi-6.18.y (FACTS from source)

Two Unicam drivers are built for the Pi 4:

- `arch/arm64/configs/bcm2711_defconfig` (6.12 and 6.18): `CONFIG_VIDEO_BCM2835_UNICAM_LEGACY=m`,
  `CONFIG_VIDEO_BCM2835_UNICAM=m`, `CONFIG_VIDEO_TC358743=m`, `CONFIG_VIDEO_ISP_BCM2835=m`, `CONFIG_VIDEO_CODEC_BCM2835=m`.
- Legacy driver `drivers/media/platform/bcm2835/bcm2835-unicam.c` (Kconfig: "This is the downstream version of this
  driver that still supports being driven from the video node for simple devices. The mainline driver only supports
  using Media Controller."). Its match table (6.12 lines 3508-3509, 6.18 lines 3425-3426):
  ```c
  { .compatible = "brcm,bcm2835-unicam", .data = (void *)1 },
  { .compatible = "brcm,bcm2835-unicam-legacy", .data = 0 },
  ```
  and probe (6.12 lines 3383-3390): `unicam->mc_api = media_controller;` (module param) then
  `if (of_device_get_match_data(...)) unicam->mc_api = true;` and `if (of_property_read_bool(..., "brcm,media-controller")) unicam->mc_api = true;`.
  In non-MC mode the video node passes EDID and DV-timing ioctls straight through to the bridge subdev
  (lines 1567-1580 `unicam_s_edid`/`unicam_g_edid` -> `v4l2_subdev_call(dev->sensor, pad, set_edid, ...)`,
  lines 1715-1776 `unicam_s_dv_timings`, `unicam_query_dv_timings`, `unicam_enum_dv_timings`, `unicam_dv_timings_cap`).
  Queue: `q->io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ;` (line 3011), `vb2_dma_contig_memops`, `.vidioc_expbuf = vb2_ioctl_expbuf` (line 1854).
- Mainline driver `drivers/media/platform/broadcom/bcm2835-unicam.c` (present on rpi-6.12.y, absent on rpi-6.6.y):
  **in the Pi tree its only compatible is** `{ .compatible = "brcm,bcm2835-unicam-upstream", }` (6.12 line 2716, 6.18 line 2764).
  A GitHub code search for `bcm2835-unicam-upstream` in `raspberrypi/linux` returns exactly one file: that driver.
  No device tree in the Pi tree selects it, so **it never binds on Raspberry Pi OS unless you edit the DT yourself.**
- Base DT `arch/arm/boot/dts/broadcom/bcm283x.dtsi` lines 469-476: `csi1: csi@7e801000 { compatible = "brcm,bcm2835-unicam"; ... brcm,num-data-lanes = <4>; status = "disabled"; }`.
- The overlay `arch/arm/boot/dts/overlays/tc358743-overlay.dts` is byte-identical on 6.12 and 6.18:
  ```dts
  legacy_frag: fragment@100 {
          target = <&csi1>;
          __overlay__ {
                  compatible = "brcm,bcm2835-unicam-legacy";
          };
  };
  __overrides__ {
          media-controller = <0>,"!100";
  ```
  So `dtoverlay=tc358743` -> csi1 compatible becomes `brcm,bcm2835-unicam-legacy` -> legacy driver, `mc_api = 0`:
  **simple `/dev/video0`, EDID and timings set on the video node.** `dtoverlay=tc358743,media-controller=1`
  disables fragment 100, the compatible stays `brcm,bcm2835-unicam` -> *the same legacy module* with `.data = 1`
  -> `mc_api = true`: video node `unicam-image` with `V4L2_CAP_IO_MC`, formats/links via `media-ctl`,
  EDID/timings on `/dev/v4l-subdevN`.
- Pi 5 variant `tc358743-pi5-overlay.dts` (added by PR #6795, merged 2025-04-25, rpi-6.12.y, firmware bump to 6.12.25):
  just `#include "tc358743.dtsi"` with `compatible = "brcm,bcm2712"`, no legacy fragment (rp1-cfe, MC mandatory).
  The firmware picks the `-pi5` variant automatically on a Pi 5. The thread that triggered it
  (https://forums.raspberrypi.com/viewtopic.php?t=385709, 6by9 2025-03-31): "on 6.12 the overlay is wrong for Pi5, and
  will be setting the compatible to "brcm,bcm2835-unicam-legacy" erroneously." That bug was Pi 5 only.
- Audio overlay README: `tc358743-audio ... Wiring is LRCK/WFS to GPIO 19, BCK/SCK to GPIO 18, and DATA/SD to GPIO 20.`
  Do not load it: GPIO 18 is the A4988 STEP PWM pin in this project.

One documentation contradiction to be aware of (FACT, same Pi docs page): "The V4L2 interface for Unicam is available
only when using `libcamera`." That sentence is about camera sensors; the TC358743 section on the same page gives
plain `v4l2-ctl` instructions, and Waveshare states the bridge "does not support libcamera driver". The V4L2 path
this app uses is the supported one for this chip.

### 3.3 Command sequence, legacy driver, non-MC mode (the default; what `capture.py` does)

```sh
# /boot/firmware/config.txt
camera_auto_detect=0
dtoverlay=tc358743            # optional: ,link-frequency=297000000  (594 Mbit/s/lane)

# after reboot
dmesg | grep -i -E "tc358743|unicam"          # "tc358743 10-000f: ... found", unicam probe
v4l2-ctl --list-devices                        # unicam (platform:fe801000.csi): /dev/video0
cat /proc/device-tree/soc/csi@7e801000/compatible | tr '\0' '\n'   # expect brcm,bcm2835-unicam-legacy
v4l2-ctl -d /dev/video0 -D                     # Driver name: unicam; Device Caps must NOT list "I/O MC"

v4l2-ctl -d /dev/video0 --set-edid=file=/opt/mechanical-tv/current/mechanical_tv/edid/1080p30.txt
#   (v4l-utils >= 1.28 fixes checksums by default; the shipped files are already correct, so no flag;
#    on 1.22 add --fix-edid-checksums only for hand-made files)
sleep 2                                        # source re-reads EDID after hot-plug toggles
v4l2-ctl -d /dev/video0 --query-dv-timings     # must show 1920x1080p30 (or 1280x720p60)
v4l2-ctl -d /dev/video0 --set-dv-bt-timings query
v4l2-ctl -d /dev/video0 -v pixelformat=UYVY    # only the pixelformat is honoured; size comes from the timings
v4l2-ctl -d /dev/video0 -V                     # confirm 1920x1080 UYVY, bytesperline 3840, sizeimage 4147200
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=300 --stream-to=/dev/null --verbose
v4l2-ctl -d /dev/video0 --log-status           # bridge state: +5V, hotplug, TMDS, "Stable sync signal"
```
Pi docs confirm the last two semantics: "The easiest approach for this is to use the command `v4l2-ctl --set-dv-bt-timings query`."
and "Changing the output pixel format is achieved by setting it by using `VIDIOC_S_FMT`, but only the pixel format field will be
updated as the resolution is configured by the DV timings." and "The driver does support generating the `SOURCE_CHANGED` events".

This is what `mechanical_tv/capture.py:663-697` (`_prepare_csi`) already does via `subprocess` with a 15 s timeout per call
(`capture.py:478`), then `v4l2.py:159-199` opens the node in-process (`VIDIOC_S_FMT` UYVY, `REQBUFS` 4 x MMAP, mmap, `STREAMON`).

### 3.4 Command sequence, media-controller mode (`media-controller=1` on Pi 0-4, or any Pi 5)

Entity names come from the drivers: the bridge subdev is `tc358743 <bus>-000f` (I2C address 0x0f, `tc358743.dtsi`),
the legacy MC video node is `unicam-image` (`bcm2835-unicam.c:3046`), on Pi 5 the nodes are `rp1-cfe-csi2-ch0` etc.
(mainline rp1-cfe doc). Read the real names and the subdev number with `media-ctl -p` first.

```sh
media-ctl -d /dev/media0 -p                                   # find 'tc358743 10-000f' and its /dev/v4l-subdevN
SD=/dev/v4l-subdev0                                           # whatever -p printed for the tc358743 entity
v4l2-ctl -d $SD --set-edid=file=.../1080p30.txt
v4l2-ctl -d $SD --query-dv-timings
v4l2-ctl -d $SD --set-dv-bt-timings query
# Pi 0-4, legacy driver in MC mode:
media-ctl -d /dev/media0 -V "'tc358743 10-000f':0 [fmt:UYVY8_1X16/1920x1080 field:none colorspace:smpte170m]"
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1920,height=1080,pixelformat=UYVY
# Pi 5 (Waveshare's sequence, verbatim from the wiki, 720p RGB example):
media-ctl -d /dev/media0 -r
media-ctl -d /dev/media0 -l "'csi2':4 -> 'rp1-cfe-csi2_ch0':0 [1]"
media-ctl -d /dev/media0 -V "'csi2':0 [fmt:RGB888_1X24/1280x720 field:none colorspace:srgb]"
media-ctl -d /dev/media0 -V "'csi2':4 [fmt:RGB888_1X24/1280x720 field:none colorspace:srgb]"
v4l2-ctl -d /dev/video0 --set-fmt-video=width=1280,height=720,pixelformat=RGB3
v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=300 --stream-to=/dev/null
```
(For UYVY on Pi 5 use `UYVY8_1X16` in both `-V` lines and `pixelformat=UYVY`.) **HYPOTHESIS:** the exact
pad numbers (`'csi2':4`) and whether the legacy-MC mode needs an explicit `-V` on the `unicam` sink pad vary by kernel;
`media-ctl -p` output is the source of truth. The C++ port should implement the non-MC path (sections 3.3) as primary and
detect MC mode by `V4L2_CAP_IO_MC` in `VIDIOC_QUERYCAP` `device_caps`, then drive the subdev with the same
`VIDIOC_S_EDID` / `VIDIOC_QUERY_DV_TIMINGS` / `VIDIOC_S_DV_TIMINGS` ioctls on `/dev/v4l-subdevN`
(`VIDIOC_SUBDEV_S_FMT` for the pad format). There is no reason to turn MC mode on for a Pi 4.

Other overlay facts: `cma=96M` (Waveshare's cmdline advice) is unnecessary here: 4 buffers x 4,147,200 B = 16.6 MB
(ESTIMATE); check `dmesg | grep -i cma` before touching it. The Pi 4 64-bit default CMA is large enough for the current
code, which already streams.

---

## 4. Colour: what UYVY from this chip really is, and whether RGB888 is worth it

**FACT (`tc358743.c:650-690`):** for `MEDIA_BUS_FMT_UYVY8_1X16` the driver programs
`VI_REP` with `MASK_VOUT_COLOR_601_YCBCR_LIMITED` (register options in `tc358743_regs.h:536-543`:
`RGB_FULL 0x00, RGB_LIMITED 0x20, 601_YCBCR_FULL 0x40, 601_YCBCR_LIMITED 0x60, 709_YCBCR_FULL 0x80, 709_YCBCR_LIMITED 0xa0, ...`)
plus `CONFCTL` `MASK_YCBCRFMT_422_8_BIT`, and reports `V4L2_COLORSPACE_SMPTE170M` (`tc358743_g_colorspace`, line 1719-1720).
For BGR888/RGB888 it programs `MASK_VOUT_COLOR_RGB_FULL` and reports `V4L2_COLORSPACE_SRGB`.
Driver default at probe is 24-bit RGB (`MEDIA_BUS_FMT_BGR888_1X24` on 6.12 line 2175, `RGB888_1X24` on 6.18 line 2247;
the app overrides to UYVY, `v4l2.py:171-176`). The 6.12 driver also warns once: "RGB format isn't actually supported by
the hardware. The application should be fixed to use BGR." (the CSI-2 byte order is B,G,R).

Consequences:
- The UYVY the app receives is **BT.601, limited range (Y 16..235)** for *any* input resolution, because the chip does the
  RGB->YCbCr conversion itself with the 601 matrix. `mechanical_tv/v4l2.py:287-300` and `:318`
  (`_yuv_tables(bt709=height >= 720)`) decode HD with BT.709 coefficients: the preview's chroma is slightly off
  (FACT about the code; a small visual error only). The disk luma path (`v4l2.py:323` full-range expansion of
  limited Y, `:359-367` box average) is correct for limited-range output. The C++ port should use BT.601 tables for
  this bridge, or read `colorspace` from `VIDIOC_G_FMT` and pick accordingly.
- **HYPOTHESIS (range):** the shipped EDID's CEA block has no Video Capability Data Block, so a Mac may send
  *limited-range* RGB for the CEA modes (CTA-861 default for CE formats). Whether the chip's "RGB -> 601 limited" path
  compresses an already-limited signal again (black at Y about 30 instead of 16) is not determinable from the
  driver; bench check B8 settles it in two minutes.
- **RGB888 instead of UYVY:** possible via `-v pixelformat=BGR3` (`V4L2_PIX_FMT_BGR24`, legacy unicam format table line 288-289),
  no colour conversion in the chip, and `make-edid.py` already claims RGB 4:4:4 input. But it is 24 bpp:
  1080p30 goes from 124 MB/s to 187 MB/s, lane use from 51% to 77%, and the CPU must then compute luma from three bytes
  per pixel instead of taking Y as-is. For a 32x25 *luminance* display UYVY is strictly better: the chip's hardware
  matrix produces Y for free. Verdict: **not worth it on 2 lanes**, keep UYVY.

---

## 5. CPU cost, and which EDID mode to advertise

### 5.1 The dominant cost is reading uncached DMA memory (FACTS)

- `videobuf2-dma-contig.c` (rpi-6.12.y): MMAP buffers are allocated with `dma_alloc_attrs` and mapped to userspace
  with `dma_mmap_attrs` -> on the non-coherent BCM2711 that is a Normal Non-cacheable mapping. Raspberry Pi engineer
  davidplowman, 2023-07-10/11 (https://github.com/raspberrypi/picamera2/issues/740): "The root cause of the
  difficultly with these buffers is that they're allocated by the V4L2 kernel drivers" "which makes them "uncached" and
  therefore slow to use because you have to go all the way out to the external memory" "the best way to use them is simply
  to `memcpy` everything into a regular cached buffer." Measured: "it seems to take about 40ms per 18MB (a 12MP YUV420
  image), which is slow." -> **about 450 MB/s** sequential read from a V4L2 buffer on a Pi 4 (18 MB / 0.040 s).
- Cached memory: tinymembench memcpy about 2.7 GB/s on a Pi 4 (source above).
- Cortex-A72 at 1.5 GHz (Pi 4 nominal); 32x25 = 800 output cells; the frame's 16:9 picture occupies rows 3-20 of the
  25 disk rows (`v4l2.py:266-283` `placement`), i.e. 18 x 32 = 576 live cells, each covering 60 x 60 source pixels at 1080p
  or 40 x 40 at 720p.

### 5.2 Per-frame estimates (ESTIMATES; arithmetic shown)

(a) **Full box filter from Y only** (every Y sample of the picture averaged into its cell). Reading is the cost; the
chroma bytes are interleaved so every bus word must be fetched anyway:
- 1080p: 4,147,200 B / 450 MB/s = **9.2 ms** of uncached streaming reads; compute on the 2.07 M luma samples with NEON
  (`ld1` 16 B -> 8 Y via `uzp1`, `uaddlp`/`uadalp` accumulators, about 4 cycles per 16 B): 259,200 x 4 = 1.04 M cycles =
  **0.7 ms**; scalar C++ about 2 cycles/sample = **2.8 ms**. Total about **10 ms (NEON) / 12 ms (scalar)** per frame;
  x 30 fps = 30-36% of one core.
- 720p: 1,843,200 / 450 = 4.1 ms + 0.3 ms NEON = **4.4 ms**; x 60 = 26%; x 30 = 13%.
- 640x480: 614,400 / 450 = 1.4 ms + 0.1 = **1.5 ms**; x 60 = 9%.
- With cached dma-heap buffers instead (section 6.3): 4.15 MB / 2.7 GB/s = 1.5 ms + cache invalidate (64,800 lines,
  about 0.5 ms) + 0.7 ms NEON = **about 2.7 ms** per 1080p frame (8%/core at 30 fps).

(b) **Sparse sampling** (what the Python path does, and what C++ should do if it stays on uncached MMAP buffers).
Sample 8 of the 60 rows in each cell row (good enough anti-aliasing for a 32x25 display), reading those rows
sequentially so the uncached loads stay 16/64-byte streaming loads:
- 1080p: 18 cell rows x 8 = 144 full rows x 3840 B = 553 KB -> 1.2 ms; or all 25 cell rows (crop mode) 200 rows = 768 KB
  -> **1.7 ms**; compute from L2 about 0.1-0.2 ms. **About 1.5-2 ms per frame; x 30 fps = 5-6% of one core.**
- 720p: 200 rows x 2560 B = 512 KB -> 1.1 ms; x 60 = 7%; x 30 = 3.5%.
- Pure point sampling (e.g. 5x5 loads per cell = 20,000 scattered 4-byte loads) is *not* cheaper: each uncached load is
  a full DRAM transaction (about 100-150 ns), partially overlapped by the A72's miss handling -> 0.5-3 ms with worse
  aliasing. Read whole rows.
- Python/numpy (`v4l2.py:342` copies 250 sampled rows = 960 KB -> 2.1 ms of uncached copy, then LUTs, gathers and the
  box sum in numpy) is about 8 ms per converted frame per `docs/INSTALL.md:54` and `docs/ARCHITECTURE.md:155`,
  at 15 fps = 12% of a core, *while holding the GIL for the Python-level parts*. C++ removes the GIL coupling entirely.

(c) **Hardware offload** (section 6.2): about 0 ms of CPU pixel work; the CPU reads a 32x32..128x100 output (1-40 KB).

### 5.3 Which EDID mode? (RECOMMENDATION with reasoning)

The shipped EDIDs (`packaging/make-edid.py`, outputs `mechanical_tv/edid/1080p30.txt` and `720p60.txt`) are FACTS:
preferred DTD VIC 34 (1920x1080p30, 74.25 MHz) with VIC 4 (1280x720p60, 74.25 MHz) as alternate (or the reverse for
`720p60.txt`), range limits "23-61 Hz vertical, 30-80 kHz horizontal, 80 MHz max pixel clock" (`make-edid.py:72`) which
excludes 1080p60 (148.5 MHz), CEA byte 3 = 0x81 = underscan, **no YCbCr support bits** (so the source must send RGB),
HDMI VSDB present. `docs/INSTALL.md:56`: the Mac lists "MechanicalTV at 1920 x 1080, 30 Hz" with the current file.

| | 1080p30 UYVY | 720p60 UYVY | 720p30 UYVY | 640x480p60 UYVY |
|---|---|---|---|---|
| bytes/frame | 4.15 MB | 1.84 MB | 1.84 MB | 0.61 MB |
| DMA MB/s | 124 | 111 | 55 | 37 |
| lanes (driver) / link use | 2 / 51% | 1 / 91% of that lane | 1 / 46% | 1 / 30% |
| C++ sparse cost per second | 30 x 2 = 60 ms (6%) | 60 x 1.1 = 68 ms (7%) | 30 x 1.1 = 34 ms (3.5%) | 60 x 0.5 = 30 ms (3%) |
| C++ full-filter cost per second | 300 ms (30%) | 265 ms (26%) | 130 ms (13%) | 90 ms (9%) |
| frame transfer latency | 33 ms | 17 ms | 33 ms | 17 ms |
| Mac acceptance | proven (INSTALL.md:56) | very likely (VIC 4 is a basic HDMI mode) | likely | **uncertain** |

- CPU: in C++ the mode barely matters (sparse: 60 vs 68 ms per second of CPU). The *only* thing 720p60 buys is about
  17 ms less average capture latency and twice as many chances per revolution to pick a fresh frame; its price is a
  1-lane link at 91% (verify, B6) and a Mac desktop rendered at 1280x720.
- 640x480p60 would cut DMA and CPU by 3-7x but **HYPOTHESIS:** macOS may refuse or hide a 640x480 "TV" mode and will
  render the UI at VGA resolution; it needs VIC 1 + a 25.175 MHz DTD added to `make-edid.py`; untested. Not worth it: the
  CPU saving is a few percent of one core.
- **Recommendation: keep `1080p30.txt` as the default for the port's bring-up** (the link is at 51%, the Mac already
  accepts it, the C++ sparse path costs about 6% of a core). Offer `720p60.txt` as an `MTV_HDMI_EDID` option and measure
  dropped/error frames before adopting it. If a full-frame box filter is wanted for picture quality, prefer the
  hardware ISP downscale (6.2) or cached dma-heap buffers (6.3) over brute-force uncached reads.

What a MacBook accepts (HYPOTHESES, verify with B7): macOS reads the EDID and offers the preferred DTD plus the
CEA SVD list; 1080p30 is already confirmed working; 1280x720@60 (VIC 4) is a standard HDMI TV format and is widely
reported to work with TC358743 boards (PiKVM's default); Apple Silicon ignores classic EDID override plists, so the EDID
in the chip is the only thing the Mac sees. Because the EDID advertises RGB only, the Mac cannot pick YCbCr 4:2:0/4:2:2.

HDCP (FACTS): driver `state->pdata.enable_hdcp = false;` (`tc358743.c:1985`) and the register-write path refuses the HDCP
registers: "It should not be possible for the user to enable HDCP with a simple v4l2-dbg command. DO NOT REMOVE THIS unless all
other issues with HDCP have been resolved." (lines 1435-1446); Waveshare FAQ: "Does it come with HDCP?" "No."
Consequence: an ordinary macOS desktop, browser, video player, screen-share, etc. is unaffected. **HYPOTHESIS (community
reports only, no Apple document found):** DRM-protected playback (Apple TV app purchases, Netflix/Prime in Safari) shows
black on this "display"; `v4l2-ctl --log-status` prints "HDCP encrypted content: yes/no" (`tc358743.c:1361`) to confirm.

---

## 6. Latency budget and the C++ capture design

### 6.1 Budget, HDMI pixel to LED (ESTIMATES built on FACTS: 30 Hz / 60 Hz frame periods, 10 FPS disk)

| Stage | 1080p30 | 720p60 | Notes |
|---|---|---|---|
| Mac compositor -> scan-out start | 0-33 ms | 0-17 ms | macOS double-buffers; not controllable |
| HDMI scan-out + bridge FIFO + Unicam DMA until frame end | 33 ms | 17 ms | buffer is complete only at frame end (`UNICAM_FEI`); V4L2 timestamp = frame start (`unicam.c:1004-1021`) |
| IRQ -> DQBUF wake-up | 0.1-1 ms | same | SCHED_FIFO capture thread, poll() |
| Convert | 1.5-2 ms sparse / 10 ms full | 1.1 / 4.4 ms | section 5.2 |
| Handoff wait for next index pulse | 0-100 ms (mean 50) | same | the disk is a 10 Hz display |
| Paint one revolution | 0-100 ms per pixel position | same | pixel n appears at n x 125 us |
| **Total, mean** | **about 135 ms** | **about 118 ms** | the two HDMI-dependent rows are 36 vs 19 ms |

The HDMI side contributes about 36 ms of the roughly 135 ms; the disk's own 100 ms period and the index-pulse wait
contribute 100 ms on average. Mode choice changes latency by about 17 ms; nothing in the capture path can change the
rest.

### 6.2 Design for the C++ capture module (RECOMMENDATION)

1. **Configure in-process with ioctls, no `v4l2-ctl` subprocesses:** `VIDIOC_S_EDID` (once), `VIDIOC_QUERY_DV_TIMINGS` ->
   `VIDIOC_S_DV_TIMINGS`, `VIDIOC_S_FMT` (UYVY), `VIDIOC_REQBUFS` (4-6 MMAP), `VIDIOC_QBUF` all, `VIDIOC_STREAMON`.
   Subscribe to `V4L2_EVENT_SOURCE_CHANGE` (`VIDIOC_SUBSCRIBE_EVENT`) and watch `POLLPRI`: on a change, `STREAMOFF`,
   re-query timings, `S_FMT`, `REQBUFS`, `STREAMON`. This replaces the 15 s `subprocess` timeouts in `capture.py:478`
   and the 1 s retry loop with event-driven recovery. Detect MC mode by `device_caps & V4L2_CAP_IO_MC` and, only then,
   send EDID/timings to the subdev.
2. **Dequeue every frame, immediately.** `poll()` on the fd; `VIDIOC_DQBUF`; check `V4L2_BUF_FLAG_ERROR` and
   `bytesused == sizeimage` (as `v4l2.py:229` does); keep at least 3 buffers queued at all times. The legacy driver
   drops into a dummy buffer when starved, so there is never a backlog, but every drop is a 33 ms hole.
3. **Convert in place, never copy the frame.** Read the sampled rows straight from the mmap pointer with 16/64-byte
   NEON loads (sequential within a row), accumulate the 32x25 (or 32x18 live) cell sums in registers/L1, write the
   800-byte frame, then `VIDIOC_QBUF`. Budget: about 2 ms per 1080p frame -> converting *every* frame is affordable
   (6% of one core) and gives the LED thread the freshest possible frame.
4. **Hand-off:** a lock-free "latest frame" slot (two or three 800-byte frames, an atomic index, a sequence counter).
   The LED thread picks the newest frame at each index pulse (frame swap at the pulse avoids tearing between
   revolutions); it never blocks on the capture thread. Record `buf.timestamp` (frame start, CLOCK_MONOTONIC) with the
   frame for latency telemetry ("age at pulse").
5. **Threads and cores:** LED modulator and index-sensor handler on an isolated core (`isolcpus=3 nohz_full=3
   rcu_nocbs=3`, `SCHED_FIFO`, mlockall); capture thread `SCHED_FIFO` low priority or `SCHED_OTHER` nice -5 on another
   core; pin the Unicam IRQ (`/proc/interrupts`, "unicam") away from the LED core. The capture path's memory traffic
   (about 125 MB/s DMA + about 25 MB/s of CPU reads) is under 6% of the bus and cannot starve the LED core.
6. **Convert every frame or every Nth?** Every frame at 30 fps costs 60 ms/s; every 2nd costs 30 ms/s and raises the mean
   frame age by 17 ms. Default to every frame; expose a divisor for 720p60.
7. **Zero-copy, stronger version (optional, section 6.3):** import cached dma-heap buffers so even full-frame filtering
   costs about 3 ms, or offload the downscale to the VideoCore ISP (next).

### 6.3 Two hardware-assisted options (FACTS on availability; performance is ESTIMATE/HYPOTHESIS)

**A. VideoCore ISP downscale, zero CPU pixel work.** The Pi docs' V4L2 node table: `/dev/video12` = "Simple ISP, can
perform conversion and resizing between RGB/YUV formats", `/dev/video13-16` = the fully programmable ISP
(`bcm2835-isp`). `drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c` (rpi-6.12.y): roles include
`"isp"` (line 105), `MIN_W 32`, `MIN_H 32`, `MAX_W_ISP 16384` (lines 121-126), format table has `V4L2_PIX_FMT_UYVY`
(line 215), `RGB24`/`BGR24` (244-251) and `V4L2_PIX_FMT_GREY` (line 541); both queues are `VB2_MMAP | VB2_DMABUF`
(lines 3244, 3258). The legacy Unicam node exports its buffers (`vb2_ioctl_expbuf`, line 1854). So a C++ pipeline can:
Unicam MMAP buffers -> `VIDIOC_EXPBUF` -> queue the dmabuf fd on the ISP's OUTPUT queue as `V4L2_MEMORY_DMABUF` ->
ISP CAPTURE queue set to e.g. `GREY` 128x100 (or 64x50 if allowed) -> CPU box-averages 4x4 (or 2x2) into 32x25 from
a 12.8 KB buffer. This is the exact Unicam->ISP dmabuf topology libcamera uses on the Pi 4, so the plumbing is
production-proven. **Unknowns (HYPOTHESIS):** the resizer's maximum downscale ratio (1920 -> 128 is 15x; if the limit
is lower, output 256x200 and average 8x8), the per-frame ISP latency (expect a few ms), and `GREY` output on the ISP
role (the format table lists it with no restriction flags). Bench check B10 answers all three with `v4l2-ctl` alone.

**B. Cached capture buffers via dma-heap.** `drivers/dma-buf/heaps/cma_heap.c` (rpi-6.12.y): `cma_heap_mmap` installs a
plain fault handler (`vmf_insert_pfn`) and does not alter `vm_page_prot` -> a normal *cacheable* mapping; cache
maintenance is done by `cma_heap_dma_buf_begin_cpu_access`/`end_cpu_access` (`dma_sync_sgtable_for_cpu/for_device`,
lines 121-156), which userspace triggers with `DMA_BUF_IOCTL_SYNC`. libcamera allocates exactly this way
(`src/libcamera/dma_buf_allocator.cpp`: `/dev/dma_heap/linux,cma` ("When the cma heap size is specified on the kernel
command line, this gets renamed to "reserved""), and calls `sync(DMA_BUF_SYNC_START)`/`SYNC_END` around CPU access,
lines 311-344). Unicam accepts `V4L2_MEMORY_DMABUF`. Expected full-frame read cost drops from about 9 ms to about 2 ms
(ESTIMATE, 5.2a). No Pi-specific "vidbuf_cached" heap exists in the Pi tree (code search: 0 hits). Only needed if the
port wants a true full-frame filter on the CPU; the sparse path does not need it.

---

## 7. Consolidated FACTS / ESTIMATES / HYPOTHESES

**FACTS (sources above)**
- F1 TC358743 outputs uncompressed CSI-2 (UYVY8_1X16 or 24-bit RGB/BGR); driver enumerates only those; no scaler, no firmware, EDID written to EDID RAM over I2C (`tc358743.c`).
- F2 Waveshare's H.264 comes from the Pi's VideoCore encoder (yavta `vc.ril.video_encode`, gst `v4l2h264enc`), applied after raw UYVY capture.
- F3 Pi 4 routes 2 of csi1's 4 lanes; default link 486 MHz = 972 Mbit/s/lane; driver lane rule `ceil(w*h*fps*bpp / bps_per_lane)`; docs: 2 lanes max 1080p30 RGB888 / 1080p50 YUV422; CM4 4-lane 1080p60; Pi 5 2 x 4-lane, rp1-cfe, MC mandatory.
- F4 On rpi-6.12.y and rpi-6.18.y the Pi 0-4 `tc358743` overlay sets `brcm,bcm2835-unicam-legacy` -> legacy driver, non-MC; `media-controller=1` -> same module in MC mode; the mainline driver binds only `brcm,bcm2835-unicam-upstream`, used by no Pi DT. Legacy node passes `S_EDID`/`QUERY_DV_TIMINGS`/`S_DV_TIMINGS` to the subdev; io_modes MMAP|DMABUF|READ; EXPBUF supported; drops to a dummy buffer when starved; timestamp at frame start.
- F5 Raspberry Pi OS Trixie: 6.12.47 (2025-10) ... 6.12.75 (2026-04), 6.18.34 (2026-06), 6.18.50 (2026-09/10); Legacy Bookworm 6.12. v4l-utils: bookworm 1.22.1, trixie 1.30.1; `--keep-edid-checksums` replaced `--fix-edid-checksums` (1.28+).
- F6 UYVY from this bridge is BT.601 limited (driver `VI_REP` = `601_YCBCR_LIMITED`, colorspace SMPTE170M); RGB mode is `RGB_FULL`/sRGB. `v4l2.py:318` uses BT.709 for >= 720 lines.
- F7 V4L2 MMAP buffers are uncached; Pi engineer measured about 40 ms per 18 MB (about 450 MB/s) on a Pi 4; cached memcpy about 2.7 GB/s.
- F8 HDCP disabled in the driver and blocked from `v4l2-dbg`; Waveshare: no HDCP.
- F9 `/dev/video12` ISP M2M: UYVY in, RGB24/BGR24/GREY out, min 32x32, DMABUF both queues. CMA dma-heap mappings are cacheable with explicit sync ioctls; libcamera uses them.
- F10 Shipped EDIDs: VIC 34 + VIC 4, 80 MHz pixel-clock cap, RGB-only, underscan; the Mac lists 1920x1080 @ 30 Hz with the 1080p30 file.

**ESTIMATES** (arithmetic in sections 2, 5, 6): lane shares; MB/s per mode; 9.2 ms uncached read per 1080p frame; about 2 ms sparse / about 10 ms full per frame in C++; about 6% / 30% of a core at 30 fps; about 2.7 ms full-frame with cached buffers; 4 buffers = 16.6 MB CMA; latency table.

**HYPOTHESES**: H1 the user's kernel is 6.18 (if updated since June 2026) rather than 6.12. H2 720p60 UYVY on the single lane the driver selects is clean (PiKVM ships it) but unverified on this board. H3 macOS accepts 720p60 from this EDID; 640x480 doubtful. H4 Mac sends limited-range RGB and the chip may double-limit black level. H5 ISP max downscale ratio and GREY output on the `isp` role. H6 DRM video is black on the Mac side (HDCP), desktop unaffected. H7 Exact `media-ctl` pad numbers in MC mode.

---

## 8. Bench checks (run on the Pi unless noted)

- **B1 OS/kernel/driver mode:** `uname -r; grep -E "^(PRETTY_NAME|VERSION_CODENAME)" /etc/os-release; v4l2-ctl --version;
  dpkg -l v4l-utils | tail -1; cat /proc/device-tree/soc/csi@7e801000/compatible | tr '\0' '\n';
  v4l2-ctl -d /dev/video0 -D; ls -l /dev/video* /dev/v4l-subdev* /dev/media* 2>/dev/null; media-ctl -d /dev/media0 -p`.
  Expect `brcm,bcm2835-unicam-legacy`, "Driver name: unicam", and no "I/O MC" in Device Caps. Confirms F4/H1.
- **B2 Bridge state:** `dmesg | grep -i -E "tc358743|unicam|cma"; v4l2-ctl -d /dev/video0 --log-status` (look for
  "Cable detected (+5V power)", "Hotplug enabled", "TMDS signal detected", "Stable sync signal", "HDCP encrypted content: no").
- **B3 EDID in chip:** `v4l2-ctl -d /dev/video0 --get-edid=pad=0,format=hex` after the service loaded it; `edid-decode` on the
  file (`apt install edid-decode`) to confirm VIC 34/4 and the 80 MHz limit.
- **B4 Timings:** `v4l2-ctl -d /dev/video0 --query-dv-timings` (expect 1920x1080p30 or 1280x720p60, pixelclock 74250000).
- **B5 Format:** `v4l2-ctl -d /dev/video0 -V` (UYVY, bytesperline 3840, sizeimage 4147200; "Colorspace: SMPTE 170M" confirms F6).
- **B6 Stream integrity (do for each EDID mode):** `v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=600 --stream-to=/dev/null --verbose 2>&1 | grep -c -i error; v4l2-ctl -d /dev/video0 --stream-mmap=4 --stream-count=600 --stream-to=/dev/null`
  (prints the achieved fps; sequence gaps mean drops). If 720p60 shows errors, add `,link-frequency=297000000` to the overlay and repeat.
- **B7 Mac side (on the MacBook):** `system_profiler SPDisplaysDataType | grep -A8 MechanicalTV` (resolution, "@ 30Hz"/"@ 60Hz",
  pixel format, "Television: Yes"); Displays settings, Option-click "Scaled" to list all modes offered for each EDID.
- **B8 Black/white levels (H4):** show a full-screen black window on the Mac, `v4l2-ctl -d /dev/video0 --stream-mmap --stream-count=1 --stream-to=/tmp/black.uyvy`,
  then a white one -> `/tmp/white.uyvy`; `python3 -I -c "import sys;d=open(sys.argv[1],'rb').read();y=d[1::2];print(min(y),max(y))" /tmp/black.uyvy`
  and the same for white. Expect about 16 / about 235 (limited). About 30 for black or about 219 for white means double limiting; 0/255 means full range.
- **B9 Uncached read cost (F7 on this box):** a 40-line C test: open `/dev/video0`, REQBUFS 4 MMAP, STREAMON, loop DQBUF ->
  `clock_gettime` -> `memcpy(cached, buf, 4147200)` -> `clock_gettime` -> QBUF; print mean ms. Expect about 9 ms at 1080p;
  then the same with only 200 rows copied (expect about 1.7 ms). Compare with `python3 -m mechanical_tv.app` CPU (`top -H`).
- **B10 ISP offload feasibility (H5):** `v4l2-ctl -d /dev/video12 --list-formats-out; v4l2-ctl -d /dev/video12 --list-formats;`
  `v4l2-ctl -d /dev/video12 --set-fmt-video-out=width=1920,height=1080,pixelformat=UYVY --set-fmt-video=width=128,height=100,pixelformat=GREY --stream-mmap --stream-out-mmap --stream-from=/tmp/white.uyvy --stream-to=/tmp/small.grey --stream-count=1 -V`
  (the `-V` shows what size the driver accepted; try 64x50 and 32x32 too; time 300 iterations with `--stream-count=300` for latency).
- **B11 CMA headroom:** `grep -i cma /proc/meminfo; dmesg | grep -i cma` (need > 17 MB free for 4 buffers; no `cma=96M` unless short).
- **B12 IRQ placement for the port:** `grep -i -E "unicam|csi" /proc/interrupts` (which CPU takes the frame interrupts) and
  `cat /sys/devices/system/cpu/isolated` (should list the LED core once `isolcpus` is set).

---

## 9. Sources

Primary (kernel, vendor, documentation):
- raspberrypi/linux `rpi-6.12.y` and `rpi-6.18.y`: `arch/arm/boot/dts/overlays/tc358743-overlay.dts`, `tc358743.dtsi`, `tc358743-pi5-overlay.dts`, `overlays/README`; `drivers/media/platform/bcm2835/bcm2835-unicam.c` + `Kconfig`; `drivers/media/platform/broadcom/bcm2835-unicam.c` + `Kconfig`; `drivers/media/i2c/tc358743.c`, `tc358743_regs.h`; `arch/arm64/configs/bcm2711_defconfig`; `arch/arm/boot/dts/broadcom/bcm283x.dtsi`; `drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c`; `drivers/staging/vc04_services/bcm2835-isp/bcm2835-v4l2-isp.c`; `drivers/dma-buf/heaps/cma_heap.c`; `drivers/media/common/videobuf2/videobuf2-dma-contig.c` (https://github.com/raspberrypi/linux).
- raspberrypi/linux PR #6795 "dtoverlays: Create Pi5 variant of tc358743 overlay" (merged 2025-04-25): https://github.com/raspberrypi/linux/pull/6795
- Raspberry Pi documentation, Camera software (Unicam, V4L2 node table, TC358743 section): https://www.raspberrypi.com/documentation/computers/camera_software.html
- Raspberry Pi OS release notes: https://downloads.raspberrypi.com/raspios_arm64/release_notes.txt ; download page: https://www.raspberrypi.com/software/operating-systems/
- Raspberry Pi 5 product page: https://www.raspberrypi.com/products/raspberry-pi-5/
- Waveshare wiki "HDMI to CSI Adapter": https://www.waveshare.com/wiki/HDMI_to_CSI_Adapter
- Toshiba TC358743XBG product page: https://toshiba.semicon-storage.com/us/semiconductor/product/interface-bridge-ics-for-mobile-peripheral-devices/hdmir-interface-bridge-ics/detail.TC358743XBG.html
- 6by9, "TC358743 HDMI to CSI-2 install instructions (Pi 0-4)", 2020-08-05: https://forums.raspberrypi.com/viewtopic.php?t=281972
- 6by9 et al., "Cannot open device /dev/video0 on Pi 5 HDMI to CSI (TC358743)", 2025-03/04: https://forums.raspberrypi.com/viewtopic.php?t=385709
- 6by9/yavta fork (MMAL H.264 encode after raw capture): https://raw.githubusercontent.com/6by9/yavta/master/yavta.c ; 6by9/raspi_tc358743 (deprecated note): https://github.com/6by9/raspi_tc358743
- picamera2 issue #740 (uncached V4L2 buffers, 40 ms per 18 MB): https://github.com/raspberrypi/picamera2/issues/740
- tinymembench Pi 4 (about 2.7 GB/s memcpy): https://github.com/geerlingguy/sbc-reviews/issues/4
- libcamera `src/libcamera/dma_buf_allocator.cpp` (dma-heap list, DMA_BUF_IOCTL_SYNC): https://github.com/libcamera-org/libcamera/blob/master/src/libcamera/dma_buf_allocator.cpp
- mainline `Documentation/admin-guide/media/raspberrypi-rp1-cfe.rst`: https://github.com/torvalds/linux/blob/master/Documentation/admin-guide/media/raspberrypi-rp1-cfe.rst
- v4l-utils `utils/v4l2-ctl/v4l2-ctl-edid.cpp` (`--keep-edid-checksums`): https://github.com/gjasny/v4l-utils/blob/master/utils/v4l2-ctl/v4l2-ctl-edid.cpp ; Debian package versions: https://packages.debian.org/search?keywords=v4l-utils
- PiKVM EDID docs (720p default on V3, 1080p50 limit): https://docs.pikvm.org/edid/

Repository code cited: `mechanical_tv/v4l2.py` (:38-39, :115, :159-199, :211-236, :266-300, :318, :337-369), `mechanical_tv/capture.py` (:351, :478, :656-697, :733-788), `packaging/make-edid.py` (:6-12, :50-51, :72, :90), `mechanical_tv/edid/1080p30.txt`, `720p60.txt`, `docs/INSTALL.md` (:54, :65, :80-88), `docs/HARDWARE.md` (:103), `docs/ARCHITECTURE.md` (:155).
