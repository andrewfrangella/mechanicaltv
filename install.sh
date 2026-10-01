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
echo 'Installing standalone HDMI conversion with preview output. No motor/LED or network configuration.'
apt-get update
apt-get install -y python3 ffmpeg v4l-utils
python3 -c 'import sys; assert sys.version_info >= (3,11), "Python 3.11+ required"'
if ! id mechanical-tv >/dev/null 2>&1; then
  useradd --system --home-dir /var/lib/mechanical-tv --shell /usr/sbin/nologin mechanical-tv
fi
install -d -m 0755 /opt/mechanical-tv/releases /etc/mechanical-tv
install -d -m 0700 -o mechanical-tv -g mechanical-tv /var/lib/mechanical-tv
release_dir="/opt/mechanical-tv/releases/$(date -u +%Y%m%dT%H%M%S)-$$"
install -d -m 0755 "$release_dir"
cp -R "$project_dir/mechanical_tv" "$release_dir/"
chmod -R a+rX "$release_dir"
# Preserve existing data and studio credentials; capture needs no password.
if [[ ! -f /etc/mechanical-tv/environment ]]; then
  printf '%s\n' 'MTV_DATA=/var/lib/mechanical-tv' 'MTV_CAPTURE_DEVICE=/dev/video0' 'MTV_CAPTURE_SIZE=640x480' 'MTV_CAPTURE_FPS=30' 'MTV_CAPTURE_FIT=fit' > /etc/mechanical-tv/environment
  chmod 0644 /etc/mechanical-tv/environment
fi
# Stop before backing up SQLite. Never copy a live database as an update backup.
systemctl stop mechanical-tv.service 2>/dev/null || true
if [[ -f /var/lib/mechanical-tv/media/library.sqlite3 ]]; then
  install -d -m 0700 /var/lib/mechanical-tv/backups
  cp /var/lib/mechanical-tv/media/library.sqlite3 "/var/lib/mechanical-tv/backups/library-$(date -u +%Y%m%dT%H%M%S).sqlite3"
fi
previous_release="$(readlink -f /opt/mechanical-tv/current 2>/dev/null || true)"
if [[ -f /etc/systemd/system/mechanical-tv.service ]]; then
  cp /etc/systemd/system/mechanical-tv.service "$release_dir/previous.service"
fi
rollback() {
  if [[ -n "$previous_release" && "$previous_release" != /opt/mechanical-tv/current && -d "$previous_release" && -f "$release_dir/previous.service" ]]; then
    ln -sfn "$previous_release" /opt/mechanical-tv/current
    install -m 0644 "$release_dir/previous.service" /etc/systemd/system/mechanical-tv.service
    systemctl daemon-reload
    systemctl restart mechanical-tv.service
    echo 'Restored previous code and service unit; inspect service health.' >&2
  fi
}
ln -sfn "$release_dir" /opt/mechanical-tv/current
install -m 0644 "$project_dir/packaging/mechanical-tv.service" /etc/systemd/system/mechanical-tv.service
systemctl daemon-reload
if ! systemctl enable --now mechanical-tv.service; then
  echo 'Service startup failed. Inspect: journalctl -u mechanical-tv -n 100' >&2
  rollback
  exit 1
fi
# This verifies service startup, not capture compatibility or HDMI signal presence.
sleep 2
if ! systemctl is-active --quiet mechanical-tv.service; then
  echo 'Startup failed. Inspect: journalctl -u mechanical-tv -n 100' >&2
  rollback
  exit 1
fi
echo 'HDMI capture service started. Missing devices are retried; no network or browser is required.'
echo 'Check /var/lib/mechanical-tv/capture/capture.json and journalctl -u mechanical-tv.'
echo 'See docs/INSTALL.md to select the device and verify its capture modes.'
