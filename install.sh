#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
  echo 'Run this installer with sudo.' >&2
  exit 1
fi
source /etc/os-release
if [[ "${ID:-}" != debian && "${ID:-}" != raspbian ]]; then
  echo 'This installer supports Raspberry Pi OS / Debian only. See README for local development.' >&2
  exit 1
fi
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ ! -t 0 ]]; then
  echo 'Run in an interactive terminal so the application password can be set.' >&2
  exit 1
fi

# Interface:
#   local (default) - the operator console runs on the Pi's own desktop (python3 -m mechanical_tv.app), started
#                     with the desktop session, full screen. No web server, password or browser.
#   web             - the password-protected HTTP service on port 8080 (python3 -m mechanical_tv.server).
# Choose with:  sudo MTV_UI=web bash install.sh
ui_mode="${MTV_UI:-local}"
case "$ui_mode" in
  local|web) ;;
  *) echo "MTV_UI must be 'local' or 'web' (got '$ui_mode')." >&2; exit 1 ;;
esac
# The desktop user who will run the local console: the account that ran sudo.
desktop_user="${SUDO_USER:-}"
if [[ "$ui_mode" == local && ( -z "$desktop_user" || "$desktop_user" == root ) ]]; then
  echo 'Local mode needs the desktop user: run  sudo bash install.sh  from that account (not as root).' >&2
  exit 1
fi

# Motor driver:
#   a4988 (default) - STEP/DIR driver (A4988 / DRV8825 / TMC2209) clocked by the Pi's hardware PWM.
#                     STEP = GPIO 18, DIR = GPIO 23, ENABLE = GPIO 24. See docs/INSTALL.md, section 'Motor driver'.
#   hat             - Adafruit DC & Stepper Motor HAT (M1/M2).
# Choose with:  sudo MTV_MOTOR_DRIVER=hat bash install.sh
motor_driver="${MTV_MOTOR_DRIVER:-a4988}"
case "$motor_driver" in
  a4988|hat) ;;
  *) echo "MTV_MOTOR_DRIVER must be 'a4988' or 'hat' (got '$motor_driver')." >&2; exit 1 ;;
esac

if [[ "$motor_driver" == a4988 ]]; then
  echo 'Installing Mechanical TV in REALTIME HARDWARE mode (A4988 stepper driver on hardware PWM, NEMA 17, LED sync).'
else
  echo 'Installing Mechanical TV in REALTIME HARDWARE mode (Adafruit Motor HAT, NEMA 17, LED sync).'
fi
apt-get update
apt-get install -y python3 ffmpeg curl python3-pip python3-smbus i2c-tools v4l-utils || true
# Hardware libraries, installed one at a time so a missing package can't block the others
for pkg in python3-lgpio python3-smbus2 python3-tk python3-numpy; do
  apt-get install -y "$pkg" || true
done
python3 -c 'import sys; assert sys.version_info >= (3,11), "Python 3.11+ required"'
if ! id mechanical-tv >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/mechanical-tv --shell /usr/sbin/nologin mechanical-tv
fi
# Add service user to hardware device groups
usermod -a -G i2c,gpio,video mechanical-tv 2>/dev/null || true
if [[ "$ui_mode" == local ]]; then
  # The desktop user drives the hardware directly from the console window.
  usermod -a -G i2c,gpio,video,spi "$desktop_user" 2>/dev/null || true
fi
# Enable I2C interface on Raspberry Pi if raspi-config is present
if command -v raspi-config >/dev/null 2>&1; then
  raspi-config nonint do_i2c 0 2>/dev/null || true
fi

