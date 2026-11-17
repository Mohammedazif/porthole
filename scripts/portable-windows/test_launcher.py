#!/usr/bin/env python3
"""Cross-build native test bootstrap; Wine validates Win32 behavior with markers.

No PowerShell is supplied by Wine. Only the TEST-compiled bootstrap accepts the
fake extractor; the production source never reads that environment variable.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import zipfile


def winpath(path):
    return "Z:" + str(path.resolve()).replace("/", "\\")


def localpath(path, env):
    if path.startswith("Z:"):
        return Path(path[2:].replace("\\", "/"))
    return Path(env["WINEPREFIX"]) / "drive_c" / path[3:].replace("\\", "/")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--wine", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    # Debian's extracted Wine needs its separate Windows zlib next to GUI PEs.
    wine_zlib = args.wine.resolve().parents[2] / "x86_64-w64-mingw32/lib/zlib1.dll"
    if wine_zlib.is_file():
        shutil.copyfile(wine_zlib, output / "zlib1.dll")
    compiler = args.toolchain.resolve() / "x86_64-w64-mingw32-clang"
    windres = args.toolchain.resolve() / "x86_64-w64-mingw32-windres"
    flags = ["-std=c11", "-Wall", "-Wextra", "-Wshadow", "-Wformat=2", "-Wundef",
             "-Wstrict-prototypes", "-Wmissing-prototypes", "-Werror", "-municode"]
    fake = output / "native fake 测试 & apostrophe's.exe"
    subprocess.run([str(compiler), *flags, str(source / "fake_payload.c"), "-o", str(fake)], check=True)
    payload = output / "fake.zip"
    with zipfile.ZipFile(payload, "w") as archive:
        archive.writestr("fixture.txt", "native test fixture\n")
    resource = output / "payload.rc"
    resource.write_text('101 RCDATA "' + payload.as_posix() + '"\n', encoding="utf-8")
    obj = output / "payload.o"
    subprocess.run([str(windres), "-i", str(resource), "-o", str(obj)], check=True)
    launcher = output / "portable-test.exe"
    subprocess.run([str(compiler), *flags, "-mwindows", "-DPORTHOLE_PORTABLE_TEST=1",
                    str(source / "launcher.c"), str(obj), "-lole32", "-o", str(launcher)], check=True)
    for mode in ["ok", "extract-fail", "missing-helper", "app-fail"]:
        result, marker = output / (mode + ".result"), output / (mode + ".marker")
        result.unlink(missing_ok=True)
        marker.unlink(missing_ok=True)
        env = dict(os.environ, PORTHOLE_PORTABLE_TEST_EXTRACTOR=winpath(fake),
                   PORTHOLE_PORTABLE_TEST_RESULT=winpath(result),
                   PORTHOLE_PORTABLE_TEST_MARKER=winpath(marker), PORTHOLE_PORTABLE_TEST_MODE=mode)
        completed = subprocess.run([str(args.wine.resolve()), str(launcher)], env=env,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        (output / (mode + ".wine.log")).write_bytes(completed.stdout)
        assert result.exists(), f"Native code did not produce {result}; Wine exit={completed.returncode}"
        text = result.read_text(encoding="utf-8")
        if mode == "ok":
            assert text.startswith("OK\n"), text
            assert marker.read_text() == "NATIVE_APP_RAN\n"
            winlog = text.splitlines()[1]
            assert localpath(winlog, env) == launcher.with_suffix(".log"), winlog
            logged = localpath(winlog, env).read_text()
            assert "FAKE APP STDOUT" in logged and "FAKE APP STDERR" in logged
        else:
            assert text.startswith("ERROR\n") and "Log:" in text and "Extraction log:" in text, text
            assert completed.returncode != 0
            winlog = text.split("Log: ", 1)[1].splitlines()[0]
            assert "Code:" in localpath(winlog, env).read_text(encoding="utf-8")
            if mode != "app-fail":
                assert not marker.exists()
        print(f"{mode}: native result and log assertions passed")
    print("PASS: Win32 resource write, literal Unicode paths, child environment, cwd/arguments, logging and actionable failures")


if __name__ == "__main__":
    main()
