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
echo 'Installing Mechanical TV in SIMULATION mode. No GPIO or network settings will be changed.'
apt-get update
apt-get install -y python3 ffmpeg curl
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
if [[ ! -f /var/lib/mechanical-tv/auth.json ]]; then
  (cd "$release_dir" && runuser -u mechanical-tv -- python3 -m mechanical_tv.server init --data /var/lib/mechanical-tv)
fi
if [[ ! -f /etc/mechanical-tv/environment ]]; then
  printf '%s\n' 'MTV_DATA=/var/lib/mechanical-tv' 'MTV_HOST=0.0.0.0' 'MTV_PORT=8080' > /etc/mechanical-tv/environment
  chmod 0644 /etc/mechanical-tv/environment
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
systemctl daemon-reload
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
echo 'Mechanical TV is running in simulation mode.'
echo "Open http://$(hostname).local:$configured_port or http://<Pi-IP-address>:$configured_port"
echo 'No desktop login is required after reboot. See docs/INSTALL.md for hotspot setup.'
