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
echo 'Installing HDMI capture preparation. Hardware output is disabled; network settings are unchanged.'
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
if [[ ! -f /etc/mechanical-tv/environment ]]; then
  printf '%s\n' 'MTV_DATA=/var/lib/mechanical-tv' 'MTV_CAPTURE_DEVICE=/dev/video0' 'MTV_CAPTURE_FIT=fit' > /etc/mechanical-tv/environment
  chmod 0644 /etc/mechanical-tv/environment
fi
usermod -a -G video mechanical-tv
# Stop before backing up SQLite. Never copy a live database as an update backup.
systemctl stop mechanical-tv.service 2>/dev/null || true
if [[ -f /var/lib/mechanical-tv/media/library.sqlite3 ]]; then
  install -d -m 0700 /var/lib/mechanical-tv/backups
  cp /var/lib/mechanical-tv/media/library.sqlite3 "/var/lib/mechanical-tv/backups/library-$(date -u +%Y%m%dT%H%M%S).sqlite3"
fi
previous_release="$(readlink -f /opt/mechanical-tv/current 2>/dev/null || true)"
ln -sfn "$release_dir" /opt/mechanical-tv/current
install -m 0644 "$project_dir/packaging/mechanical-tv.service" /etc/systemd/system/mechanical-tv.service
systemctl daemon-reload
systemctl enable --now mechanical-tv.service
if ! systemctl is-active --quiet mechanical-tv.service; then
  echo 'Startup failed. Inspect: journalctl -u mechanical-tv -n 100' >&2
  exit 1
fi
echo 'HDMI capture service started; this does not prove an HDMI signal is present.'
echo 'Inspect /var/lib/mechanical-tv/live/capture.json and journalctl -u mechanical-tv.'
echo 'No AP, browser, application password, or desktop login is required.'