# Boot settings for the A4988 (only added if missing):
#   dtparam=audio=off            headphone audio shares the PWM hardware
#   dtoverlay=pwm,pin=18,func=2  hardware PWM on GPIO 18 drives the STEP pin
#   gpio=24=op,dh                ENABLE held high from power-on: motor stays off until the app starts it
reboot_needed=false
if [[ "$motor_driver" == a4988 ]]; then
  boot_config=/boot/firmware/config.txt
  [[ -f "$boot_config" ]] || boot_config=/boot/config.txt
  missing=()
  for line in 'dtparam=audio=off' 'dtoverlay=pwm,pin=18,func=2' 'gpio=24=op,dh'; do
    grep -qxF "$line" "$boot_config" || missing+=("$line")
  done
  if (( ${#missing[@]} > 0 )); then
    cp "$boot_config" "$boot_config.mechanical-tv.bak"
    {
      echo
      echo '# Mechanical TV: A4988 stepper driver (added by install.sh)'
      echo '[all]'
      printf '%s\n' "${missing[@]}"
    } >> "$boot_config"
    reboot_needed=true
  fi
fi

# An HDMI-to-CSI-2 adapter (TC358743) needs its kernel overlay. Print guidance; the HDMI adapter type cannot be
# detected before the overlay is loaded, so unlike the motor settings above this is never added automatically.
hdmi_boot_config=''
for candidate in /boot/firmware/config.txt /boot/config.txt; do
  if [[ -f "$candidate" ]]; then hdmi_boot_config="$candidate"; break; fi
done
if [[ -n "$hdmi_boot_config" ]] && ! grep -Eq '^[[:space:]]*dtoverlay=tc358743' "$hdmi_boot_config"; then
  cat <<NOTICE
NOTE: $hdmi_boot_config has no dtoverlay=tc358743. If the HDMI input is an HDMI-to-CSI-2 adapter
(GeeekPi/52Pi, Geekworm, Waveshare, Auvidea B101) add these lines to it and reboot:
  camera_auto_detect=0
  dtoverlay=tc358743
USB HDMI capture dongles need no change. See docs/INSTALL.md, section 'HDMI input'.
NOTICE
fi

install -d -m 0755 /opt/mechanical-tv/releases /etc/mechanical-tv
install -d -m 0700 -o mechanical-tv -g mechanical-tv /var/lib/mechanical-tv
release_dir="/opt/mechanical-tv/releases/$(date -u +%Y%m%dT%H%M%S)-$$"
install -d -m 0755 "$release_dir"
cp -R "$project_dir/mechanical_tv" "$release_dir/"
chmod -R a+rX "$release_dir"
if [[ ! -f /var/lib/mechanical-tv/auth.json ]]; then
  (cd "$release_dir" && runuser -u mechanical-tv -- python3 -m mechanical_tv.server init --data /var/lib/mechanical-tv)
fi
if [[ ! -f /etc/mechanical-tv/environment ]]; then
  printf '%s\n' 'MTV_DATA=/var/lib/mechanical-tv' 'MTV_HOST=0.0.0.0' 'MTV_PORT=8080' \
    'MTV_HDMI_DEVICE=/dev/video0' '# MTV_DEFAULT_SOURCE=hdmi starts capture at boot (web default: pattern; local console: hdmi when the device exists)' \
    '# MTV_HDMI_BRIDGE=auto|csi|usb  MTV_HDMI_FORMAT=auto|uyvy422|mjpeg|none  MTV_HDMI_EDID=auto|none|/path  MTV_HDMI_FIT=fit|crop' \
    '# MTV_HDMI_BACKEND=auto|v4l2|ffmpeg|opencv  (auto: in-process V4L2 reader for the CSI bridge, FFmpeg otherwise)' \
    > /etc/mechanical-tv/environment
  chmod 0644 /etc/mechanical-tv/environment
fi
# The motor driver choice must reach the local console too (the web service also gets it from its drop-in).
if grep -q '^MTV_MOTOR_DRIVER=' /etc/mechanical-tv/environment; then
  sed -i "s/^MTV_MOTOR_DRIVER=.*/MTV_MOTOR_DRIVER=$motor_driver/" /etc/mechanical-tv/environment
else
  printf 'MTV_MOTOR_DRIVER=%s\n' "$motor_driver" >> /etc/mechanical-tv/environment
fi
# Stop before backing up SQLite. Never copy a live database as an update backup.
systemctl stop mechanical-tv.service 2>/dev/null || true
if [[ -f /var/lib/mechanical-tv/media/library.sqlite3 ]]; then
  install -d -m 0700 /var/lib/mechanical-tv/backups
  cp /var/lib/mechanical-tv/media/library.sqlite3 "/var/lib/mechanical-tv/backups/library-$(date -u +%Y%m%dT%H%M%S).sqlite3"
fi
previous_release="$(readlink -f /opt/mechanical-tv/current 2>/dev/null || true)"
ln -sfn "$release_dir" /opt/mechanical-tv/current
install -m 0644 "$project_dir/packaging/mechanical-tv.service" /etc/systemd/system/mechanical-tv.service

# Hardware settings for the service, managed by this installer (rewritten on every install).
dropin_dir=/etc/systemd/system/mechanical-tv.service.d
install -d -m 0755 "$dropin_dir"
# Hand-made overrides from earlier troubleshooting are replaced by 10-hardware.conf
rm -f "$dropin_dir/lgpio.conf" "$dropin_dir/motor.conf" "$dropin_dir/a4988.conf"
{
  echo '[Service]'
  echo '# Managed by install.sh - edits here are overwritten on the next install.'
  echo '# lgpio creates helper files in its working directory; the app directory is read-only.'
  echo 'Environment=LG_WD=/tmp'
  if [[ "$motor_driver" == a4988 ]]; then
    echo 'Environment=MTV_MOTOR_DRIVER=a4988'
    echo '# Export the hardware PWM channel (STEP pin) as root and let the service user drive it.'
    echo "ExecStartPre=+/bin/sh -c 'test -d /sys/class/pwm/pwmchip0/pwm0 || echo 0 > /sys/class/pwm/pwmchip0/export; sleep 0.5; chgrp gpio /sys/class/pwm/pwmchip0/pwm0/period /sys/class/pwm/pwmchip0/pwm0/duty_cycle /sys/class/pwm/pwmchip0/pwm0/enable; chmod g+rw /sys/class/pwm/pwmchip0/pwm0/period /sys/class/pwm/pwmchip0/pwm0/duty_cycle /sys/class/pwm/pwmchip0/pwm0/enable'"
  fi
} > "$dropin_dir/10-hardware.conf"
chmod 0644 "$dropin_dir/10-hardware.conf"
systemctl daemon-reload

# ---------------------------------------------------------------------------------------------
# Local console (default): a desktop launcher plus an autostart entry for the desktop user, and the
# web service disabled so the two never fight over /dev/video0, the GPIOs and the PWM channel.
# ---------------------------------------------------------------------------------------------
if [[ "$ui_mode" == local ]]; then
  systemctl disable --now mechanical-tv.service 2>/dev/null || true
  install -m 0644 "$project_dir/packaging/mechanical-tv.desktop" /usr/share/applications/mechanical-tv.desktop
  desktop_home="$(getent passwd "$desktop_user" | cut -d: -f6)"
  if [[ "${MTV_AUTOSTART:-1}" == 1 && -d "$desktop_home" ]]; then
    install -d -m 0755 -o "$desktop_user" -g "$desktop_user" "$desktop_home/.config" "$desktop_home/.config/autostart"
    install -m 0644 -o "$desktop_user" -g "$desktop_user" "$project_dir/packaging/mechanical-tv.desktop" "$desktop_home/.config/autostart/mechanical-tv.desktop"
  fi
  # Verify the console can import its engine and Tk without opening a window.
  if ! (cd /opt/mechanical-tv/current && runuser -u "$desktop_user" -- /usr/bin/python3 -m mechanical_tv.app --check --env-file /etc/mechanical-tv/environment > /tmp/mechanical-tv-check.json); then
    echo 'Console check failed (is python3-tk installed?). Report: /tmp/mechanical-tv-check.json' >&2
    cat /tmp/mechanical-tv-check.json >&2 || true
    if [[ -n "$previous_release" && "$previous_release" != /opt/mechanical-tv/current && -d "$previous_release" ]]; then
      ln -sfn "$previous_release" /opt/mechanical-tv/current
      echo 'Restored the previous application directory.' >&2
    fi
    exit 1
  fi
  if [[ "$reboot_needed" == true ]]; then
    echo "Added A4988 boot settings to $boot_config (backup: $boot_config.mechanical-tv.bak)."
    echo 'Reboot to finish:  sudo reboot'
    echo 'The Mechanical TV console opens full screen when the desktop starts.'
    exit 0
  fi
  echo "Mechanical TV console installed (motor driver: $motor_driver)."
  echo "It opens full screen with the desktop. Start it now from the menu (Sound & Video > Mechanical TV) or with:"
  echo "  python3 -m mechanical_tv.app --env-file /etc/mechanical-tv/environment"
  echo "Run from /opt/mechanical-tv/current. F11 full screen, F12 debug log, Ctrl+Q quit. Log file: ~/.local/share/mechanical-tv/logs/"
  echo "Log out and back in once so the new hardware group memberships (gpio, i2c, video, spi) apply."
  echo "Web interface instead:  sudo MTV_UI=web bash install.sh"
  exit 0
fi

# Remove a local-mode autostart entry so the web service alone owns the hardware.
if [[ -n "$desktop_user" && "$desktop_user" != root ]]; then
  desktop_home="$(getent passwd "$desktop_user" | cut -d: -f6)"
  rm -f "$desktop_home/.config/autostart/mechanical-tv.desktop"
fi

if [[ "$reboot_needed" == true ]]; then
  systemctl enable mechanical-tv.service
  echo "Added A4988 boot settings to $boot_config (backup: $boot_config.mechanical-tv.bak)."
  echo 'Reboot to finish:  sudo reboot'
  echo 'Mechanical TV starts automatically after the reboot.'
  exit 0
fi

systemctl enable --now mechanical-tv.service
configured_port="$(sed -n 's/^MTV_PORT=//p' /etc/mechanical-tv/environment)"
if [[ ! "$configured_port" =~ ^[0-9]+$ ]]; then
  echo 'MTV_PORT must be a numeric port in /etc/mechanical-tv/environment.' >&2
  exit 1
fi
healthy=false
for attempt in {1..20}; do
  if curl --fail --silent --max-time 2 "http://127.0.0.1:$configured_port/healthz" >/dev/null; then
    healthy=true
    break
  fi
  sleep 1
done
if [[ "$healthy" != true ]]; then
  echo 'Startup check failed. Inspect: journalctl -u mechanical-tv -n 100' >&2
  if [[ -n "$previous_release" && "$previous_release" != /opt/mechanical-tv/current && -d "$previous_release" ]]; then
    ln -sfn "$previous_release" /opt/mechanical-tv/current
    systemctl restart mechanical-tv.service
    echo 'Restored the previous application directory. Check service health before use.' >&2
  fi
  exit 1
fi
echo "Mechanical TV is running in REALTIME mode (motor driver: $motor_driver)."
echo "Open http://$(hostname).local:$configured_port or http://<Pi-IP-address>:$configured_port"
echo "Select 'Live HDMI Video (capture adapter)' in the page to start capture, or set MTV_DEFAULT_SOURCE=hdmi in /etc/mechanical-tv/environment."
echo 'See docs/INSTALL.md, section HDMI input.'
