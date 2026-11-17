#!/usr/bin/env bash
set -euo pipefail

prefix="${PREFIX:-/usr/local}"
destdir="${DESTDIR:-}"
bin="${BIN:-./porthole}"
root="${destdir}${prefix}"
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

install -d "${root}/bin" "${root}/share/applications"
install -m 0755 "${bin}" "${root}/bin/porthole"
install -m 0644 "${script_dir}/porthole.desktop" "${root}/share/applications/porthole.desktop"
