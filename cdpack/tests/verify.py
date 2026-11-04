"""Independent format, corruption, exact-buffer, TLS and forced-ASLR regression checks."""
import argparse
import importlib.util
import lzma
from pathlib import Path
import random
import struct
import subprocess
import zlib

parser = argparse.ArgumentParser()
parser.add_argument("--tools", type=Path, required=True)
args = parser.parse_args()
out = args.tools.resolve()
spec = importlib.util.spec_from_file_location("pack", Path(__file__).resolve().parents[1] / "tools/pack.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)
scratch = out / "tests"
scratch.mkdir(exist_ok=True)
rng = random.Random(0x43445031)
checksum_rng = random.Random(0x41444c52)
checksum_checks = 0
for size in (0, 1, 3, 4, 7, 8, 9, 15, 16, 31, 5551, 5552, 5553, 11103, 11104, 11105, 100000):
    for data in (checksum_rng.randbytes(size), b"\xff" * size):
        source = scratch / "checksum.bin"
        source.write_bytes(data)
        for skip in (None, 0, 1, max(0, size - 2), size, size + 1):
            expected = bytearray(data)
            command = [str(out / "decode.exe"), "--checksum", str(source)]
            if skip is not None:
                command.append(str(skip))
                expected[skip:min(skip + 4, size)] = b"\0" * max(0, min(4, size - skip))
            result = subprocess.run(command, check=True, capture_output=True, text=True)
            assert int(result.stdout) == zlib.adler32(expected), (size, skip)
            checksum_checks += 1
cases = [b"", b"a", b"ab", b"a" * 4096, b"abc" * 4000, bytes(range(256)) * 20,
         rng.randbytes(8192), b"\xe8\x01\0\0\0\x48\x8d\x05\xff\xff\xff\xff" * 400]
cases += [rng.randbytes(size) for size in (2, 3, 4, 7, 15, 31, 63, 127, 255, 4095, 4096, 4097)]
cases += [(rng.randbytes(1024) * 8 + b"\0" * 8192) for _ in range(4)]
# Exercise self-overlapping copies and word-sized distances with a non-periodic tail.
cases += [bytes(range(period)) * 600 + b"tail" for period in (2, 4, 5, 6, 7, 8, 9)]
for index, data in enumerate(cases):
    source, expected = scratch / f"case-{index}.packed", scratch / f"case-{index}.bin"
    stream = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[dict(
        id=lzma.FILTER_LZMA1, dict_size=1 << 23, lc=3, lp=0, pb=2)])
    source.write_bytes(stream)
    expected.write_bytes(data)
    subprocess.run([str(out / "decode.exe"), "--test", str(source), str(expected), str(len(data))], check=True)
    source.write_bytes(stream + b"\0")
    result = subprocess.run([str(out / "decode.exe"), "--decode", str(source), str(scratch / "unexpected.bin"), str(len(data))])
    assert result.returncode != 0, "trailing byte was accepted"

for name in ("fixture.exe", "fixture.dll"):
    native_path = out / name
    native = native_path.read_bytes()
    # The overlay is outside all sections and must survive both container and file reconstruction.
    native += b"CDP exact overlay\0\xff"
    packed, _, _ = pack.pack_image(native, out / "loader.exe")
    again, _, _ = pack.pack_image(native, out / "loader.exe")
    assert packed == again, "nondeterministic packing"
    assert pack.unpack_image(packed, out / "decode.exe", scratch) == native
    candidate = scratch / name
    candidate.write_bytes(packed)
    if name.endswith(".dll"):
        pe = pack.Pe(native)
        subprocess.run([str(out / "dll-host.exe"), str(native_path), hex(pe.base), str(pe.size)], check=True)
        subprocess.run([str(out / "dll-host.exe"), str(candidate), hex(pe.base), str(pe.size)], check=True)
    else:
        subprocess.run([str(native_path)], check=True, timeout=10)
        subprocess.run([str(candidate)], check=True, timeout=10)
    # Several chunks: an EXE decodes them on worker threads at its entry point and still runs its
    # native process-attach TLS callback exactly once and no thread callback for the workers
    # (pe_fixture.cpp counts both); a DLL decodes them in turn under the loader lock.
    for count in (2, 3):
        chunked, _, report = pack.pack_image(native, out / "loader.exe", chunks=count)
        assert report["Chunks"] > 1, report
        assert pack.unpack_image(chunked, out / "decode.exe", scratch) == native
        candidate.write_bytes(chunked)
        if name.endswith(".dll"):
            subprocess.run([str(out / "dll-host.exe"), str(candidate), hex(pe.base), str(pe.size)], check=True)
        else:
            subprocess.run([str(candidate)], check=True, timeout=10)
    fields = pack.HEADER.unpack_from(chunked, next(s for s in pack.Pe(chunked).sections if s["name"] == b"CDP1")["raw"])
    meta = next(s for s in pack.Pe(chunked).sections if s["name"] == b"CDP1")["raw"]
    damaged = bytearray(chunked)
    damaged[meta + fields[6] + fields[7] - 3] ^= 1  # inside the last chunk's stream
    try:
        pack.unpack_image(damaged, out / "decode.exe", scratch)
        raise AssertionError("damaged chunk accepted")
    except (ValueError, subprocess.CalledProcessError):
        pass
    if name.endswith(".exe"):
        broken = scratch / "broken-chunk.exe"
        broken.write_bytes(damaged)
        result = subprocess.run([str(broken)], timeout=10)
        assert result.returncode & 0xffffffff == 0xcd000017, hex(result.returncode & 0xffffffff)
    damaged = bytearray(packed)
    meta = next(s for s in pack.Pe(packed).sections if s["name"] == b"CDP1")["raw"]
    damaged[meta + 20] ^= 1
    try:
        pack.unpack_image(damaged, out / "decode.exe", scratch)
        raise AssertionError("damaged metadata accepted")
    except ValueError:
        pass
    fields = pack.HEADER.unpack_from(packed, meta)
    damaged = bytearray(packed)
    damaged[meta + fields[6] + fields[7] // 2] ^= 1
    try:
        pack.unpack_image(damaged, out / "decode.exe", scratch)
        raise AssertionError("damaged stream accepted")
    except (ValueError, subprocess.CalledProcessError):
        pass
    # The same corrupted image must fail before any native callback/entry runs.
    if name.endswith(".exe"):
        broken = scratch / "broken.exe"
        broken.write_bytes(damaged)
        result = subprocess.run([str(broken)], timeout=10)
        assert result.returncode & 0xffffffff == 0xcd000017, hex(result.returncode & 0xffffffff)
    cfg = bytearray(native)
    pe = pack.Pe(cfg)
    struct.pack_into("<H", cfg, pe.opt + 70, pack.u16(cfg, pe.opt + 70) | 0x4000)
    try:
        pack.pack_image(cfg, out / "loader.exe")
        raise AssertionError("unsupported CFG accepted")
    except ValueError:
        pass
print(f"PASS {checksum_checks} checksum oracles, {len(cases)} decoder corpora, mutations, exact guards, determinism, overlay, EXE/TLS/DLL/ASLR, chunked threads and corruption")
