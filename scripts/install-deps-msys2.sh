#!/usr/bin/env bash
# Installs Porthole's build dependencies on Windows.
# Run inside an "MSYS2 UCRT64" terminal (https://www.msys2.org):
#   ./scripts/install-deps-msys2.sh
set -euo pipefail

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    echo "Please run this from an MSYS2 UCRT64 terminal (current: ${MSYSTEM:-not MSYS2})." >&2
    exit 1
fi

pacman -S --needed --noconfirm \
    git \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-openssl \
    mingw-w64-ucrt-x86_64-libplist \
    mingw-w64-ucrt-x86_64-SDL2 \
    mingw-w64-ucrt-x86_64-ffmpeg \
    mingw-w64-ucrt-x86_64-libusb

echo "Done. Build the Wi-Fi mirror with: cmake --preset windows-release && cmake --build --preset windows-release"
