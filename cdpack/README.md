# cdpack

cordinal's own CDP1 decoder, PE64 loader, packer and verifier. The sources are bundled
so a fresh cordinal checkout can build them without the sibling UPX research checkout.

It packs cordinal's Windows release from 0.1.5 on (`build.bat cdpack`, which release CI runs);
`build.bat stock-nrv` still packs with the pinned stock UPX. [ROADMAP.md](ROADMAP.md) lists what
is still to improve, starting with idle memory and testing beyond Windows 11.

From the cordinal root:

```powershell
build.bat cdpack
cmake -S cdpack -B build/cdpack-check -G Ninja
ctest --test-dir build/cdpack-check --output-on-failure
```

The independent build needs an installed LLVM-MinGW `clang++.exe` (the GNU Windows target) and
Python 3.14 through `py.exe`. CMake and PowerShell run the standalone verification suite.
None of these build tools is a player runtime dependency. Standalone output defaults to
`build/cdpack`; the player build keeps its tools under `build/tools/cdpack`.

`src/decoder/stream.h` decodes the packed stream, `src/stub/loader.cpp` restores a PE64 image, and `tools/pack.py`
encodes and verifies the container. Every player pack requires exact native-file and resource
equality. Tests include guards, corrupted/truncated streams, checksums, deterministic output,
overlays, EXE/DLL and TLS loading, forced ASLR and unsupported CFG rejection.

See `spec.md`, `PROVENANCE.md`, `PORTING.md` and `origin.json` for the format and source history.
The imported original code is GPL-2.0-or-later; `COPYING` preserves the license text. It contains
no UPX/UCL decoder or loader implementation.
