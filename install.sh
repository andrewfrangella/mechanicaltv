#!/usr/bin/env bash
# Mechanical TV installer for Raspberry Pi OS (64-bit, Bookworm or Trixie) on a Raspberry Pi 4.
#
# Builds the C++ desktop application on the Pi, installs it under /opt/mechanical-tv, adds a desktop
# icon, a menu entry (Sound & Video > Mechanical TV) and an autostart entry for the account that ran
# sudo, and applies the boot and realtime settings the motor, index sensor and LED timing need.
#
#   sudo bash install.sh
#
# Choices (environment variables):
#   MTV_MOTOR_DRIVER=a4988|hat   A4988-style STEP/DIR driver on hardware PWM (default) or the Adafruit Motor HAT
#   MTV_LED_OUTPUT=gpio|spi      LED from GPIO 21 bit-banged (default, original wiring) or SPI0 MOSI / GPIO 10 by DMA (recommended)
#   MTV_ISOLATE_CPU=1|0          reserve CPU 3 for the realtime LED thread (default 1)
#   MTV_AUTOSTART=1|0            open the console when the desktop starts (default 1)
#   MTV_RUN_TESTS=1|0            run the test suite after building (default 1)
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
  echo 'Run this installer with sudo.' >&2
  exit 1
fi
source /etc/os-release
if [[ "${ID:-}" != debian && "${ID:-}" != raspbian ]]; then
  echo 'This installer supports Raspberry Pi OS / Debian only. See README for building on a development computer.' >&2
  exit 1
fi
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
desktop_user="${SUDO_USER:-}"
if [[ -z "$desktop_user" || "$desktop_user" == root ]]; then
  echo 'Run  sudo bash install.sh  from the desktop account that will operate the TV (not as root).' >&2
  exit 1
fi
desktop_home="$(getent passwd "$desktop_user" | cut -d: -f6)"

# On a re-run, keep the choices made last time unless they are given again.
env_file=/etc/mechanical-tv/environment
previous_setting() { if [[ -f "$env_file" ]]; then sed -n "s/^$1=//p" "$env_file" | tail -n1; fi; }
motor_driver="${MTV_MOTOR_DRIVER:-$(previous_setting MTV_MOTOR_DRIVER)}"
motor_driver="${motor_driver:-a4988}"
case "$motor_driver" in a4988|hat) ;; *) echo "MTV_MOTOR_DRIVER must be 'a4988' or 'hat' (got '$motor_driver')." >&2; exit 1 ;; esac
led_output="${MTV_LED_OUTPUT:-$(previous_setting MTV_LED_OUTPUT)}"
led_output="${led_output:-gpio}"
case "$led_output" in gpio|spi) ;; *) echo "MTV_LED_OUTPUT must be 'gpio' or 'spi' (got '$led_output')." >&2; exit 1 ;; esac

echo "Installing Mechanical TV (motor driver: $motor_driver, LED output: $led_output)."

# ---------------------------------------------------------------------------------------------
# Packages: compiler and Qt to build the console; FFmpeg for library conversion and the USB
# capture fallback; v4l-utils and gpiod for bench checks.
# ---------------------------------------------------------------------------------------------
apt-get update
apt-get install -y build-essential cmake ninja-build pkg-config qt6-base-dev libsqlite3-dev \
  ffmpeg v4l-utils i2c-tools gpiod
for pkg in rt-tests edid-decode; do apt-get install -y "$pkg" || true; done
if command -v raspi-config >/dev/null 2>&1 && [[ "$motor_driver" == hat ]]; then
  raspi-config nonint do_i2c 0 2>/dev/null || true
fi

# Hardware access without root: the console runs as the desktop user.
for group in gpio i2c video spi; do
  getent group "$group" >/dev/null && usermod -a -G "$group" "$desktop_user"
done

