"""Writes tools/code-profile.txt: the functions the native player runs in the memory-budget scenarios.
cdpack expands the code pages that hold them at start-up and leaves the rest compressed until
it first runs (cdpack/spec.md, "Code decoded on demand").

Run after `build.bat unpacked`, from the repository root, when the code has changed a lot:
    py -3 tools/make-code-profile.py [--fixtures build/fixtures]

It runs `cordinal-test.exe --memory-report` against build/unpacked/cordinal.exe and samples which
.text pages each scenario's player has in its working set, with QueryWorkingSetEx. A page is the
finest grain Windows reports, so every function on a page the player ran is listed: the profile
is generous, never short; a function that was only partly resident lists the parts that were. Each
line starts with the FNV-1a hash of a function's object file and decorated
name (code_profile_hash, shared with cdpack/tools/pack.py), so it survives relinks and code
moving; a function added later is simply not listed and is decoded when it first runs.
"""
import argparse
import ctypes
import ctypes.wintypes as wintypes
from pathlib import Path
import re
import struct
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
# A scenario asks for its report about 8 seconds after start (memory_budget.cpp); pages first
# touched later belong to closing the player, which runs once and may be decoded then.
SCENARIO_SECONDS = 8.2
MAP_FUNCTION = re.compile(r"^\s*0001:([0-9a-f]{8})\s+(\S+)\s+[0-9a-f]{16}\s+f\s.*?(\S+)\s*$")


def code_profile_hash(obj, name):
    """32-bit FNV-1a of 'object|name'; cdpack/tools/pack.py has the same function."""
    value = 2166136261
    for byte in f"{obj}|{name}".encode():
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


def map_functions(text):
    """Map offsets in .text (section 0001) to their (object, name) pairs, in address order."""
    by_address = {}
    for line in text.splitlines():
        match = MAP_FUNCTION.match(line)
        if match:
            obj = match.group(3).rpartition(":")[2]
            by_address.setdefault(int(match.group(1), 16), []).append((obj, match.group(2)))
    return sorted(by_address.items())


class WorkingSetEx(ctypes.Structure):
    _fields_ = [("address", ctypes.c_void_p), ("attributes", ctypes.c_uint64)]


def text_section(exe):
    data = exe.read_bytes()
    pe = struct.unpack_from("<I", data, 60)[0]
    count = struct.unpack_from("<H", data, pe + 6)[0]
    table = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(count):
        name, virtual, rva = struct.unpack_from("<8sII", data, table + i * 40)
        if name.rstrip(b"\0") == b".text":
            return rva, virtual, struct.unpack_from("<I", data, pe + 8)[0]
    raise SystemExit("make-code-profile: no .text section")


def sample_players(exe, rva, size, stop, pages):
    """Unions each player's resident .text pages over its scenario, until `stop` is set."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    psapi = ctypes.WinDLL("psapi")
    kernel.OpenProcess.restype = wintypes.HANDLE
    target, first_seen, count = str(exe).lower(), {}, (size + 4095) // 4096
    while not stop.is_set():
        listing = subprocess.run(["powershell", "-NoProfile", "-Command",
                                  "Get-Process cordinal -ErrorAction SilentlyContinue | "
                                  "ForEach-Object { \"$($_.Id)|$($_.Path)\" }"],
                                 capture_output=True, text=True).stdout
        for line in listing.splitlines():
            pid, _, path = line.partition("|")
            if path.strip().lower() != target:
                continue
            seen = first_seen.setdefault(int(pid), time.monotonic())
            if time.monotonic() - seen > SCENARIO_SECONDS:
                continue
            handle = kernel.OpenProcess(0x0410 | 0x1000, False, int(pid))
            if not handle:
                continue
            modules, needed = (ctypes.c_void_p * 1)(), wintypes.DWORD()
            if psapi.EnumProcessModulesEx(handle, modules, ctypes.sizeof(modules), ctypes.byref(needed), 3):
                query = (WorkingSetEx * count)()
                for i in range(count):
                    query[i].address = modules[0] + rva + i * 4096
                if psapi.QueryWorkingSetEx(handle, query, ctypes.sizeof(query)):
                    pages.update(i for i in range(count) if query[i].attributes & 1)
            kernel.CloseHandle(handle)
        time.sleep(0.25)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixtures", default="build/fixtures")
    parser.add_argument("--map", default="build/cordinal.map")
    parser.add_argument("--out", default="tools/code-profile.txt")
    parser.add_argument("--pages", help="also write the resident .text page numbers here")
    args = parser.parse_args()
    exe = (ROOT / "build/unpacked/cordinal.exe").resolve()
    rva, size, stamp = text_section(exe)
    text = (ROOT / args.map).read_text(errors="replace")
    found = re.search(r"Timestamp is ([0-9a-f]{8})", text)
    if not found or int(found.group(1), 16) != stamp:
        raise SystemExit("make-code-profile: the map does not belong to build/unpacked/cordinal.exe; relink")
    functions = map_functions(text)
    pages, stop = set(), threading.Event()
    sampler = threading.Thread(target=sample_players, args=(exe, rva, size, stop, pages))
    sampler.start()
    try:
        report = subprocess.run([str(ROOT / "build/unpacked/cordinal-test.exe"), "--memory-report", str(exe),
                                 str(ROOT / args.fixtures)], capture_output=True, text=True)
    finally:
        stop.set()
        sampler.join()
    if report.returncode or "no report" in report.stdout or not pages:
        sys.stdout.write(report.stdout)
        raise SystemExit("make-code-profile: the scenarios did not run")
    if args.pages:
        Path(args.pages).write_text(" ".join(map(str, sorted(pages))) + "\n")
    # A function runs to the next code address in the map. One that is only partly on resident pages
    # also lists, as hex byte offsets from its start, the parts that were.
    lines, hot_bytes = {}, 0
    for index, (offset, aliases) in enumerate(functions):
        end = max(functions[index + 1][0] if index + 1 < len(functions) else size, offset + 1)
        spans, start = [], None
        for page in range(offset // 4096, (end - 1) // 4096 + 2):
            if page in pages and page <= (end - 1) // 4096:
                start = page if start is None else start
            elif start is not None:
                spans.append((max(start * 4096, offset) - offset, min(page * 4096, end) - offset))
                start = None
        if not spans:
            continue
        hot_bytes += sum(high - low for low, high in spans)
        suffix = "" if spans == [(0, end - offset)] else " " + ",".join(f"{low:x}:{high:x}" for low, high in spans)
        for obj, name in aliases:
            lines[code_profile_hash(obj, name)] = suffix
    Path(ROOT / args.out).write_text("".join(f"{value:08x}{lines[value]}\n" for value in sorted(lines)),
                                     newline="\n")
    print(f"{len(pages)} resident .text pages ({len(pages) * 4096 / 1048576:.2f} MiB); "
          f"{len(lines)} functions, {hot_bytes / 1048576:.2f} MiB, written to {args.out}")


if __name__ == "__main__":
    main()
