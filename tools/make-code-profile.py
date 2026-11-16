"""Writes tools/code-profile.txt: the functions the native player runs in the memory-budget scenarios.
cdpack expands the code pages that hold them at start-up and leaves the rest compressed until
it first runs (cdpack/spec.md, "Code decoded on demand"), and a cdpack build links them first
(tools/build.ps1), so they share as few pages as possible.

Run after `build.bat unpacked`, from the repository root, when the code has changed a lot:
    py -3 tools/make-code-profile.py [--fixtures build/fixtures]

It runs `cordinal-test.exe --memory-report` against build/unpacked/cordinal.exe under a debugger
that breaks once on the first byte of every function in the player, so the profile lists exactly
the functions that ran (about 0.7 of 3.7 MiB of code on November 16, 2026). Sampling which pages
were resident, as it did before, listed every function sharing a page with one that ran: 2 MiB.
Each line is the FNV-1a hash of a function's object file and decorated name (code_profile_hash,
shared with cdpack/tools/pack.py), so it survives relinks and code moving; a function added later
is simply not listed and is decoded when it first runs.

`--write-order` writes the link order from the profile and the last link's map instead; the
cdpack build runs it.
"""
import argparse
import ctypes
import ctypes.wintypes as wintypes
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
# The main window's close path (WM_CLOSE in app_window.cpp): a player's record ends at the first of them.
CLOSING = {"?StopAppWorkers@app@@YAXPEAUHWND__@@@Z", "?SaveBeforeClose@app@@YAXXZ"}
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


class DebugEvent(ctypes.Structure):
    _fields_ = [("code", wintypes.DWORD), ("pid", wintypes.DWORD), ("tid", wintypes.DWORD),
                ("pad", wintypes.DWORD), ("info", ctypes.c_ubyte * 160)]


def trace_scenarios(command, exe, rva, starts, closing):
    """Runs the scenarios under a debugger that puts a breakpoint on the first byte of every function
    in each player's .text and returns the offsets (from .text) of the functions that ran.

    A breakpoint is removed when it is first hit, so each function costs one stop. A player's
    record ends when it enters one of `closing` (the main window's close path): what runs after
    that runs once, at exit, and may be decoded then."""
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenThread.restype = wintypes.HANDLE
    for name in ("ReadProcessMemory", "WriteProcessMemory"):
        getattr(kernel, name).argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                          ctypes.POINTER(ctypes.c_size_t)]
    kernel.VirtualProtectEx.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t, wintypes.DWORD,
                                        ctypes.POINTER(wintypes.DWORD)]
    kernel.FlushInstructionCache.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_size_t]
    kernel.GetThreadContext.argtypes = kernel.SetThreadContext.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    kernel.ContinueDebugEvent.argtypes = [wintypes.DWORD, wintypes.DWORD, wintypes.DWORD]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR,
                                                  ctypes.POINTER(wintypes.DWORD)]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]

    class StartupInfo(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD)] + [(f"f{i}", ctypes.c_void_p) for i in range(12)]

    class ProcessInfo(ctypes.Structure):
        _fields_ = [("process", wintypes.HANDLE), ("thread", wintypes.HANDLE), ("pid", wintypes.DWORD),
                    ("tid", wintypes.DWORD)]

    startup, info = StartupInfo(), ProcessInfo()
    startup.cb = ctypes.sizeof(startup)
    # DEBUG_PROCESS also delivers the events of the players the companion starts.
    if not kernel.CreateProcessW(None, ctypes.create_unicode_buffer(subprocess.list2cmdline(command)), None, None,
                                 False, 0x1 | 0x08000000, None, str(ROOT), ctypes.byref(startup), ctypes.byref(info)):
        raise SystemExit(f"make-code-profile: could not start the companion ({ctypes.get_last_error()})")
    companion = info.pid
    target, size = str(exe).lower(), max(starts) + 1
    players, ran = {}, set()  # pid -> [process handle, .text address, original bytes, closed]
    context = ctypes.create_string_buffer(1232 + 16)
    aligned = (ctypes.addressof(context) + 15) & ~15
    event = DebugEvent()
    while True:
        if not kernel.WaitForDebugEvent(ctypes.byref(event), 0xFFFFFFFF):
            raise SystemExit("make-code-profile: WaitForDebugEvent failed")
        status = 0x00010002  # DBG_CONTINUE
        code, pid = event.code, event.pid
        if code == 3:  # CREATE_PROCESS: hFile, hProcess, hThread, lpBaseOfImage
            file, process, _, base = struct.unpack_from("<QQQQ", bytes(event.info), 0)
            if file:
                kernel.CloseHandle(file)
            name, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
            kernel.QueryFullProcessImageNameW(process, 0, name, ctypes.byref(length))
            if name.value.lower() == target:
                text = base + rva
                original = (ctypes.c_ubyte * size)()
                done = ctypes.c_size_t()
                old = wintypes.DWORD()
                kernel.ReadProcessMemory(process, text, original, size, ctypes.byref(done))
                patched = bytearray(original)
                for start in starts:
                    patched[start] = 0xCC
                kernel.VirtualProtectEx(process, text, size, 0x40, ctypes.byref(old))
                kernel.WriteProcessMemory(process, text, bytes(patched), size, ctypes.byref(done))
                kernel.VirtualProtectEx(process, text, size, old.value, ctypes.byref(old))
                kernel.FlushInstructionCache(process, text, size)
                players[pid] = [process, text, original, False]
        elif code == 6:  # LOAD_DLL: hFile first
            file = struct.unpack_from("<Q", bytes(event.info), 0)[0]
            if file:
                kernel.CloseHandle(file)
        elif code == 1:  # EXCEPTION: code, flags, record, address
            exception, _, _, address = struct.unpack_from("<IIQQ", bytes(event.info), 0)
            player = players.get(pid)
            offset = address - player[1] if player else -1
            if exception == 0x80000003 and 0 <= offset < size and offset in starts:
                process, text, original = player[0], player[1], player[2]
                done, old = ctypes.c_size_t(), wintypes.DWORD()
                kernel.VirtualProtectEx(process, address, 1, 0x40, ctypes.byref(old))
                kernel.WriteProcessMemory(process, address, ctypes.addressof(original) + offset, 1, ctypes.byref(done))
                kernel.VirtualProtectEx(process, address, 1, old.value, ctypes.byref(old))
                kernel.FlushInstructionCache(process, address, 1)
                thread = kernel.OpenThread(0x0008 | 0x0010, False, event.tid)
                ctypes.c_uint32.from_address(aligned + 0x30).value = 0x00100001  # CONTEXT_CONTROL
                kernel.GetThreadContext(thread, aligned)
                ctypes.c_uint64.from_address(aligned + 0xF8).value = address  # Rip back onto the function
                kernel.SetThreadContext(thread, aligned)
                kernel.CloseHandle(thread)
                if offset in closing:
                    player[3] = True
                if not player[3]:
                    ran.add(offset)
            elif exception != 0x80000003:
                status = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED: the player's own exceptions
        elif code == 5:  # EXIT_PROCESS
            players.pop(pid, None)
        kernel.ContinueDebugEvent(pid, event.tid, status)
        if code == 5 and pid == companion:
            break
    kernel.CloseHandle(info.process)
    kernel.CloseHandle(info.thread)
    return ran


