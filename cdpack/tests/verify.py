"""Independent format, corruption, exact-buffer, TLS and forced-ASLR regression checks."""
import argparse
import importlib.util
import os
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



def exported(pe):
    """The fixture's exports by name, as RVAs."""
    at, size = pe.directory(0)
    table = pe.at(at, size)
    names, ordinals, functions = (pack.u32(table, offset) for offset in (32, 36, 28))
    result = {}
    for i in range(pack.u32(table, 24)):
        name = pe.data[pe.offset(pack.u32(pe.at(names + i * 4, 4), 0)):]
        ordinal = pack.u16(pe.at(ordinals + i * 2, 2), 0)
        result[name[:name.index(0)].decode()] = pack.u32(pe.at(functions + ordinal * 4, 4), 0)
    return result


def run(path, mode=None):
    environment = dict(os.environ)
    environment.pop("CDP_FIXTURE", None)
    if mode:
        environment["CDP_FIXTURE"] = mode
    result = subprocess.run([str(path)], timeout=20, env=environment)
    assert result.returncode == 0, (path.name, mode, result.returncode)


for name in ("fixture.exe", "fixture.dll"):
    native_path = out / name
    native = native_path.read_bytes()
    # The overlay is outside all sections and must survive both container and file reconstruction.
    native += b"CDP exact overlay\0\xff"
    packed, _, _ = pack.pack_image(native, out / "loader.exe")
    again, _, _ = pack.pack_image(native, out / "loader.exe")
    assert packed == again, "nondeterministic packing"
    # Every thunk Windows binds lies inside the IAT directory, so the loader unprotects only those
    # pages and the rest of CDP1 stays shared with the file instead of becoming private memory.
    image = pack.Pe(packed)
    iat, iat_bytes = image.directory(12)
    imports, import_bytes = image.directory(1)
    assert iat and iat_bytes <= 4096, (iat, iat_bytes)
    for at in range(imports, imports + import_bytes - 20, 20):
        thunk = pack.u32(packed, image.offset(at) + 16)
        assert iat <= thunk and thunk + 16 <= iat + iat_bytes, "import thunks outside the IAT directory"
    assert pack.unpack_image(packed, out / "decode.exe", scratch) == native
    candidate = scratch / name
    candidate.write_bytes(packed)
    if name.endswith(".dll"):
        pe = pack.Pe(native)
        subprocess.run([str(out / "dll-host.exe"), str(native_path), hex(pe.base), str(pe.size)], check=True)
        subprocess.run([str(out / "dll-host.exe"), str(candidate), hex(pe.base), str(pe.size)], check=True)
    else:
        run(native_path)
        run(candidate)
    # Code the profile marks cold stays compressed until first touched. The fixture's whole .text
    # is cold, so its entry point and the DIR64 fixup inside it are decoded by the fault handler;
    # fixture_probe checks that the fixup was applied. Cold code makes the image detached: restored
    # into private memory of its own that Windows is told is the program (fixture_probe checks its
    # module identity). A DLL stays eager and attached: its handler would point at code the unload
    # takes away.
    pe = pack.Pe(native)
    text = next(s for s in pe.sections if s["name"] == b".text")
    cold = set(range(text["rva"] // 4096, (text["rva"] + text["raw_bytes"] + 4095) // 4096))
    if name.endswith(".dll"):
        try:
            pack.pack_image(native, out / "loader.exe", cold=cold)
            raise AssertionError("a DLL accepted cold code")
        except ValueError:
            pass
        try:
            pack.pack_image(native, out / "loader.exe", detach=True)
            raise AssertionError("a DLL was detached")
        except ValueError:
            pass
    else:
        lazy, _, report = pack.pack_image(native, out / "loader.exe", cold=cold)
        assert report["LazyChunks"] == 1 and report["LazyBytes"] == len(cold) * 4096 >= 8192, report
        assert report["Detached"] and report["HookRva"], report
        assert pack.unpack_image(lazy, out / "decode.exe", scratch) == native
        assert not any(s["name"] == b"CDP0" for s in pack.Pe(lazy).sections), "a detached image kept CDP0"
        candidate.write_bytes(lazy)
        run(candidate)
        # Detached without cold code: the same module identity, every chunk decoded at start-up.
        detached, _, report = pack.pack_image(native, out / "loader.exe", detach=True)
        assert report["Detached"] and not report["LazyChunks"], report
        assert pack.unpack_image(detached, out / "decode.exe", scratch) == native
        candidate.write_bytes(detached)
        run(candidate)
        # fixture_far, fixture_blocker and fixture_fence each start a page; one cold chunk per page.
        # "evict": decommit, decode again, a thread blocked inside an evicted page, suspension.
        # "cap": two resident cold chunks at most, the one decoded longest ago decommitted first.
        symbols = exported(pe)
        own = {symbols[s] // 4096 for s in ("fixture_far", "fixture_blocker", "fixture_fence")}
        assert len(own) == 3 and symbols["fixture_probe"] // 4096 not in own, symbols
        for mode, cap in (("evict", 0), ("cap", 2)):
            paged, _, report = pack.pack_image(native, out / "loader.exe", cold=own, lazy_block=4096, cold_cap=cap)
            assert report["LazyChunks"] == 3 and report["ColdCap"] == cap, report
            assert pack.unpack_image(paged, out / "decode.exe", scratch) == native
            candidate.write_bytes(paged)
            run(candidate, mode)
        # Damage in a cold stream stops the process at start-up, by the up-front input checksum,
        # rather than only when the chunk is first decoded.
        section = next(s for s in pack.Pe(lazy).sections if s["name"] == b"CDP1")["raw"]
        fields = pack.HEADER.unpack_from(lazy, section)
        damaged = bytearray(lazy)
        damaged[section + fields[6] + fields[7] - 1] ^= 1
        (scratch / "cold-damaged.exe").write_bytes(bytes(damaged))
        result = subprocess.run([str(scratch / "cold-damaged.exe")], timeout=10)
        assert result.returncode & 0xffffffff == 0xcd000026, hex(result.returncode & 0xffffffff)
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
            run(candidate)
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
        # The up-front input checksums see the damaged stream before any chunk decodes.
        assert result.returncode & 0xffffffff == 0xcd000026, hex(result.returncode & 0xffffffff)
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
        assert result.returncode & 0xffffffff == 0xcd000026, hex(result.returncode & 0xffffffff)
    # The fixture delay-loads version.dll (fixture_probe checks it maps on first call). The packed
    # headers leave the directory out; the restored image keeps it. A descriptor that is not
    # RVA-based is refused rather than packed.
    pe = pack.Pe(native)
    delay, delay_bytes = pe.directory(13)
    assert delay and delay_bytes >= 64, (delay, delay_bytes)
    assert pack.Pe(packed).directory(13) == (0, 0)
    broken_delay = bytearray(native)
    struct.pack_into("<I", broken_delay, pe.offset(delay), 0)
    try:
        pack.pack_image(bytes(broken_delay), out / "loader.exe")
        raise AssertionError("a delay-import descriptor without RVAs was accepted")
    except ValueError:
        pass
    cfg = bytearray(native)
    pe = pack.Pe(cfg)
    struct.pack_into("<H", cfg, pe.opt + 70, pack.u16(cfg, pe.opt + 70) | 0x4000)
    try:
        pack.pack_image(cfg, out / "loader.exe")
        raise AssertionError("unsupported CFG accepted")
    except ValueError:
        pass
print(f"PASS {checksum_checks} checksum oracles, {len(cases)} decoder corpora, mutations, exact guards, determinism, overlay, EXE/TLS/DLL/ASLR, detached images, cold code decommitted and decoded again, the cold cap, chunked threads and corruption")
