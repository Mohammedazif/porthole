#!/usr/bin/env bash
# Installs everything needed to build and develop Porthole on Debian, Ubuntu or Linux Mint.
# Usage: sudo ./scripts/install-deps-linux.sh
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Please run with sudo: sudo $0" >&2
    exit 1
fi

packages=(
    # Toolchain
    build-essential cmake ninja-build pkg-config git
    # AirPlay receiver core (UxPlay lib): crypto, plists, mDNS
    libssl-dev libplist-dev libavahi-compat-libdnssd-dev
    # Video/audio decoding and the mirror window (Phase 1)
    libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
    # USB capture (Phase 4)
    libusb-1.0-0-dev
    # Bluetooth HID experiments on Linux (spikes S3/S5)
    bluez python3-dbus python3-gi
)

if ! apt-get update; then
    # Often caused by an unrelated third-party repository (e.g. a missing signing key);
    # the packages below come from the distribution's own repositories.
    echo "Warning: 'apt-get update' reported errors; continuing with the current package lists." >&2
fi
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
echo "Done. Build with: cmake --preset debug && cmake --build --preset debug"
