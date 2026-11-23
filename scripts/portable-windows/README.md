# One-file Windows preview

The portable launcher embeds a complete ZIP as RCDATA resource 101. It prepares a
unique directory under the current user's Windows TEMP, extracts with the system's
PowerShell archive module, then opens the Porthole workspace with Bluetooth available.
Open Set up controls from the workspace when you need pairing or pointer calibration.
The progress window remains responsive during extraction. The child receives its own
runtime-cache path and log handles; no system settings, services or drivers are installed.

The Bluetooth helper is independently self-contained: .NET 10's single-file host embeds
the runtime and all managed dependencies. It needs no adjacent `hostpolicy.dll`, runtime
configuration or installed .NET. Native SDL/FFmpeg dependencies remain inside the outer
portable payload and are extracted automatically.

To reproduce the portable file, first prepare the complete native Windows package using
the committed MSYS2 packaging script. Then run, with Python 3 and the x64 MinGW compiler
and resource compiler available:

```sh
python3 scripts/portable-windows/build_portable.py \
    out/porthole-windows-x64 out/Porthole-Windows-Portable.exe
python3 scripts/portable-windows/verify_payload.py \
    out/Porthole-Windows-Portable.exe --report out/portable-verification.json
```

`--compiler` and `--windres` accept explicit compiler paths for cross builds. Existing
output executables are not overwritten. The build report records hashes and a recursive
inventory. Verification checks the embedded .NET manifest and decompressed assemblies,
the statically linked runtime, native dependency closure and safe Windows paths. It also
inspects the final executable's embedded ZIP independently.

On Windows, run the resulting portable executable directly. The connection log is written
beside the portable executable with the same name and `.log` extension. A read-only launch
folder falls back to `portable.log` under the private TEMP directory. Extraction files,
`extractor.log` and `dotnet-host.log` remain under TEMP for diagnosis. The file does not
install Porthole or change Bluetooth pairing.
Actual HID advertising and iPhone input still need a live host check.
