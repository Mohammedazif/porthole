#!/usr/bin/env python3
"""Build a portable Windows launcher containing a complete, verified mirror bundle."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

from verify_payload import verify_payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("payload", type=Path, help="Complete portable package directory")
    parser.add_argument("output", type=Path, help="New Windows executable to create")
    parser.add_argument("--compiler", default="gcc", help="Windows x64 C compiler")
    parser.add_argument("--windres", default="windres", help="Windows resource compiler")
    args = parser.parse_args()
    payload, output = args.payload.resolve(), args.output.resolve()
    if output.exists():
        parser.error(f"Output already exists: {output}")
    if output.is_relative_to(payload):
        parser.error("Output must be outside the payload directory")
    report = verify_payload(payload)
    source = Path(__file__).resolve().parent
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="porthole-portable-build-", dir=output.parent) as work:
        work = Path(work)
        archive = work / "payload.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as package:
            for path in sorted(payload.rglob("*")):
                if path.is_symlink():
                    raise ValueError(f"Payload contains a symlink: {path}")
                if path.is_file():
                    relative = path.relative_to(payload).as_posix()
                    if any(c in relative for c in ("\\", ":", "\r", "\n")):
                        raise ValueError(f"Unsupported Windows payload path: {relative!r}")
                    package.write(path, relative)
        archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
        resource = work / "payload.rc"
        icon = source.parents[1] / "branding/porthole/app-icons/dark/porthole.ico"
        archive_name, icon_name = archive.as_posix(), icon.as_posix()
        if any(character in name for name in (archive_name, icon_name) for character in '"\n\r'):
            raise ValueError("Resource paths cannot contain quotes or newlines")
        resource.write_text(f'1 ICON "{icon_name}"\n101 RCDATA "{archive_name}"\n', encoding="utf-8")
        object_file = work / "payload.o"
        subprocess.run([args.windres, "-i", str(resource), "-o", str(object_file), "-O", "coff"], check=True)
        binary = work / "Porthole-Portable.exe"
        subprocess.run([
            args.compiler, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-municode", "-mwindows", "-static-libgcc", "-s",
            str(source / "launcher.c"), str(object_file), "-lole32", "-o", str(binary),
        ], check=True)
        if binary.read_bytes()[:2] != b"MZ":
            raise ValueError("Compiler did not produce a Windows PE executable")
        shutil.copy2(binary, output)
    report = {
        "output": str(output),
        "bytes": output.stat().st_size,
        "sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "payload_zip_sha256": archive_hash,
        "validation": report,
    }
    output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "output": str(output), "bytes": report["bytes"], "sha256": report["sha256"],
        "payload_files": report["validation"]["payload_file_count"],
        "report": str(output.with_suffix(".json")),
    }))


if __name__ == "__main__":
    main()