# ---------------------------------------------------------------------------------------------
# Build and install a versioned release.
# ---------------------------------------------------------------------------------------------
build_dir="$(mktemp -d /tmp/mechanical-tv-build.XXXXXX)"
release_dir="/opt/mechanical-tv/releases/$(date -u +%Y%m%dT%H%M%S)-$$"
chown "$desktop_user" "$build_dir"
runuser -u "$desktop_user" -- cmake -S "$project_dir" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$release_dir" -DMTV_SHARE_DIR="$release_dir/share/mechanical-tv"
runuser -u "$desktop_user" -- cmake --build "$build_dir" -j"$(nproc)"
if [[ "${MTV_RUN_TESTS:-1}" == 1 ]]; then
  if ! runuser -u "$desktop_user" -- env QT_QPA_PLATFORM=offscreen "$build_dir/mtv_tests"; then
    echo "The test suite failed; nothing was installed. Build directory kept at $build_dir" >&2
    exit 1
  fi
fi
install -d -m 0755 /opt/mechanical-tv/releases /etc/mechanical-tv
cmake --install "$build_dir"
chmod -R a+rX "$release_dir"
previous_release="$(readlink -f /opt/mechanical-tv/current 2>/dev/null || true)"
[[ -x "$previous_release/bin/mechanical-tv" ]] || previous_release=''   # releases before 0.3 were Python
ln -sfn "$release_dir" /opt/mechanical-tv/current
rm -rf "$build_dir"
# Keep the three newest releases for rollback.
ls -1dt /opt/mechanical-tv/releases/*/ 2>/dev/null | tail -n +4 | while read -r old; do
  [[ "$(readlink -f "$old")" == "$(readlink -f /opt/mechanical-tv/current)" ]] || rm -rf "$old"
done

# ---------------------------------------------------------------------------------------------
# Settings file shared by every launch path.
# ---------------------------------------------------------------------------------------------
if [[ ! -f "$env_file" ]]; then
  cat > "$env_file" <<'ENV'
# Mechanical TV settings: one KEY=value per line, read by the console at start (see docs/INSTALL.md).
MTV_HDMI_DEVICE=/dev/video0
# MTV_DEFAULT_SOURCE=auto|pattern|hdmi   (auto: Live HDMI when the capture device exists)
# MTV_HDMI_BRIDGE=auto|csi|usb  MTV_HDMI_FORMAT=auto|uyvy422|mjpeg|none  MTV_HDMI_EDID=auto|none|/path  MTV_HDMI_FIT=fit|crop
# MTV_HDMI_BACKEND=auto|v4l2|ffmpeg
# MTV_MICROSTEPS=16   must match the driver's MS1/MS2/MS3 jumpers (use Check microstepping in the console)
# MTV_RAMP_RATE=2.0   FPS gained per second while spinning up; lower it (e.g. 0.5) for a heavy disk
# MTV_START_FPS=0.02  speed of the first steps after the driver is enabled
ENV
fi
# Settings of the retired web service mean nothing to the console.
sed -i -E 's/^(MTV_HOST=|MTV_PORT=|MTV_DATA=\/var\/lib\/mechanical-tv)/# (web service, retired) \1/' "$env_file"
set_env() {
  if grep -q "^$1=" "$env_file"; then sed -i "s|^$1=.*|$1=$2|" "$env_file"; else printf '%s=%s\n' "$1" "$2" >> "$env_file"; fi
}
set_env MTV_MOTOR_DRIVER "$motor_driver"
set_env MTV_LED_OUTPUT "$led_output"
chmod 0644 "$env_file"

# ---------------------------------------------------------------------------------------------
# Boot configuration (only lines that are missing are added; a backup is kept next to the file).
# ---------------------------------------------------------------------------------------------
boot_config=/boot/firmware/config.txt
[[ -f "$boot_config" ]] || boot_config=/boot/config.txt
cmdline=/boot/firmware/cmdline.txt
[[ -f "$cmdline" ]] || cmdline=/boot/cmdline.txt
reboot_needed=false
wanted=()
if [[ "$motor_driver" == a4988 ]]; then
  #   dtparam=audio=off            headphone audio shares the PWM hardware
  #   dtoverlay=pwm,pin=18,func=2  hardware PWM on GPIO 18 drives the STEP pin
  #   gpio=24=op,dh                ENABLE held high from power-on: the motor stays off until the app starts it
  wanted+=('dtparam=audio=off' 'dtoverlay=pwm,pin=18,func=2' 'gpio=24=op,dh')
fi
if [[ "$motor_driver" == hat ]]; then
  #   dtparam=i2c_arm_baudrate=400000   one I2C write per step: 100 kHz allows only ~750 steps/s (about 2 FPS)
  wanted+=('dtparam=i2c_arm=on' 'dtparam=i2c_arm_baudrate=400000')
fi
if [[ "$led_output" == spi ]]; then
  #   dtparam=spi=on       /dev/spidev0.0 for the LED bitstream (MOSI = GPIO 10, pin 19)
  #   core_freq_fixed=1    the SPI clock is divided from the core clock; a scaling core clock would change the pixel timing
  wanted+=('dtparam=spi=on' 'core_freq_fixed=1')
fi
if [[ -f "$boot_config" && ${#wanted[@]} -gt 0 ]]; then
  missing=()
  for line in "${wanted[@]}"; do grep -qxF "$line" "$boot_config" || missing+=("$line"); done
  if (( ${#missing[@]} > 0 )); then
    cp "$boot_config" "$boot_config.mechanical-tv.bak"
    { echo; echo '# Mechanical TV (added by install.sh)'; echo '[all]'; printf '%s\n' "${missing[@]}"; } >> "$boot_config"
    reboot_needed=true
  fi
fi
# Kernel command line (one line): keep CPU 3 for the realtime LED thread, and let one spidev transfer
# carry a whole revolution.
if [[ -f "$cmdline" ]]; then
  args=()
  if [[ "${MTV_ISOLATE_CPU:-1}" == 1 ]]; then args+=('isolcpus=3' 'irqaffinity=0-2'); fi
  if [[ "$led_output" == spi ]]; then args+=('spidev.bufsiz=65536'); fi
  add=()
  for a in "${args[@]}"; do grep -qw -- "${a%%=*}" "$cmdline" || add+=("$a"); done
  if (( ${#add[@]} > 0 )); then
    cp "$cmdline" "$cmdline.mechanical-tv.bak"
    sed -i "1 s|\$| ${add[*]}|" "$cmdline"
    reboot_needed=true
  fi
  if [[ "${MTV_ISOLATE_CPU:-1}" == 0 ]] && grep -qE '(^| )(isolcpus=3|irqaffinity=0-2)( |$)' "$cmdline"; then
    cp "$cmdline" "$cmdline.mechanical-tv.bak"
    sed -i -E '1 s/ ?isolcpus=3//; 1 s/ ?irqaffinity=0-2//' "$cmdline"
    reboot_needed=true
  fi
fi
if [[ -n "$boot_config" ]] && [[ -f "$boot_config" ]] && ! grep -Eq '^[[:space:]]*dtoverlay=tc358743' "$boot_config"; then
  cat <<NOTICE
NOTE: $boot_config has no dtoverlay=tc358743. For the HDMI-to-CSI-2 adapter (GeeekPi/52Pi, Geekworm,
Waveshare, Auvidea B101) add these lines and reboot:
  camera_auto_detect=0
  dtoverlay=tc358743
USB HDMI capture dongles need no change. See docs/INSTALL.md, section 'HDMI input'.
NOTICE
fi

# ---------------------------------------------------------------------------------------------
# Realtime permissions and boot-time tuning.
# ---------------------------------------------------------------------------------------------
cat > /etc/security/limits.d/90-mechanical-tv.conf <<'LIMITS'
# Mechanical TV: the console's LED/index thread runs SCHED_FIFO and locks its memory.
@gpio  -  rtprio   80
@gpio  -  memlock  unlimited
LIMITS
if [[ "$led_output" == gpio ]]; then
  # The bit-banged LED thread spins for the whole revolution on CPU 3; without this the kernel pauses
  # realtime threads for 50 ms every second (a dark band on the disk). Other CPUs are unaffected.
  echo 'kernel.sched_rt_runtime_us = -1' > /etc/sysctl.d/90-mechanical-tv.conf
  sysctl -q -p /etc/sysctl.d/90-mechanical-tv.conf || true
elif [[ -f /etc/sysctl.d/90-mechanical-tv.conf ]]; then
  rm -f /etc/sysctl.d/90-mechanical-tv.conf
  sysctl -q -w kernel.sched_rt_runtime_us=950000 || true
fi
install -m 0644 "$project_dir/packaging/mechanical-tv-hw.service" /etc/systemd/system/mechanical-tv-hw.service
install -m 0755 "$project_dir/packaging/mechanical-tv-hw-setup" /opt/mechanical-tv/mechanical-tv-hw-setup

# The web service of versions before 0.3 is retired: the console owns the hardware.
if systemctl list-unit-files mechanical-tv.service >/dev/null 2>&1; then
  systemctl disable --now mechanical-tv.service 2>/dev/null || true
  rm -f /etc/systemd/system/mechanical-tv.service
  rm -rf /etc/systemd/system/mechanical-tv.service.d
fi
systemctl daemon-reload
systemctl enable mechanical-tv-hw.service
systemctl start mechanical-tv-hw.service || true

# ---------------------------------------------------------------------------------------------
# Desktop application.
# ---------------------------------------------------------------------------------------------
install -m 0644 "$project_dir/packaging/mechanical-tv.desktop" /usr/share/applications/mechanical-tv.desktop
install -D -m 0644 "$project_dir/packaging/mechanical-tv.png" /usr/share/icons/hicolor/256x256/apps/mechanical-tv.png
gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
if [[ -d "$desktop_home/Desktop" ]]; then
  install -m 0755 -o "$desktop_user" -g "$desktop_user" "$project_dir/packaging/mechanical-tv.desktop" "$desktop_home/Desktop/mechanical-tv.desktop"
fi
if [[ "${MTV_AUTOSTART:-1}" == 1 && -d "$desktop_home" ]]; then
  install -d -m 0755 -o "$desktop_user" -g "$desktop_user" "$desktop_home/.config" "$desktop_home/.config/autostart"
  install -m 0644 -o "$desktop_user" -g "$desktop_user" "$project_dir/packaging/mechanical-tv.desktop" "$desktop_home/.config/autostart/mechanical-tv.desktop"
else
  rm -f "$desktop_home/.config/autostart/mechanical-tv.desktop"
fi

if ! runuser -u "$desktop_user" -- /opt/mechanical-tv/current/bin/mechanical-tv --check --env-file "$env_file" > /tmp/mechanical-tv-check.json; then
  echo 'Console check failed. Report: /tmp/mechanical-tv-check.json' >&2
  cat /tmp/mechanical-tv-check.json >&2 || true
  if [[ -n "$previous_release" && -d "$previous_release" ]]; then
    ln -sfn "$previous_release" /opt/mechanical-tv/current
    echo 'Restored the previous release.' >&2
  fi
  exit 1
fi

echo "Mechanical TV $(/opt/mechanical-tv/current/bin/mechanical-tv --version | awk '{print $3}') installed in $release_dir."
echo "Start it from the desktop icon, the menu (Sound & Video > Mechanical TV) or with:"
echo "  /opt/mechanical-tv/current/bin/mechanical-tv --env-file $env_file"
echo "F11 full screen, F12 debug log, Ctrl+Q quit. Log file: ~/.local/share/mechanical-tv/logs/mechanical-tv.log"
if [[ "$reboot_needed" == true ]]; then
  echo "Boot settings were added ($boot_config, $cmdline; backups *.mechanical-tv.bak)."
  echo 'Reboot to finish:  sudo reboot   (the console opens full screen with the desktop)'
else
  echo "Log out and back in once so the hardware groups and realtime limits apply."
fi
