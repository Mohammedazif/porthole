#!/usr/bin/env bash
# Installs Porthole's build dependencies on Windows.
# Run inside an "MSYS2 UCRT64" terminal (https://www.msys2.org):
#   ./scripts/install-deps-msys2.sh
set -euo pipefail

if [[ "${MSYSTEM:-}" != "UCRT64" ]]; then
    echo "Please run this from an MSYS2 UCRT64 terminal (current: ${MSYSTEM:-not MSYS2})." >&2
    exit 1
fi

# WinRT stays in a separate managed helper; its self-contained bundle needs no
# installed .NET runtime on the user's PC. The build requires the .NET 10 SDK.
if ! command -v dotnet >/dev/null 2>&1 || ! dotnet --list-sdks | grep -q '^10\.'; then
    echo 'Install the .NET 10 SDK from https://dotnet.microsoft.com/download/dotnet/10.0, then reopen UCRT64.' >&2
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
