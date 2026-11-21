#!/usr/bin/env bash
# Portable Wi-Fi mirror bundle; run inside MSYS2 UCRT64 after a clean release build.
# Matching source archives are written beside the binary bundle for distribution.
set -euo pipefail

if [[ ${MSYSTEM:-} != UCRT64 ]]; then
    echo 'Run this script from an MSYS2 UCRT64 terminal.' >&2
    exit 1
fi
repo_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-"$repo_root/build/windows-release/src/porthole.exe"}
package_dir=${2:-"$repo_root/out/porthole-windows-x64"}
source_dir="${package_dir}-sources"
[[ -f "$binary" ]] || { echo "Missing binary: $binary" >&2; exit 1; }
[[ ! -e "$package_dir" && ! -e "$source_dir" ]] || {
    echo 'Choose a new output directory; existing packages are not overwritten.' >&2; exit 1;
}
if ! git -C "$repo_root" diff --quiet || ! git -C "$repo_root" diff --cached --quiet; then
    echo 'Tracked files differ from HEAD, so the source archive would not match the binary:' >&2
    git -C "$repo_root" status --short --untracked-files=no >&2
    git -C "$repo_root" diff --stat >&2
    exit 1
fi
mkdir -p "$package_dir/licenses" "$source_dir"
cp "$binary" "$package_dir/porthole.exe"
cp "$repo_root/LICENSE" "$package_dir/LICENSE"
cp "$repo_root/third_party/UxPlay/LICENSE" "$package_dir/licenses/UxPlay-LICENSE"
cp "$repo_root/src/ui/fonts/LICENSE" "$package_dir/licenses/Geist-OFL-LICENSE"
cp "$repo_root/scripts/windows-firewall.ps1" "$package_dir/windows-firewall.ps1"
helper_dir="$(dirname -- "$binary")/ble-winrt"
if [[ -f "$helper_dir/porthole-ble-winrt.exe" ]]; then
    cp -a "$helper_dir" "$package_dir/ble-winrt"
fi
git -C "$repo_root" rev-parse HEAD > "$package_dir/REVISION.txt"

