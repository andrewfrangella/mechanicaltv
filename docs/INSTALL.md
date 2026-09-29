# Install and operate on a Raspberry Pi

This guide describes the provided simulation software. Hardware output remains disabled. Commands marked as configuration changes affect the Pi on which you run them, not the laptop hosting your browser.

## 1. Prepare Pi OS

Use [Raspberry Pi Imager](https://www.raspberrypi.com/software/) to write Raspberry Pi OS **64-bit**. Lite is sufficient when you use a laptop or phone browser; Desktop is useful for an attached monitor. Choose a supported release with Python 3.11+. Set:

- Hostname: `mechanical-tv` (recommended, not mandatory).
- Your own OS username/password; there is no assumed `pi` user.
- Country/timezone and home Wi-Fi credentials, or use Ethernet.
- SSH if you want remote maintenance, preferably with your public key.

Let Imager verify the card. Boot with the Pi kit's power supply. Initial installation needs internet. [Official setup instructions](https://www.raspberrypi.com/documentation/computers/getting-started.html).

## 2. Connect for installation

On the Pi's desktop, open Terminal; or from your laptop:

```bash
ssh YOUR_USERNAME@mechanical-tv.local
```

Replace `YOUR_USERNAME`. If `.local` discovery fails, find the Pi's IP in your router and use it instead. On a locally attached Pi terminal, `hostname -I` shows addresses.

## 3. Install the application

```bash
sudo apt update
sudo apt install -y git
git clone https://github.com/andrewfrangella/mechanicaltv.git
cd mechanicaltv
sudo bash install.sh
```

Read the installer before running it if desired. It:

1. Checks Pi OS/Debian and Python prerequisites.
2. Installs Python, FFmpeg and curl through apt.
3. Creates an unprivileged `mechanical-tv` service account.
4. Copies application files into a versioned `/opt/mechanical-tv/releases/` directory.
5. Prompts for an application password of at least 12 characters on first install.
6. Creates persistent data storage and the environment file.
7. Installs and starts `mechanical-tv.service`, enabled at boot.
8. Checks the local `/healthz` endpoint.

The release uses only Python's standard library, so there is no pip or virtual-environment step. It never starts a motor. It does not alter networking.

Open `http://mechanical-tv.local:8080` or `http://<Pi-IP>:8080`. Sign in using the **application** password. This is distinct from your OS and Wi-Fi passwords.

If the Pi runs a firewall, allow TCP 8080 only from the intended local network. No public router port-forwarding is required.

## 4. Verify the first installation

```bash
systemctl status mechanical-tv --no-pager
curl --fail http://127.0.0.1:8080/healthz
cd /opt/mechanical-tv/current
sudo -u mechanical-tv python3 -m mechanical_tv.server doctor --data /var/lib/mechanical-tv
```

`/healthz` checks that HTTP responds, not video conversion or hardware health. `doctor` checks dependencies and local setup. Follow [VALIDATION.md](VALIDATION.md) for an end-to-end check.

Generate a test clip from your cloned repository with `bash packaging/make-test-clip.sh /tmp/test-pattern.mp4`. If you generated it on the Pi, copy it to your laptop with `scp YOUR_USERNAME@mechanical-tv.local:/tmp/test-pattern.mp4 .` and upload it through Library.

Reboot once with `sudo reboot`. The application should return without any desktop login. Playback deliberately returns to idle.

## 5. Optional standalone Wi-Fi hotspot

This is an explicit manual setup, **not performed by the installer**. First verify the application over your home network. Use Ethernet or a directly attached keyboard/screen while changing Wi-Fi so you don't strand your only SSH connection. Creating a hotspot disconnects the Pi's existing Wi-Fi client connection.

The commands below assume NetworkManager, interface `wlan0`, and an unused `192.168.50.0/24` subnet. Check `nmcli device status` and choose another subnet if it conflicts with Ethernet or another connected network. Do not run the create command again if a profile named `mechanical-tv-hotspot` already exists.

```bash
sudo nmcli --ask device wifi hotspot ifname wlan0 con-name mechanical-tv-hotspot ssid MechanicalTV
sudo nmcli connection modify mechanical-tv-hotspot ipv4.addresses 192.168.50.1/24 ipv4.method shared ipv6.method disabled connection.autoconnect yes connection.autoconnect-priority 100
sudo nmcli connection up mechanical-tv-hotspot
```

NetworkManager will configure or generate hotspot credentials; inspect the active hotspot locally using `nmcli device wifi show-password` and keep those credentials private. Behavior can vary by OS/NetworkManager version; use the [official Raspberry Pi networking guide](https://www.raspberrypi.com/documentation/computers/configuration.html) for your release.

On your laptop or phone, join **MechanicalTV**, then visit **http://192.168.50.1:8080**. No internet is needed after installation. The client may display “No internet”; stay connected to this network. Ethernet can provide the Pi with internet for maintenance while Wi-Fi serves clients. Do not depend on simultaneous hotspot/client operation on the built-in Wi-Fi adapter.

To return to home Wi-Fi, from Ethernet or a local terminal:

```bash
sudo nmcli connection modify mechanical-tv-hotspot connection.autoconnect no
sudo nmcli connection down mechanical-tv-hotspot
sudo nmcli --ask device wifi connect YOUR_HOME_SSID
```

Replace the SSID. The browser address will change to the Pi's home-network address. A captive portal and automatic fallback timer are not implemented.

## Configuration and maintenance

Configuration: `/etc/mechanical-tv/environment`:

```text
MTV_DATA=/var/lib/mechanical-tv
MTV_HOST=0.0.0.0
MTV_PORT=8080
```

The service unit grants write access only to the standard data directory. Moving data requires a corresponding unit override. The default port is recommended; after changing configuration, restart the service and use the new address.

```bash
sudo systemctl restart mechanical-tv
journalctl -u mechanical-tv -n 100 --no-pager
journalctl -u mechanical-tv -f
```

Reset the application password:

```bash
cd /opt/mechanical-tv/current
sudo -u mechanical-tv python3 -m mechanical_tv.server reset-password --data /var/lib/mechanical-tv
sudo systemctl restart mechanical-tv
```

Existing sessions are invalidated by the restart. Sessions normally expire after 12 hours.

## Updates and backups

Stop playback first. From your cloned repository:

```bash
git pull --ff-only
sudo bash install.sh
```

The installer preserves media and passwords, retains previous application directories, and backs up the stopped SQLite database before switching versions. Its startup check is basic HTTP health; it is not a full regression test. A failed check restores the previous application directory when available. Database schema migration/automatic downgrade is not implemented in version 0.1.0.

For a complete backup, stop the service and copy `/var/lib/mechanical-tv` and `/etc/mechanical-tv` to another storage device, then start the service. Database-only update backups do not contain uploaded videos. Protect backups because they include password hashes and originals. Monitor and periodically remove old release/database backups manually after verifying a new version.

## Shutdown

Press **Stop** in the UI, then use a Pi terminal or SSH:

```bash
sudo shutdown -h now
```

Wait for the Pi to complete shutdown before disconnecting power. A web shutdown button is not part of this release. No reboot, package installation, networking, or shutdown privilege is granted to the web service.

## Remove the application

```bash
sudo systemctl disable --now mechanical-tv
```

This stops automatic operation and preserves your library. Delete installed files and the account separately only after backing up anything you want to keep. Network profiles created manually are independent of the application.
