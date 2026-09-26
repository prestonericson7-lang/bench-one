# Offline packages for the Orange Pi

The stock Orange Pi 4 Pro image (Ubuntu 26.04 "resolute", 1.1.0) lacks two packages the accelerators
need: `nbd-client` (the Zynq's RAM as swap) and `teensy-loader-cli` (flashing the geometry engine).
The Pi's only Ethernet port goes to the Zynq, so it may have no internet at install time. These are
the official arm64 builds, so `install_pi.sh` installs them with `dpkg -i` and needs no network.

| Package | Version | Depends (all present on the stock image) |
|---|---|---|
| nbd-client | 1:3.26.1-6.1ubuntu2 | libc6 ≥ 2.38, libgnutls30t64 ≥ 3.8.1 (3.8.12 there), libnl-3-200 / libnl-genl-3-200 ≥ 3.11.0 (3.12.0 there), debconf |
| teensy-loader-cli | 2.2-1.1build1 | libc6 ≥ 2.38, libusb-0.1-4 ≥ 2:0.1.12 (2:0.1.12-35build2 there) |

Provenance, 2026-09-25: `http://ports.ubuntu.com/ubuntu-ports`, suites resolute / -updates /
-security, main + universe. `InRelease` passed `gpgv` against `ubuntu-archive-keyring.gpg` (Ubuntu
Archive Automatic Signing Key (2018)). The signed `Packages.xz` hash matched, and each `.deb`
matched its `SHA256:` in that index (see `SHA256SUMS`). The versions installed on the image were read
from its `/var/lib/dpkg/status`.
