#!/usr/bin/env bash
set -euo pipefail

prefix="${PREFIX:-/usr/local}"
destdir="${DESTDIR:-}"
root="${destdir}${prefix}"

rm -f "${root}/bin/porthole" "${root}/share/applications/porthole.desktop"