# Inspect every PE import recursively; never execute arbitrary dependency DLLs.
declare -A included=() packages=()
queue=("$package_dir/porthole.exe")
for (( index=0; index<${#queue[@]}; index++ )); do
    while IFS= read -r dll; do
        dll=${dll//$'\r'/}
        lower=${dll,,}
        [[ -n ${included[$lower]:-} ]] && continue
        included[$lower]=1
        if [[ -f "$MINGW_PREFIX/bin/$dll" ]]; then
            cp "$MINGW_PREFIX/bin/$dll" "$package_dir/$dll"
            queue+=("$package_dir/$dll")
            owner=$(pacman -Qoq "$MINGW_PREFIX/bin/$dll")
            packages[$owner]=1
        elif [[ $lower == api-ms-win-* || $lower == ext-ms-win-* ]] \
             || [[ -f "$(cygpath -u "$SYSTEMROOT")/System32/$dll" ]]; then
            : # Windows-provided DLL/API set, not redistributed
        else
            echo "Unresolved runtime DLL: $dll (imported by ${queue[$index]})" >&2
            exit 1
        fi
    done < <(objdump -p "${queue[$index]}" | awk '/DLL Name:/ {print $3}')
done

# Preserve exact binary package versions, copyright/license files, build metadata
# and matching complete MSYS2 source packages (upstream source + PKGBUILD/patches).
: > "$package_dir/DEPENDENCIES.txt"
: > "$source_dir/SOURCE-URLS.txt"
declare -A source_packages=()
for package in "${!packages[@]}"; do
    pacman -Qi "$package" >> "$package_dir/DEPENDENCIES.txt"
    while IFS= read -r path; do
        [[ -f "$path" ]] || continue
        destination="$package_dir/licenses/$package/${path#*share/licenses/}"
        mkdir -p "$(dirname -- "$destination")"
        cp "$path" "$destination"
    done < <(pacman -Qlq "$package" | awk '/\/share\/licenses\//')
    version=$(pacman -Q "$package" | awk '{print $2}')
    find_archive() {
        find /var/cache/pacman/pkg -maxdepth 1 -name "$package-${version#*:}-*.pkg.tar.*" \
            ! -name '*.sig' -print -quit
    }
    archive=$(find_archive)
    if [[ -z $archive ]]; then
        # Preinstalled packages are not cached. Fetch the repository copy; it is
        # accepted only if it is the exact installed version.
        pacman -Sw --noconfirm "$package" >&2 || true
        archive=$(find_archive)
    fi
    if [[ -z $archive ]]; then
        echo "Cached package metadata missing for $package $version; rebuild after installing dependencies." >&2
        exit 1
    fi
    metadata=$(tar -xOf "$archive" .PKGINFO)
    base=$(sed -n 's/^pkgbase = //p' <<< "$metadata")
    source_version=$(sed -n 's/^pkgver = //p' <<< "$metadata")
    source_archive="$base-${source_version#*:}.src.tar.zst"
    if [[ -z ${source_packages[$source_archive]:-} ]]; then
        source_packages[$source_archive]=1
        url="https://repo.msys2.org/mingw/sources/$source_archive"
        curl --fail --location --retry 3 "$url" -o "$source_dir/$source_archive"
        curl --fail --location --retry 3 "$url.sig" -o "$source_dir/$source_archive.sig"
        # pacman-key --verify needs root (or lock-never); gpgv checks the same
        # MSYS2 keyring read-only.
        gpgv --keyring /etc/pacman.d/gnupg/pubring.gpg \
            "$source_dir/$source_archive.sig" "$source_dir/$source_archive"
        printf '%s\n' "$url" >> "$source_dir/SOURCE-URLS.txt"
    fi
    printf '%s\n' "$metadata" > "$source_dir/$package.PKGINFO"
    tar -xOf "$archive" .BUILDINFO > "$source_dir/$package.BUILDINFO"
done
git -C "$repo_root" archive --format=tar --prefix=porthole/ HEAD | gzip -n > "$source_dir/porthole-source.tar.gz"
git -C "$repo_root/third_party/UxPlay" archive --format=tar --prefix=UxPlay/ HEAD \
    | gzip -n > "$source_dir/UxPlay-source.tar.gz"
cp "$package_dir/REVISION.txt" "$source_dir/REVISION.txt"
cat > "$package_dir/READ-ME.txt" <<'TEXT'
Porthole Windows Wi-Fi mirror and Bluetooth controls (portable prototype)

Requires Windows 10 version 1903 or newer, or Windows 11. No Bonjour installation.
Keep porthole.exe and all bundled DLLs together. Launch porthole.exe, join the same
Private Wi-Fi network as the iPhone, then select Porthole in Screen Mirroring.
Bluetooth controls require Windows 10 version 2004 or later and an adapter/driver
that supports Bluetooth LE peripheral mode. Keep the entire ble-winrt/ folder
beside porthole.exe; no Python or installed .NET runtime is required.
The mirror-only build can omit that folder; --no-bluetooth disables controls.

On iPhone enable AssistiveTouch and disable Dwell Control, then pair the PC's
Windows Bluetooth name in Settings > Bluetooth. Accept any Windows pairing
prompt. The Bluetooth name can differ from the Screen Mirroring name. F2 walks
through pairing, mirroring and safe pointer calibration. Keep Tracking
Sensitivity fixed after calibration.

To check hardware support before pairing, run in PowerShell:
  .\ble-winrt\porthole-ble-winrt.exe --probe
The probe creates the real HID service without advertising or connecting.
Hardware/policy failures leave the Wi-Fi mirror usable. Read the probe's error
before retrying pairing; do not remove unrelated Bluetooth devices.

If discovery/video is blocked, review windows-firewall.ps1 and run it explicitly
in an elevated PowerShell window. It allows only the fixed mirror ports and mDNS
on Private networks, limited to the local subnet. No firewall changes run on launch.

Corresponding project/UxPlay and dependency sources are in the matching -sources
artifact with the same revision. DEPENDENCIES.txt records exact runtime packages;
licenses/ contains their installed copyright/license files. See LICENSE (GPL-3.0).
TEXT
(cd "$package_dir" && find . -type f ! -name SHA256SUMS.txt -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS.txt)
(cd "$source_dir" && sha256sum -- *.tar.gz *.tar.zst > SHA256SUMS.txt)
printf 'Binary bundle: %s\nMatching sources: %s\n' "$package_dir" "$source_dir"