def write_order(map_path, profile_path, out):
    """Writes the link order: every profiled function, then every other one, one name per function.

    The rest are listed too because the linker otherwise slips unlisted functions in among the listed
    ones. Each part is sorted by name, which puts a template's instances and a class's members side
    by side: cdpack compresses each 256 KiB of cold code on its own, and sorted it packed 33 KB
    smaller than in link order (November 16, 2026). Returns True when the file changed."""
    wanted = {int(line.split()[0], 16) for line in Path(profile_path).read_text().splitlines() if line.strip()}
    names, rest = [], []
    for _, aliases in map_functions(Path(map_path).read_text(errors="replace")):
        hot = [name for obj, name in aliases if code_profile_hash(obj, name) in wanted]
        (names if hot else rest).append(hot[0] if hot else aliases[0][1])
    text = "".join(f"{name}\n" for name in dict.fromkeys(sorted(names) + sorted(rest)))
    out = Path(out)
    if out.exists() and out.read_text() == text:
        return False
    out.write_text(text, newline="\n")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fixtures", default="build/fixtures")
    parser.add_argument("--map", default="build/cordinal.map")
    parser.add_argument("--out", default="tools/code-profile.txt")
    parser.add_argument("--order", default="build/obj/code-order.txt")
    parser.add_argument("--write-order", action="store_true",
                        help="only write --order from --out (the profile) and --map; tools/build.ps1 does this")
    args = parser.parse_args()
    if args.write_order:
        write_order(ROOT / args.map, ROOT / args.out, ROOT / args.order)
        return
    exe = (ROOT / "build/unpacked/cordinal.exe").resolve()
    rva, size, stamp = text_section(exe)
    text = (ROOT / args.map).read_text(errors="replace")
    found = re.search(r"Timestamp is ([0-9a-f]{8})", text)
    if not found or int(found.group(1), 16) != stamp:
        raise SystemExit("make-code-profile: the map does not belong to build/unpacked/cordinal.exe; relink")
    functions = map_functions(text)
    starts = {offset for offset, _ in functions}
    closing = {offset for offset, aliases in functions for _, name in aliases if name in CLOSING}
    if len(closing) != len(CLOSING):
        raise SystemExit("make-code-profile: the close path's functions are not in the map")
    ran = trace_scenarios([str(ROOT / "build/unpacked/cordinal-test.exe"), "--memory-report", str(exe),
                           str(ROOT / args.fixtures)], exe, rva, starts, closing)
    if len(ran) < 1000:
        raise SystemExit(f"make-code-profile: the scenarios did not run ({len(ran)} functions)")
    lines, hot_bytes = set(), 0
    for index, (offset, aliases) in enumerate(functions):
        if offset not in ran:
            continue
        end = max(functions[index + 1][0] if index + 1 < len(functions) else size, offset + 1)
        hot_bytes += end - offset
        lines.update(code_profile_hash(obj, name) for obj, name in aliases)
    Path(ROOT / args.out).write_text("".join(f"{value:08x}\n" for value in sorted(lines)), newline="\n")
    print(f"{len(ran)} of {len(functions)} functions ran, {hot_bytes / 1048576:.2f} MiB; written to {args.out}")


if __name__ == "__main__":
    main()
