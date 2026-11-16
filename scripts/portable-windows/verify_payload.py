#!/usr/bin/env python3
"""Verify the complete Windows payload, ZIP or portable launcher before shipping.

The .NET bundle layout and static-host exports follow dotnet/runtime:
  src/installer/managed/Microsoft.NET.HostModel/Bundle/{Manifest,FileEntry}.cs
  src/native/corehost/apphost/static/{CMakeLists.txt,singlefilehost.def}
Only Python's standard library is needed. This inspects binaries without running them.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import struct
import sys
import tempfile
import zipfile
import zlib


class VerificationError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def safe_relative(name: str) -> str:
    require(bool(name) and "\0" not in name and "\\" not in name,
            f"Invalid payload path: {name!r}")
    posix, windows = PurePosixPath(name), PureWindowsPath(name)
    require(not posix.is_absolute() and not windows.is_absolute() and not windows.drive
            and all(part not in ("", ".", "..") and ":" not in part
                    and not part.endswith((" ", ".")) for part in name.split("/")),
            f"Unsafe payload path: {name!r}")
    return name


class PE:
    def __init__(self, data: bytes):
        self.data = data
        require(data[:2] == b"MZ", "Expected a Windows PE executable")
        pe = self.unpack("<I", 0x3c)[0]
        require(data[pe:pe + 4] == b"PE\0\0", "Invalid PE signature")
        self.machine, count, _, _, _, optional_size, _ = self.unpack("<HHIIIHH", pe + 4)
        optional = pe + 24
        magic = self.unpack("<H", optional)[0]
        require(magic in (0x10b, 0x20b), "Unsupported PE optional header")
        self.image_base = self.unpack("<Q" if magic == 0x20b else "<I", optional + (24 if magic == 0x20b else 28))[0]
        self.headers = self.unpack("<I", optional + 60)[0]
        self.directory = optional + (112 if magic == 0x20b else 96)
        self.directory_count = self.unpack("<I", self.directory - 4)[0]
        require(0 < count <= 96 and self.directory_count <= 32, "Invalid PE section/directory count")
        self.sections = []
        for index in range(count):
            section = optional + optional_size + index * 40
            virtual_size, address, raw_size, raw_offset = self.unpack("<IIII", section + 8)
            self.sections.append((address, virtual_size, raw_size, raw_offset))

    def unpack(self, fmt: str, offset: int):
        require(0 <= offset <= len(self.data) - struct.calcsize(fmt), "Truncated binary data")
        return struct.unpack_from(fmt, self.data, offset)

    def directory_entry(self, index: int):
        return self.unpack("<II", self.directory + index * 8) if index < self.directory_count else (0, 0)

    def rva(self, address: int) -> int:
        if address < self.headers:
            require(address < len(self.data), "PE header address outside file")
            return address
        for base, virtual_size, raw_size, raw_offset in self.sections:
            if base <= address < base + max(virtual_size, raw_size):
                require(address - base < raw_size, "PE address has no file content")
                offset = raw_offset + address - base
                require(offset < len(self.data), "PE address outside file")
                return offset
        raise VerificationError(f"Unmapped PE address 0x{address:x}")

    def string(self, address: int) -> str:
        offset = self.rva(address)
        end = self.data.find(b"\0", offset, offset + 4096)
        require(end >= offset, "Unterminated PE string")
        return self.data[offset:end].decode("ascii")

    @property
    def managed(self) -> bool:
        return bool(self.directory_entry(14)[0])

    def imports(self) -> set[str]:
        names = set()
        for directory, stride, name_offset in ((1, 20, 12), (13, 32, 4)):
            address, size = self.directory_entry(directory)
            if not address:
                continue
            start = self.rva(address)
            require(0 < size <= len(self.data), "Invalid PE import table size")
            for offset in range(start, start + size, stride):
                words = self.unpack("<" + "I" * (stride // 4), offset)
                if not any(words):
                    break
                name_address = self.unpack("<I", offset + name_offset)[0]
                if directory == 13 and not words[0] & 1:
                    name_address -= self.image_base
                names.add(self.string(name_address).lower())
        return names

    def exports(self) -> set[str]:
        address, _ = self.directory_entry(0)
        if not address:
            return set()
        start = self.rva(address)
        count = self.unpack("<I", start + 24)[0]
        require(count < 100000, "Invalid PE export count")
        names = self.rva(self.unpack("<I", start + 32)[0])
        return {self.string(self.unpack("<I", names + index * 4)[0]) for index in range(count)}

    def resource(self, resource_type: int, resource_id: int) -> bytes:
        address, size = self.directory_entry(2)
        require(address != 0, "Portable launcher has no embedded resources")
        root = self.rva(address)

        def entries(relative: int):
            require(0 <= relative <= size - 16, "Invalid PE resource directory")
            named, numbered = self.unpack("<HH", root + relative + 12)
            require(named + numbered <= 4096, "Invalid PE resource count")
            return [self.unpack("<II", root + relative + 16 + i * 8) for i in range(named + numbered)]

        def child(relative: int, identifier: int):
            for name, target in entries(relative):
                if name == identifier:
                    require(target & 0x80000000, "Expected a resource directory")
                    return target & 0x7fffffff
            raise VerificationError(f"Missing PE resource {identifier}")

        languages = entries(child(child(0, resource_type), resource_id))
        require(bool(languages), "Embedded payload resource has no language")
        _, target = languages[0]
        require(not target & 0x80000000 and target <= size - 16, "Invalid resource data entry")
        value_address, length, _, _ = self.unpack("<IIII", root + target)
        offset = self.rva(value_address)
        require(offset + length <= len(self.data), "Embedded ZIP is truncated")
        return self.data[offset:offset + length]


BUNDLE_SIGNATURE = bytes.fromhex("8b1202b96a612038727b930214d7a03213f5b9e6efae3318ee3b2dce24b36aae")
STATIC_HOST_EXPORTS = {"DotNetRuntimeInfo", "g_CLREngineMetrics", "CLRJitAttachState", "g_dacTable"}
REQUIRED_MANAGED = {"porthole-ble-winrt.dll", "Porthole.Ble.Core.dll", "WinRT.Runtime.dll",
                    "Microsoft.Windows.SDK.NET.dll", "System.Private.CoreLib.dll", "System.Runtime.dll"}


def verify_helper(path: Path | str) -> dict:
    path = Path(path)
    data = path.read_bytes()
    pe = PE(data)
    require(pe.machine == 0x8664 and not pe.managed, "Bluetooth helper must be a native win-x64 apphost")
    require(STATIC_HOST_EXPORTS <= pe.exports(), "Bluetooth helper lacks its statically linked .NET runtime")
    marker = data.find(BUNDLE_SIGNATURE)
    require(marker >= 8, "Bluetooth helper is not a .NET single-file bundle")
    header = pe.unpack("<q", marker - 8)[0]
    require(marker + 32 < header < len(data), "Bluetooth helper has no valid bundle header")
    major, minor, count = pe.unpack("<IIi", header)
    require(major == 6 and minor == 0 and 0 < count < 4096, "Unsupported or malformed .NET bundle manifest")
    cursor = header + 12

    def binary_string() -> str:
        nonlocal cursor
        length = 0
        for shift in range(0, 35, 7):
            value = pe.unpack("<B", cursor)[0]; cursor += 1
            length |= (value & 127) << shift
            if value < 128:
                break
        else:
            raise VerificationError("Invalid bundle string length")
        require(length < 65536 and cursor + length <= len(data), "Truncated bundle string")
        value = data[cursor:cursor + length].decode("utf-8"); cursor += length
        return value

    bundle_id = binary_string()
    deps_offset, deps_size, config_offset, config_size, flags = pe.unpack("<qqqqQ", cursor); cursor += 40
    require(flags & 1, "Helper must extract all bundled files before managed/WinRT loading")
    entries, configurations, names = [], {}, set()
    for _ in range(count):
        offset, length, compressed, kind = pe.unpack("<qqqB", cursor); cursor += 25
        name = safe_relative(binary_string())
        require(name.lower() not in names, f"Duplicate bundle path: {name}")
        names.add(name.lower())
        require(0 < length < 512 * 1024 * 1024 and 0 <= compressed <= length
                and marker + 32 <= offset < header and offset + (compressed or length) <= header,
                f"Invalid/truncated embedded file: {name}")
        payload = data[offset:offset + (compressed or length)]
        if compressed:
            inflater = zlib.decompressobj(-15)
            payload = inflater.decompress(payload, length + 1)
            require(inflater.eof and not inflater.unused_data and len(payload) == length,
                    f"Corrupt compressed bundle entry: {name}")
        require(len(payload) == length, f"Wrong embedded length: {name}")
        if name.endswith(".dll"):
            require(PE(payload).managed, f"Unexpected nonmanaged bundle DLL: {name}")
        if name.endswith(".json"):
            configurations[name] = json.loads(payload)
        entries.append({"path": name, "size": length, "compressed_size": compressed,
                        "sha256": hashlib.sha256(payload).hexdigest(), "type": kind})
    require({name.lower() for name in REQUIRED_MANAGED} <= names, "Bundled helper/WinRT/framework assemblies are incomplete")
    config_name, deps_name = "porthole-ble-winrt.runtimeconfig.json", "porthole-ble-winrt.deps.json"
    require(config_name in configurations and deps_name in configurations, "Bundled runtimeconfig/deps are missing")
    require(data[config_offset:config_offset + config_size].lstrip().startswith(b"{")
            and data[deps_offset:deps_offset + deps_size].lstrip().startswith(b"{"),
            "Bundle JSON header locations are invalid")
    config = configurations[config_name]["runtimeOptions"]
    require(config.get("tfm") == "net10.0" and not config.get("framework") and not config.get("frameworks")
            and any(framework.get("name") == "Microsoft.NETCore.App" and framework.get("version", "").startswith("10.")
                    for framework in config.get("includedFrameworks", [])),
            "Bluetooth helper depends on an installed .NET runtime")
    deps = configurations[deps_name]
    target = deps["runtimeTarget"]["name"]
    require(target.endswith("/win-x64") and target in deps["targets"], "Bluetooth helper dependency target is not win-x64")
    required_assets = set()
    for package in deps["targets"][target].values():
        for category in ("runtime", "native", "resources"):
            for asset in package.get(category, {}):
                if not asset.endswith("/_._"):
                    required_assets.add(PurePosixPath(asset).name.lower())
    require(required_assets <= names, "Missing bundled dependency assets: " + ", ".join(sorted(required_assets - names)))
    require(not any(imported in {"hostfxr.dll", "hostpolicy.dll", "coreclr.dll", "clrjit.dll"} for imported in pe.imports()),
            "Single-file helper imports an adjacent runtime DLL")
    return {"path": str(path), "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
            "bundle_version": f"{major}.{minor}", "bundle_id": bundle_id, "extract_all": True,
            "runtime": config["includedFrameworks"], "dependency_target": target,
            "static_runtime_exports": sorted(STATIC_HOST_EXPORTS), "entries": entries}


# Only operating-system libraries/API sets may be absent from the app directory.
# MSVC, MinGW, FFmpeg, SDL and other application libraries must be bundled.
SYSTEM_DLLS = set("""advapi32 avicap32 avrt bcrypt bcryptprimitives cabinet cfgmgr32 comctl32 comdlg32
crypt32 cryptbase cryptnet cryptsp d2d1 d3d11 d3d12 d3d9 dbghelp dnsapi dsound dwrite dwmapi dxgi
dxva2 evr gdi32 gdiplus glu32 hid imm32 iphlpapi kernel32 kernelbase mf mfplat mfreadwrite mfuuid mmdevapi msimg32
mpr msacm32 mscoree msvcrt ncrypt netapi32 normaliz ntdll ole32 oleacc oleaut32 opengl32 powrprof
propsys psapi rpcrt4 runtimeobject secur32 setupapi shcore shell32 shlwapi strmiids user32 userenv
ucrtbase usp10 uxtheme version vfw32 winhttp wininet winmm winspool wintrust wldap32 ws2_32 wtsapi32
wsock32 mswsock win32u authz dhcpcsvc dhcpcsvc6 dinput8 xinput1_4 xinput9_1_0""".split())


def verify_payload(path: Path | str) -> dict:
    root = Path(path)
    require(root.is_dir(), f"Payload directory is missing: {root}")
    files = {}
    for file in root.rglob("*"):
        require(not file.is_symlink(), f"Payload symlink is not portable: {file}")
        if file.is_file():
            name = safe_relative(file.relative_to(root).as_posix())
            require(name.lower() not in files, f"Case-colliding payload path: {name}")
            files[name.lower()] = file
    require("porthole.exe" in files and "ble-winrt/porthole-ble-winrt.exe" in files,
            "Payload must contain porthole.exe and ble-winrt/porthole-ble-winrt.exe")
    helper = verify_helper(files["ble-winrt/porthole-ble-winrt.exe"])
    unresolved, imports = [], {}
    for name, file in files.items():
        if file.suffix.lower() not in (".exe", ".dll"):
            continue
        binary = PE(file.read_bytes())
        require(binary.machine == 0x8664, f"Native payload architecture is not x64: {name}")
        require(not binary.managed, f"Unexpected loose managed DLL in single-file payload: {name}")
        directory = PurePosixPath(name).parent
        imported = sorted(binary.imports())
        imports[name] = imported
        for dependency in imported:
            local = (directory / dependency).as_posix()
            if local in files or dependency.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if dependency.endswith(".dll") and dependency[:-4] in SYSTEM_DLLS:
                continue
            unresolved.append(f"{name} -> {dependency}")
    require(not unresolved, "Missing native dependencies: " + "; ".join(unresolved))
    inventory = [{"path": file.relative_to(root).as_posix(), "size": file.stat().st_size,
                  "sha256": hashlib.sha256(file.read_bytes()).hexdigest()} for _, file in sorted(files.items())]
    return {"payload_file_count": len(inventory), "native_imports": imports,
            "helper": helper, "files": inventory}


def verify_artifact(path: Path | str) -> dict:
    path = Path(path)
    if path.is_dir():
        return verify_payload(path)
    artifact = path.read_bytes()
    archive = PE(artifact).resource(10, 101) if path.suffix.lower() == ".exe" else artifact
    with zipfile.ZipFile(io.BytesIO(archive)) as zipped, tempfile.TemporaryDirectory(prefix="porthole-verify-") as temporary:
        seen = set()
        for member in zipped.infolist():
            if member.is_dir():
                continue
            name = safe_relative(member.filename)
            require(name.lower() not in seen, f"Duplicate ZIP payload path: {name}")
            require(member.file_size < 512 * 1024 * 1024 and not member.flag_bits & 1,
                    f"Invalid/encrypted ZIP entry: {name}")
            require((member.external_attr >> 16) & 0xf000 != 0xa000, f"ZIP symlink is not portable: {name}")
            seen.add(name.lower())
            destination = Path(temporary, *PurePosixPath(name).parts)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(zipped.read(member))  # verifies ZIP CRC as well
        result = verify_payload(temporary)
        result["artifact"] = {"path": str(path), "size": len(artifact),
                              "sha256": hashlib.sha256(artifact).hexdigest()}
        result["helper"]["path"] = "ble-winrt/porthole-ble-winrt.exe"
        return result


def run_regressions(helper_path: Path, payload: Path | None = None) -> None:
    """Mutate a verified fixture; never modify the real package or helper."""
    original = helper_path.read_bytes()

    def rejected(label, action):
        try:
            action()
        except (VerificationError, UnicodeError, KeyError, json.JSONDecodeError, zipfile.BadZipFile, zlib.error):
            print("PASS rejects " + label)
            return
        raise VerificationError("Regression accepted " + label)

    # Keep mutations outside the staging tree, so neither enumeration nor a
    # concurrent packager can mistake test fixtures for distribution files.
    scratch_parent = payload.parent if payload is not None else helper_path.parent
    with tempfile.TemporaryDirectory(prefix="porthole-package-tests-", dir=scratch_parent) as temporary:
        directory = Path(temporary)
        candidate = directory / "helper.exe"

        def mutated(label, change):
            data = bytearray(original)
            change(data)
            candidate.write_bytes(data)
            rejected(label, lambda: verify_helper(candidate))

        marker = original.index(BUNDLE_SIGNATURE)
        header = struct.unpack_from("<q", original, marker - 8)[0]
        mutated("an unbundled/partial apphost", lambda data: struct.pack_into("<q", data, marker - 8, 0))
        mutated("a truncated single-file bundle", lambda data: data.pop())

        cursor = header + 12
        while original[cursor] & 0x80:
            cursor += 1
        cursor += 1 + original[cursor]
        first_entry = cursor + 40
        payload_offset, _, compressed, _ = struct.unpack_from("<qqqB", original, first_entry)
        require(compressed > 0, "Compressed helper fixture is required for the corruption regression")
        mutated("corrupt embedded compression", lambda data: data.__setitem__(payload_offset, 7))
        missing = original.rfind(b"WinRT.Runtime.dll")
        require(missing > header, "WinRT assembly manifest fixture is missing")
        mutated("a missing WinRT dependency", lambda data: data.__setitem__(slice(missing, missing + 17), b"LostT.Runtime.dll"))
        export = original.find(b"DotNetRuntimeInfo\0")
        require(export >= 0, "Static host export fixture is missing")
        mutated("an apphost without static-runtime exports", lambda data: data.__setitem__(export, ord("X")))

        unsafe = directory / "unsafe.zip"
        with zipfile.ZipFile(unsafe, "w") as archive:
            archive.writestr("../porthole.exe", b"invalid")
        rejected("a ZIP path outside the payload", lambda: verify_artifact(unsafe))

        if payload is not None:
            copy = directory / "payload"
            for file in payload.rglob("*"):
                if file.is_file():
                    destination = copy / file.relative_to(payload)
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    os.link(file, destination)
            sdl = next(copy.glob("[Ss][Dd][Ll]2.dll"), None)
            require(sdl is not None, "SDL2 fixture is required for the native dependency regression")
            sdl.unlink()
            rejected("a missing native SDL dependency", lambda: verify_payload(copy))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", nargs="?", type=Path, help="Staging directory, payload ZIP, or portable launcher EXE")
    parser.add_argument("--helper-only", type=Path, metavar="EXE", help="Inspect a standalone single-file Bluetooth helper")
    parser.add_argument("--report", type=Path, help="Write detailed SHA-256 inventory and runtime evidence as JSON")
    parser.add_argument("--self-test", action="store_true", help="Run rejection regressions against a helper or payload directory")
    arguments = parser.parse_args()
    if bool(arguments.payload) == bool(arguments.helper_only):
        parser.error("Supply either PAYLOAD or --helper-only EXE")
    try:
        result = verify_helper(arguments.helper_only) if arguments.helper_only else verify_artifact(arguments.payload)
        if arguments.report:
            arguments.report.parent.mkdir(parents=True, exist_ok=True)
            arguments.report.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        helper = result if arguments.helper_only else result["helper"]
        if arguments.self_test:
            require(arguments.helper_only is not None or arguments.payload.is_dir(),
                    "Self-tests require a helper EXE or staging directory")
            run_regressions(arguments.helper_only or arguments.payload / "ble-winrt/porthole-ble-winrt.exe",
                            arguments.payload if arguments.payload and arguments.payload.is_dir() else None)
        print(f"PASS complete win-x64 payload: helper {helper['size']:,} bytes, "
              f"{len(helper['entries'])} bundled files, static .NET runtime; SHA-256 {helper['sha256']}")
        return 0
    except (VerificationError, OSError, UnicodeError, KeyError, json.JSONDecodeError, zipfile.BadZipFile, zlib.error) as error:
        print(f"FAIL payload verification: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
