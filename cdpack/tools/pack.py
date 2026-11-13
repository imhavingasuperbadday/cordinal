#!/usr/bin/env python3
"""Independent CDP1 PE64 packer/verifier (format version 3). No UPX invocation or UPX-format support."""
import argparse
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time
import zlib


HEADER = struct.Struct("<8s15I32s")
# format.h Chunk: output bytes, input offset/bytes, output and input Adler-32, flags.
CHUNK = struct.Struct("<6I")
# format.h Extent: offset from memory_rva, bytes, chunk. Sorted by offset; each chunk's output is
# the bytes of its extents in that order.
EXTENT = struct.Struct("<3I")
LAZY = 1  # Chunk flags: decoded when its code first runs instead of at start-up
MAGIC = b"CDPACK1\0"
VERSION = 3
MAX_CHUNKS = 64
MAX_EAGER = 8  # chunks decoded at start-up, each on its own thread
MAX_EXTENTS = 4096
WORKSPACE_SLOT = 32768  # format.h workspace_slot: one model set per decoding thread
# Cold code is cut into blocks of about this much, each decoded as a whole on its first fault:
# smaller blocks leave more code compressed and compress worse, larger ones decode more than runs.
LAZY_BLOCK = 128 * 1024
# Images at least this large are split into DEFAULT_CHUNKS streams the loader decodes in parallel.
# Four cost about 4% of the stream on cordinal's player and cut its decode time about threefold.
DEFAULT_CHUNKS = 4
CHUNKED_FROM = 1 << 20
# Runs of at least this many zero pages outside code are left out of the chunks: the loader never
# writes them, so they stay demand-zero instead of private memory. The player's .data has one of
# 207 pages (its uninitialized tail).
ZERO_GAP_PAGES = 16
ANCHOR = b"CDPANCHOR20261\0\0"
LIMIT = 128 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def align(value, boundary=4096):
    return (value + boundary - 1) // boundary * boundary


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]


def w32(data, offset, value):
    struct.pack_into("<I", data, offset, value & 0xffffffff)


def w64(data, offset, value):
    struct.pack_into("<Q", data, offset, value)


class Pe:
    def __init__(self, data):
        self.data = data
        require(64 <= len(data) <= LIMIT and data[:2] == b"MZ", "expected a bounded PE file")
        self.pe = u32(data, 60)
        require(self.pe + 264 <= len(data) and data[self.pe:self.pe + 4] == b"PE\0\0", "PE header")
        require(u16(data, self.pe + 4) == 0x8664 and u16(data, self.pe + 24) == 0x20b, "only AMD64 PE32+")
        self.opt = self.pe + 24
        self.table = self.opt + u16(data, self.pe + 20)
        count = u16(data, self.pe + 6)
        require(0 < count <= 96 and self.table + count * 40 <= len(data), "section table")
        self.headers = u32(data, self.opt + 60)
        self.size = u32(data, self.opt + 56)
        self.base = u64(data, self.opt + 24)
        require(self.table + count * 40 <= self.headers <= len(data) and self.size <= LIMIT, "image/header extent")
        require(u32(data, self.opt + 108) >= 16, "sixteen PE directories required")
        self.sections = []
        for index in range(count):
            at = self.table + index * 40
            name, virtual, rva, raw_bytes, raw, flags = struct.unpack_from("<8sIIII12xI", data, at)
            require(rva + max(virtual, raw_bytes) <= self.size and raw + raw_bytes <= len(data), "section extent")
            self.sections.append(dict(name=name.rstrip(b"\0"), virtual=virtual, rva=rva,
                                      raw_bytes=raw_bytes, raw=raw, flags=flags))

    def directory(self, index):
        return struct.unpack_from("<II", self.data, self.opt + 112 + index * 8)

    def offset(self, rva, size=1):
        if rva < self.headers and size <= self.headers - rva:
            return rva
        for section in self.sections:
            relative = rva - section["rva"]
            if 0 <= relative <= section["raw_bytes"] and size <= section["raw_bytes"] - relative:
                return section["raw"] + relative
        raise ValueError(f"RVA {rva:#x}+{size} is not file-backed")

    def at(self, rva, size):
        offset = self.offset(rva, size)
        return self.data[offset:offset + size]

    def memory(self):
        result = bytearray(self.size)
        result[:self.headers] = self.data[:self.headers]
        for section in self.sections:
            rva, raw, size = section["rva"], section["raw"], section["raw_bytes"]
            result[rva:rva + size] = self.data[raw:raw + size]
        return result

    def resources(self):
        root, size = self.directory(2)
        if not root:
            return [], 0
        require(size and root + size <= self.size, "resource extent")
        leaves, visiting = [], set()
        prefix_end = 0

        def inside(at, length):
            nonlocal prefix_end
            require(0 <= at <= size and length <= size - at, "resource metadata extent")
            prefix_end = max(prefix_end, at + length)
            return self.at(root + at, length)

        def walk(at, path):
            require(at not in visiting and len(path) < 16, "cyclic/deep resource tree")
            visiting.add(at)
            directory = inside(at, 16)
            count = u16(directory, 12) + u16(directory, 14)
            entries = inside(at + 16, count * 8)
            for i in range(count):
                name, target = struct.unpack_from("<II", entries, i * 8)
                if name & 0x80000000:
                    string_at = name & 0x7fffffff
                    length = u16(inside(string_at, 2), 0)
                    label = "@" + inside(string_at + 2, length * 2).decode("utf-16-le")
                else:
                    label = f"#{name}"
                child_path = path + (label,)
                if target & 0x80000000:
                    walk(target & 0x7fffffff, child_path)
                else:
                    entry = inside(target, 16)
                    data, length = struct.unpack_from("<II", entry)
                    require(data + length <= self.size, "resource payload extent")
                    leaves.append(dict(path=child_path, entry=target, rva=data, bytes=length))
            visiting.remove(at)
        walk(0, ())
        require(len({leaf["path"] for leaf in leaves}) == len(leaves), "duplicate resource path")
        return leaves, prefix_end


def filter_code(data, start, size, inverse, rva=None, visited=None):
    """Converts disp32 operands in place. `rva` is the address of `data[start]`; it defaults to
    start, the layout of a whole memory image. A segment cut out of the image passes its own."""
    base = 0 if rva is None else rva - start
    end, at = start + size, start
    while at < end:
        if visited is not None:
            visited[at] = 1
        prefix, op = 0, data[at]
        if op in (0xe8, 0xe9):
            prefix = 1
        elif end - at >= 2 and ((op == 0x0f and data[at + 1] & 0xf0 == 0x80) or
                               (op == 0xff and data[at + 1] in (0x15, 0x25))):
            prefix = 2
        elif end - at >= 3 and op & 0xf0 == 0x40 and data[at + 1] in (0x8b, 0x89, 0x8d) and data[at + 2] & 0xc7 == 5:
            prefix = 3
        if prefix and end - at >= prefix + 4:
            next_rva = base + at + prefix + 4
            old = u32(data, at + prefix)
            w32(data, at + prefix, old - next_rva if inverse else old + next_rva)
            at += prefix + 4
        else:
            at += 1


# Delay imports need nothing from the loader: the linker's helper reaches each descriptor through
# the thunk that calls it, and writes the module handle and the IAT slot itself, so restoring the
# image with its relocations is enough. The packed headers leave the directory out, as they leave
# out the load configuration. The helper reads the headers at __ImageBase only when the load config
# asks it to protect the delay IAT, which would find the packed ones, so that flag is refused.
def validate_delay_imports(pe):
    at, size = pe.directory(13)
    if not at and not size:
        return
    require(at and size >= 32 and at + size <= pe.size, "delay-import directory extent")
    config, config_size = pe.directory(10)
    if config and config_size >= 0x94:
        require(not u32(pe.at(config, config_size), 0x90) & 0x1000, "protected delay-load IATs are unsupported")

    def writable(rva, length):
        return any(s["rva"] <= rva and rva + length <= s["rva"] + align(max(s["virtual"], s["raw_bytes"]))
                   and s["flags"] & 0x80000000 for s in pe.sections)

    descriptors = pe.memory()[at:at + size]
    for cursor in range(0, size - 31, 32):
        attributes, name, module, iat, names, bound, unload, _ = struct.unpack_from("<8I", descriptors, cursor)
        if not any((attributes, name, module, iat, names, bound, unload)):
            return
        require(attributes == 1, "delay-import descriptors must hold RVAs")
        require(all(0 < rva < pe.size for rva in (name, iat, names)), "delay-import descriptor outside the image")
        require(writable(module, 8) and writable(iat, 8), "delay-import handle and IAT must be writable")
        require(all(rva == 0 or rva < pe.size for rva in (bound, unload)), "delay-import tables outside the image")
    raise ValueError("delay-import directory has no terminating descriptor")


def validate_native(pe):
    require(u32(pe.data, pe.opt + 32) == 4096, "native section alignment must be 4096")
    require(not any(s["name"].startswith((b"CDP", b"UPX")) for s in pe.sections), "input is already packed")
    require(not u16(pe.data, pe.opt + 70) & 0x4000, "CFG images require a dedicated loader implementation")
    for i in (4, 7, 8, 11, 14, 15):
        require(pe.directory(i) == (0, 0), f"unsupported native directory {i}")
    validate_delay_imports(pe)
    virtual_end, raw_end = 4096, pe.headers
    for section in sorted(pe.sections, key=lambda s: s["rva"]):
        size = align(max(section["virtual"], section["raw_bytes"]))
        require(section["rva"] % 4096 == 0 and section["rva"] >= virtual_end, "overlapping/unaligned sections")
        require(section["flags"] & 0x40000000 and section["flags"] & 0xa0000000 != 0xa0000000,
                "sections must be readable and never W+X")
        virtual_end = section["rva"] + size
    require(virtual_end == pe.size, "unexpected native image end")
    for section in sorted(pe.sections, key=lambda s: s["raw"]):
        if section["raw_bytes"]:
            require(section["raw"] >= raw_end, "overlapping raw sections")
            raw_end = section["raw"] + section["raw_bytes"]
    require(pe.directory(5)[1] or not u16(pe.data, pe.opt + 70) & 0x40, "ASLR image needs relocations")
    config, size = pe.directory(10)
    if config:
        cfg = pe.at(config, size)
        # Default MSVC guard dispatch stubs do not enable CFG. Tables/OS extensions need tests.
        for offset in (0x80, 0x88, 0xa0, 0xa8, 0xb0, 0xb8, 0xc0, 0xc8, 0xf8, 0x108, 0x110):
            require(offset + 8 > size or not u64(cfg, offset), "unsupported load-config table")


def native_relocations(pe):
    at, size = pe.directory(5)
    data = pe.at(at, size) if size else b""
    targets, cursor = [], 0
    while cursor < size:
        require(cursor + 8 <= size, "relocation header extent")
        page, block = struct.unpack_from("<II", data, cursor)
        require(8 <= block <= size - cursor and block % 2 == 0, "relocation block extent")
        for offset in range(cursor + 8, cursor + block, 2):
            item = u16(data, offset)
            if item >> 12 == 0:
                continue
            require(item >> 12 == 10 and 4096 <= page + (item & 4095) <= pe.size - 8, "only native DIR64 relocations")
            targets.append(page + (item & 4095))
        cursor += block
    return targets


def resource_image(pe, memory):
    leaves, prefix = pe.resources()
    root, _ = pe.directory(2)
    blob = bytearray(pe.at(root, prefix)) if root else bytearray()
    copies, shared, rcdata = [], {}, []
    for leaf in leaves:
        if leaf["path"][0] == "#10":
            rcdata.append((leaf["rva"], leaf["rva"] + leaf["bytes"]))
    for leaf in leaves:
        if leaf["path"][0] == "#10":
            continue
        rva, size = leaf["rva"], leaf["bytes"]
        require(not any(rva < end and start < rva + size for start, end in rcdata), "resource alias crosses RCDATA")
        require(root <= rva and rva + size <= root + pe.directory(2)[1], "visible resource must be in .rsrc")
        key = rva, size
        if key not in shared:
            blob.extend(b"\0" * (align(len(blob), 4) - len(blob)))
            offset = len(blob)
            blob.extend(pe.at(rva, size))
            shared[key] = offset
            copies.append((offset, rva, size))
        w32(blob, leaf["entry"], shared[key])  # Converted to an RVA after layout.
        memory[rva:rva + size] = b"\0" * size
    return blob, copies, leaves


def loader_image(path):
    pe = Pe(path.read_bytes())
    require(not pe.directory(1)[1] and not pe.directory(5)[1], "loader must have no imports or base relocations")
    require(not any(s["flags"] & 0x80000000 for s in pe.sections), "loader must have no writable globals")
    included = [s for s in pe.sections if s["name"] in (b".text", b".rdata")]
    first = min(s["rva"] for s in included)
    end = max(s["rva"] + s["virtual"] for s in included)
    blob = bytearray(pe.memory()[first:end])
    require(blob.count(ANCHOR) == 1, "unique loader anchor required")
    exports, export_size = pe.directory(0)
    table = pe.at(exports, export_size)
    names = pe.at(u32(table, 32), u32(table, 24) * 4)
    ordinals = pe.at(u32(table, 36), u32(table, 24) * 2)
    addresses = pe.at(u32(table, 28), u32(table, 20) * 4)
    symbols = {}
    for i in range(u32(table, 24)):
        offset = pe.offset(u32(names, i * 4))
        end_name = pe.data.index(0, offset)
        symbols[pe.data[offset:end_name].decode("ascii")] = u32(addresses, u16(ordinals, i * 2) * 4) - first
    for name in ("cdp_entry", "cdp_tls"):
        require(name in symbols and 0 <= symbols[name] < len(blob), f"missing loader symbol {name}")
    return blob, symbols, blob.index(ANCHOR)


def exports_image(pe, new_rva):
    old_rva, size = pe.directory(0)
    if not old_rva:
        return bytearray()
    data = bytearray(pe.at(old_rva, size))

    def translated(value, length=1):
        require(old_rva <= value <= old_rva + size and length <= old_rva + size - value, "export data outside directory")
        return value - old_rva + new_rva

    for field in (12, 28, 32, 36):
        w32(data, field, translated(u32(data, field)))
    functions, count = u32(data, 28) - new_rva, u32(data, 20)
    names, named = u32(data, 32) - new_rva, u32(data, 24)
    require(functions + count * 4 <= size and names + named * 4 <= size, "export arrays")
    for i in range(count):
        rva = u32(data, functions + i * 4)
        if old_rva <= rva < old_rva + size:
            w32(data, functions + i * 4, translated(rva))
    for i in range(named):
        w32(data, names + i * 4, translated(u32(data, names + i * 4)))
    return data


def relocation_bytes(targets):
    pages = {}
    for target in targets:
        pages.setdefault(target & ~4095, []).append(0xa000 | (target & 4095))
    blob = bytearray()
    for page, entries in sorted(pages.items()):
        if len(entries) % 2:
            entries.append(0)
        blob += struct.pack("<II", page, 8 + len(entries) * 2)
        blob += struct.pack(f"<{len(entries)}H", *entries)
    return blob


def dependency_imports(pe, new_rva):
    at, size = pe.directory(1)
    descriptors = pe.at(at, size) if size else b""
    libraries = []
    for offset in range(0, size - 19, 20):
        lookup, _, _, name, iat = struct.unpack_from("<5I", descriptors, offset)
        if not name:
            break
        raw = pe.offset(name)
        library = pe.data[raw:pe.data.index(0, raw) + 1]
        item = u64(pe.at(lookup or iat, 8), 0)
        require(item, "empty import descriptor")
        symbol = None
        if not item >> 63:
            raw = pe.offset(item)
            symbol = pe.data[raw:pe.data.index(0, raw + 2) + 1]
        libraries.append((library, item, symbol))
    if not libraries:
        # Windows can omit TLS dispatch for an importless DLL. ntdll is already implicit in
        # every Windows process; this dependency keeps ordinary loader initialization active.
        libraries.append((b"ntdll.dll\0", 0, b"\0\0NtProtectVirtualMemory\0"))
    directory_bytes = (len(libraries) + 1) * 20
    data = bytearray(directory_bytes)
    tables = []
    for index, (library, item, symbol) in enumerate(libraries):
        name_rva = new_rva + len(data)
        data += library
        if symbol is not None:
            item = new_rva + len(data)
            data += symbol
        data += b"\0" * (align(len(data), 8) - len(data))
        table = new_rva + len(data)
        tables.append(table)
        data += struct.pack("<QQ", item, 0)
        struct.pack_into("<5I", data, index * 20, table, 0, 0, name_rva, table)
    # The IAT directory names exactly the thunks Windows writes. Without it, the Windows loader
    # makes the whole section holding them writable while it binds imports, and every page of
    # CDP1 (the packed stream, 1.5 MB in the player) became private memory instead of staying
    # shared with the file (measured November 9, 2026).
    return data, directory_bytes, (tables[0], tables[-1] + 16 - tables[0])


def lzma_raw(data):
    dictionary = max(1 << 23, 1 << max(1, (len(data) - 1).bit_length()))
    compressed = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[dict(
        id=lzma.FILTER_LZMA1, dict_size=dictionary, lc=3, lp=0, pb=2,
        mode=lzma.MODE_NORMAL, nice_len=80, mf=lzma.MF_BT4, depth=128)])
    oracle = lzma.decompress(compressed, format=lzma.FORMAT_RAW, filters=[dict(
        id=lzma.FILTER_LZMA1, dict_size=dictionary, lc=3, lp=0, pb=2)])
    require(oracle == data, "encoder/oracle byte equality")
    return compressed, dictionary


def zero_pages(pe, memory, copies, relocations):
    """Page numbers in runs of at least ZERO_GAP_PAGES zero pages outside code, resource copies and
    relocation targets. No chunk covers them: CDP0 is demand-zero, so they stay out of memory."""
    zero, pages = bytes(4096), set()
    written = [(destination, destination + size) for _, destination, size in copies]
    targeted = {target // 4096 for target in relocations}
    for section in pe.sections:
        if section["flags"] & 0x20000000:
            continue
        start, end = section["rva"], section["rva"] + align(max(section["virtual"], section["raw_bytes"]))
        run = start
        for page in range(start, end + 4096, 4096):
            written_page = any(low < page + 4096 and page < high for low, high in written)
            if (page < end and memory[page:page + 4096] == zero and not written_page and
                    page // 4096 not in targeted):
                continue
            if page - run >= ZERO_GAP_PAGES * 4096:
                pages.update(range(run // 4096, page // 4096))
            run = page + 4096
    return pages


def balanced(memory, pages, count):
    """Splits pages into `count` runs with about equal compressed size, which decode time follows."""
    if count <= 1 or len(pages) < count:
        return [pages]
    step = max(1, min(32, len(pages) // (count * 16)))
    weights = [len(lzma.compress(b"".join(memory[p * 4096:(p + 1) * 4096] for p in pages[i:i + step]),
                                 format=lzma.FORMAT_RAW, filters=[dict(id=lzma.FILTER_LZMA1, dict_size=1 << 23,
                                                                       lc=3, lp=0, pb=2)]))
               for i in range(0, len(pages), step)]
    total, cuts, running = sum(weights), [0], 0
    for index, weight in enumerate(weights):
        running += weight
        while len(cuts) < count and running >= total * len(cuts) / count:
            at = min((index + 1) * step, len(pages))
            if cuts[-1] < at < len(pages):
                cuts.append(at)
            else:
                break
    cuts.append(len(pages))
    return [pages[low:high] for low, high in zip(cuts, cuts[1:])]


def section_of(pe, rva):
    for section in pe.sections:
        if section["rva"] <= rva < section["rva"] + align(max(section["virtual"], section["raw_bytes"])):
            return section
    return None


def runs(pe, pages):
    """Sorted page numbers as (rva, bytes) extents of consecutive pages of one section. An extent
    never crosses a section, so it is entirely code or entirely data and one filter scan."""
    extents = []
    for page in pages:
        if extents and extents[-1][0] + extents[-1][1] == page * 4096 and \
                section_of(pe, extents[-1][0]) is section_of(pe, page * 4096):
            extents[-1][1] += 4096
        else:
            extents.append([page * 4096, 4096])
    return [tuple(extent) for extent in extents]


def layout(pe, memory, copies, count, cold, relocations):
    """Chunks as (flags, extents): `count` eager ones over every page that is neither zero (see
    zero_pages) nor cold, then ~LAZY_BLOCK-byte blocks of cold code. Eager chunks come first, so a
    chunk index below the eager count decodes at start-up."""
    first = min(s["rva"] for s in pe.sections)
    zero = zero_pages(pe, memory, copies, relocations)
    cold = {page for page in cold if page not in zero}
    eager = [page for page in range(first // 4096, pe.size // 4096) if page not in zero and page not in cold]
    pieces = [(0, runs(pe, part)) for part in balanced(memory, eager, count) if part]
    block = []
    for page in sorted(cold):
        block.append(page)
        if len(block) * 4096 >= LAZY_BLOCK:
            pieces.append((LAZY, runs(pe, block)))
            block = []
    if block:
        pieces.append((LAZY, runs(pe, block)))
    return pieces, len(zero)


def cold_pages(pe, hot, relocations):
    """Page numbers of executable sections that no hot range covers. A relocation whose eight bytes
    cross a page boundary keeps both pages eager, so the loader can apply it whole in one extent."""
    pages = set()
    for section in pe.sections:
        if section["flags"] & 0x20000000:
            pages.update(range(section["rva"] // 4096,
                               (section["rva"] + align(section["raw_bytes"])) // 4096))
    for low, high in hot:
        pages.difference_update(range(low // 4096, (high - 1) // 4096 + 1))
    for target in relocations:
        if target // 4096 != (target + 7) // 4096:
            pages.difference_update((target // 4096, (target + 7) // 4096))
    return pages


def code_profile_hash(obj, name):
    """32-bit FNV-1a of 'object|name', as tools/make-code-profile.py writes it."""
    value = 2166136261
    for byte in f"{obj}|{name}".encode():
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return value


MAP_FUNCTION = re.compile(r"^\s*0001:([0-9a-f]{8})\s+(\S+)\s+[0-9a-f]{16}\s+f\s.*?(\S+)\s*$")


def is_dll(pe):
    return bool(u16(pe.data, pe.pe + 22) & 0x2000)


def profile_code(pe, map_text, profile_text):
    """RVA ranges the profile marks hot, found through the linker map of exactly this image, and
    how many of the profile's function hashes the map still has. A hash the map lacks is a function
    that changed or disappeared since the profile was taken; its pages simply stay cold and decode
    when they first run."""
    stamp = re.search(r"Timestamp is ([0-9a-f]{8})", map_text)
    require(stamp and int(stamp.group(1), 16) == u32(pe.data, pe.pe + 8), "the map does not belong to this image")
    code = pe.sections[0]  # the map's section 0001
    require(code["flags"] & 0x20000000, "the map's first section is not code")
    listed = {}
    for line in profile_text.splitlines():
        if line.strip():
            value, _, spans = line.strip().partition(" ")
            listed[int(value, 16)] = [tuple(int(x, 16) for x in span.split(":")) for span in spans.split(",")] \
                if spans else None
    functions = {}
    for line in map_text.splitlines():
        match = MAP_FUNCTION.match(line)
        if match:
            functions.setdefault(int(match.group(1), 16), []).append((match.group(3).rpartition(":")[2], match.group(2)))
    addresses, ranges, found = sorted(functions), [], set()
    for index, offset in enumerate(addresses):
        end = max(addresses[index + 1] if index + 1 < len(addresses) else code["virtual"], offset + 1)
        hashes = [code_profile_hash(obj, name) for obj, name in functions[offset]]
        known = [value for value in hashes if value in listed]
        if not known:
            continue
        found.update(known)
        for low, high in listed[known[0]] or [(0, end - offset)]:
            ranges.append((code["rva"] + offset + low, code["rva"] + offset + min(high, end - offset)))
    return ranges, len(found), len(listed)


def pack_image(native, loader, chunks=None, cold=None, profile=None):
    pe = Pe(native)
    validate_native(pe)
    native_targets = native_relocations(pe)
    memory = pe.memory()
    resources, copies, leaves = resource_image(pe, memory)
    first = min(s["rva"] for s in pe.sections)
    if profile is not None:
        require(cold is None, "cold pages or a profile, not both")
        hot, found, total = profile_code(pe, *profile)
        require(found, f"none of the profile's {total} functions is in this image")
        # A DLL unloads the code its fault handler still points at, so it stays eager.
        cold = set() if is_dll(pe) else cold_pages(pe, hot, native_targets)
    require(not (cold and is_dll(pe)), "cold code needs an EXE")
    if chunks is None:
        chunks = DEFAULT_CHUNKS if pe.size - first >= CHUNKED_FROM else 1
    require(1 <= chunks <= MAX_EAGER, "chunk count")
    pieces, zero = layout(pe, memory, copies, chunks, cold or set(), native_targets)
    # Extents in image order. Each chunk's input is its extents' filtered bytes in that order.
    extents = []
    for index, (_, spans) in enumerate(pieces):
        for rva, size in spans:
            extents.append((rva - first, size, index))
    extents.sort()
    require(1 <= len(extents) <= MAX_EXTENTS, "extent count")
    filtered = bytearray(pe.size - first)
    compressed, table, dictionary = bytearray(), [], 0
    for index, (flags, _) in enumerate(pieces):
        data = bytearray()
        for offset, size, chunk in extents:
            if chunk != index:
                continue
            rva = first + offset
            segment = bytearray(memory[rva:rva + size])
            # Each extent is a scan of its own, so a cut may fall anywhere and the loader can
            # reverse one extent without having decoded its neighbours.
            if section_of(pe, rva)["flags"] & 0x20000000:
                filter_code(segment, 0, size, False, rva)
            data += segment
            filtered[offset:offset + size] = segment
        stream, size = lzma_raw(bytes(data))
        table.append((len(data), len(compressed), len(stream), zlib.adler32(data),
                      zlib.adler32(stream), flags))
        compressed += stream
        dictionary = max(dictionary, size)
    require(1 <= len(table) <= MAX_CHUNKS, "chunk count")
    gaps, cursor = [], pe.headers
    for section in sorted(pe.sections, key=lambda s: s["raw"]):
        if not section["raw_bytes"]:
            continue
        if cursor < section["raw"]:
            gaps.append((cursor, native[cursor:section["raw"]]))
        cursor = section["raw"] + section["raw_bytes"]
    overlay = native[cursor:]
    if overlay:
        gaps.append((cursor, overlay))
    extra = bytearray(native[:pe.headers])
    for offset, data in gaps:
        extra += struct.pack("<II", offset, len(data)) + data
    metadata_bytes = (HEADER.size + len(extra) + len(copies) * 12 + len(table) * CHUNK.size +
                      len(extents) * EXTENT.size)
    stub, symbols, anchor = loader_image(loader)
    stub_rva = pe.size
    data_rva = align(stub_rva + len(stub))
    tls_offset = align(metadata_bytes, 16)
    aux = bytearray(56)
    native_tls, _ = pe.directory(9)
    tls_index, zero_fill, characteristics, template_rva = 0, 0, 0, 0
    if native_tls:
        tls = pe.at(native_tls, 40)
        start, end, index = struct.unpack_from("<QQQ", tls)
        require(pe.base <= start <= end <= pe.base + pe.size and end - start <= 65536, "TLS template extent")
        require(pe.base <= index <= pe.base + pe.size - 4, "TLS index extent")
        tls_index = index - pe.base
        template_rva = start - pe.base
        zero_fill, characteristics = struct.unpack_from("<II", tls, 32)
        template = pe.at(start - pe.base, end - start)
    else:
        template = b""
    aux += template
    export_offset = tls_offset + align(len(aux), 16)
    aux.extend(b"\0" * (align(len(aux), 16) - len(aux)))
    export = exports_image(pe, data_rva + export_offset)
    aux += export
    aux.extend(b"\0" * (align(len(aux), 16) - len(aux)))
    import_offset = tls_offset + len(aux)
    dependency_data, import_bytes, iat = dependency_imports(pe, data_rva + import_offset)
    aux += dependency_data
    compressed_offset = align(tls_offset + len(aux), 16)
    data_bytes = compressed_offset + len(compressed)
    resource_rva = align(data_rva + data_bytes)
    for leaf in leaves:
        if leaf["path"][0] != "#10":
            w32(resources, leaf["entry"], u32(resources, leaf["entry"]) + resource_rva)
    workspace_rva = align(resource_rva + len(resources))
    # A state page, then one model slot per chunk, each deliberately larger than the checked
    # sizeof(Models); no model storage is on the stack. Never written to the file.
    workspace_bytes = 4096 + len(table) * WORKSPACE_SLOT
    reloc_rva = workspace_rva + workspace_bytes
    tls_rva = data_rva + tls_offset
    struct.pack_into("<QQQQII", aux, 0, pe.base + tls_rva + 56, pe.base + tls_rva + 56 + len(template),
                     pe.base + (tls_index or workspace_rva + 4), pe.base + tls_rva + 40,
                     zero_fill, characteristics)
    w64(aux, 40, pe.base + stub_rva + symbols["cdp_tls"])
    tls_fixups = [tls_rva + 56 + target - template_rva for target in native_targets
                  if template_rva <= target and target + 8 <= template_rva + len(template)]
    relocation = relocation_bytes([tls_rva + i for i in (0, 8, 16, 24, 40)] + tls_fixups)
    struct.pack_into("<4I", stub, anchor + 16, stub_rva + anchor, data_rva, metadata_bytes, workspace_rva)
    for source, destination, count in copies:
        extra += struct.pack("<III", resource_rva + source, destination, count)
    for chunk in table:
        extra += CHUNK.pack(*chunk)
    for extent in extents:
        extra += EXTENT.pack(*extent)
    fields = [MAGIC, VERSION, metadata_bytes, 0, first, pe.size - first, compressed_offset, len(compressed),
              zlib.adler32(filtered), len(native), pe.headers, len(gaps), len(copies), tls_index, len(table),
              len(extents), hashlib.sha256(native).digest()]
    metadata = bytearray(HEADER.pack(*fields)) + extra
    w32(metadata, 16, zlib.adler32(metadata))
    payload = metadata + b"\0" * (tls_offset - len(metadata)) + aux
    payload += b"\0" * (compressed_offset - len(payload)) + compressed
    sections = [(b"CDP0", first, pe.size - first, b"", 0xc0000080),
                (b"CDPX", stub_rva, len(stub), stub, 0x60000020),
                (b"CDP1", data_rva, len(payload), payload, 0x40000040)]
    if resources:
        sections.append((b".rsrc", resource_rva, len(resources), resources, 0x40000040))
    sections += [(b"CDPW", workspace_rva, workspace_bytes, b"", 0xc0000080),
                 (b".reloc", reloc_rva, len(relocation), relocation, 0x42000040)]
    header_bytes = align(pe.table + len(sections) * 40, 512)
    result = bytearray(header_bytes)
    result[:pe.table] = native[:pe.table]
    struct.pack_into("<H", result, pe.pe + 6, len(sections))
    w32(result, pe.pe + 12, 0); w32(result, pe.pe + 16, 0)
    w32(result, pe.opt + 4, align(len(stub), 512))
    w32(result, pe.opt + 8, align(len(payload), 512) + align(len(resources), 512) + align(len(relocation), 512))
    w32(result, pe.opt + 12, pe.size - first + workspace_bytes)
    w32(result, pe.opt + 16, stub_rva + symbols["cdp_entry"])
    w32(result, pe.opt + 20, stub_rva)
    w32(result, pe.opt + 36, 512)
    w32(result, pe.opt + 56, align(reloc_rva + len(relocation)))
    w32(result, pe.opt + 60, header_bytes); w32(result, pe.opt + 64, 0)
    result[pe.opt + 112:pe.opt + 240] = b"\0" * 128
    directories = {0: (data_rva + export_offset, len(export)) if export else (0, 0),
                   1: (data_rva + import_offset, import_bytes) if import_bytes else (0, 0),
                   2: (resource_rva, len(resources)) if resources else (0, 0),
                   3: pe.directory(3), 5: (reloc_rva, len(relocation)), 9: (tls_rva, 40), 12: iat}
    for index, pair in directories.items():
        struct.pack_into("<II", result, pe.opt + 112 + index * 8, *pair)
    for i, (name, rva, virtual, data, flags) in enumerate(sections):
        raw_bytes = align(len(data), 512) if data else 0
        raw = len(result) if data else 0
        struct.pack_into("<8sIIIIIIHHI", result, pe.table + i * 40, name, virtual, rva, raw_bytes, raw, 0, 0, 0, 0, flags)
        if data:
            result += data + b"\0" * (raw_bytes - len(data))
    result += overlay
    return bytes(result), bytes(filtered), dict(DecoderBytes=len(stub), CompressedBytes=len(compressed),
        ExpandedBytes=pe.size - first, MetadataBytes=metadata_bytes, ResourcePayloads=len(leaves),
        DictionaryBytes=dictionary, FilterProfile=1, FormatVersion=VERSION, Chunks=len(table),
        LazyChunks=sum(1 for flags, _ in pieces if flags & LAZY),
        LazyBytes=sum(table[i][0] for i in range(len(table)) if table[i][5] & LAZY),
        Extents=len(extents), ZeroPagesSkipped=zero)


def unpack_image(packed, decoder, scratch):
    pe = Pe(packed)
    candidates = [s for s in pe.sections if s["name"] == b"CDP1"]
    require(len(candidates) == 1, "CDP1 section required")
    section = candidates[0]
    at = section["raw"]
    require(section["raw_bytes"] >= HEADER.size, "CDP1 header extent")
    fields = HEADER.unpack_from(packed, at)
    (magic, version, metadata_bytes, checksum, first, expanded, compressed_offset, compressed_bytes,
     memory_adler, file_bytes, native_header_bytes, gap_count, copy_count, tls_index, chunk_count,
     extent_count, digest) = fields
    require(magic == MAGIC and version == VERSION, "CDP1 magic/version")
    require(1 <= chunk_count <= MAX_CHUNKS and 1 <= extent_count <= MAX_EXTENTS, "chunk/extent counts")
    require(HEADER.size <= metadata_bytes <= section["raw_bytes"], "metadata extent")
    metadata = bytearray(packed[at:at + metadata_bytes])
    w32(metadata, 16, 0)
    require(zlib.adler32(metadata) == checksum, "metadata checksum")
    require(0 < first < LIMIT and first % 4096 == 0 and 0 < expanded <= LIMIT - first and file_bytes <= LIMIT,
            "expanded extent")
    require(metadata_bytes <= compressed_offset <= section["raw_bytes"] and
            compressed_bytes <= section["raw_bytes"] - compressed_offset, "compressed extent")
    require(0 < native_header_bytes <= metadata_bytes - HEADER.size and gap_count <= 97 and copy_count <= 4096,
            "metadata counts")
    stream = packed[at + compressed_offset:at + compressed_offset + compressed_bytes]
    extents_at = metadata_bytes - extent_count * EXTENT.size
    table_at = extents_at - chunk_count * CHUNK.size
    require(table_at >= HEADER.size, "chunk table extent")
    chunks = [CHUNK.unpack_from(metadata, table_at + i * CHUNK.size) for i in range(chunk_count)]
    extents = [EXTENT.unpack_from(metadata, extents_at + i * EXTENT.size) for i in range(extent_count)]
    # The extents are the chunks' output: page-aligned, sorted, never overlapping.
    cursor, totals, skipped = 0, [0] * chunk_count, []
    for offset, size, chunk in extents:
        require(chunk < chunk_count and size and offset % 4096 == 0 and size % 4096 == 0 and
                offset >= cursor and offset + size <= expanded, "extent placement")
        if cursor != offset:
            skipped.append((first + cursor, first + offset))
        cursor = offset + size
        totals[chunk] += size
    if cursor != expanded:
        skipped.append((first + cursor, first + expanded))
    for index, (output_bytes, input_at, input_bytes, adler, input_adler, flags) in enumerate(chunks):
        require(flags in (0, LAZY) and output_bytes == totals[index] and output_bytes and input_bytes,
                "chunk fields")
    decoded = bytearray(expanded)
    with tempfile.TemporaryDirectory(prefix="verify-", dir=scratch) as temp:
        source, destination = Path(temp) / "stream.packed", Path(temp) / "memory.bin"
        reached = 0
        for index, (output_bytes, input_at, input_bytes, adler, input_adler, flags) in enumerate(chunks):
            require(input_at == reached and input_bytes <= compressed_bytes - input_at, "chunk input extent")
            piece_bytes = stream[input_at:input_at + input_bytes]
            require(zlib.adler32(piece_bytes) == input_adler, "chunk input checksum")
            source.write_bytes(piece_bytes)
            subprocess.run([str(decoder), "--decode", str(source), str(destination), str(output_bytes)], check=True)
            piece = destination.read_bytes()
            require(len(piece) == output_bytes and zlib.adler32(piece) == adler, "chunk checksum")
            at_piece = 0
            for offset, size, chunk in extents:
                if chunk == index:
                    decoded[offset:offset + size] = piece[at_piece:at_piece + size]
                    at_piece += size
            require(at_piece == output_bytes, "chunk output coverage")
            reached = input_at + input_bytes
        require(reached == compressed_bytes, "chunk input coverage")
    require(zlib.adler32(decoded) == memory_adler, "memory checksum")
    memory = bytearray(first) + decoded
    header = metadata[HEADER.size:HEADER.size + native_header_bytes]
    # Pe validates raw extents too; use the declared native file size as a zero-filled template.
    require(native_header_bytes <= file_bytes, "native header extent")
    result = bytearray(file_bytes)
    result[:native_header_bytes] = header
    native = Pe(result)
    require(native.size == len(memory), "native memory size")
    for low, high in skipped:
        require(not any(s["flags"] & 0x20000000 and low < s["rva"] + s["raw_bytes"] and s["rva"] < high
                        for s in native.sections), "skipped zero pages inside code")
        require(memory[low:high] == bytes(high - low), "skipped pages are zero")
    # Exactly as the loader does: every extent reverses its own scan, so both the memory checksum
    # above and the full-file hash below prove every extent boundary.
    for offset, size, chunk in extents:
        begin = first + offset
        if any(s["flags"] & 0x20000000 and s["rva"] <= begin < s["rva"] + s["raw_bytes"] for s in native.sections):
            filter_code(memory, begin, size, True)
    cursor = HEADER.size + native_header_bytes
    gaps = []
    for _ in range(gap_count):
        require(cursor + 8 <= metadata_bytes, "gap header")
        offset, size = struct.unpack_from("<II", metadata, cursor)
        cursor += 8
        require(cursor + size <= metadata_bytes and offset + size <= file_bytes, "gap extent")
        gaps.append((offset, metadata[cursor:cursor + size]))
        cursor += size
    require(cursor + copy_count * 12 == table_at, "copy table extent")
    for i in range(copy_count):
        source, destination, size = struct.unpack_from("<III", metadata, cursor + i * 12)
        require(first <= destination and destination + size <= len(memory), "resource restore extent")
        memory[destination:destination + size] = pe.at(source, size)
    for section in native.sections:
        raw, size, rva = section["raw"], section["raw_bytes"], section["rva"]
        result[raw:raw + size] = memory[rva:rva + size]
    for offset, data in gaps:
        result[offset:offset + len(data)] = data
    require(hashlib.sha256(result).digest() == digest, "full native-file SHA-256")
    restored = Pe(result)
    require(u16(pe.data, pe.opt + 68) == u16(result, restored.opt + 68) and
            u16(pe.data, pe.opt + 70) == u16(result, restored.opt + 70), "subsystem/security flags")
    original_resources, _ = restored.resources()
    packed_resources, _ = pe.resources()
    require([r["path"] for r in original_resources] == [r["path"] for r in packed_resources], "resource names/order")
    for original, visible in zip(original_resources, packed_resources):
        if original["path"][0] != "#10":
            require(restored.at(original["rva"], original["bytes"]) == pe.at(visible["rva"], visible["bytes"]),
                    "visible resource payload")
    return bytes(result)


def atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "wb") as file:
            file.write(data)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("pack", "unpack", "test", "check-loader"))
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--tools", type=Path, default=Path(__file__).resolve().parents[2] / "build/cdpack")
    parser.add_argument("--report", type=Path)
    parser.add_argument("--profile", type=Path, help="hot-function profile from tools/make-code-profile.py")
    parser.add_argument("--map", type=Path, help="linker map of the input image, with --profile")
    args = parser.parse_args()
    tools = args.tools.resolve()
    if args.mode == "check-loader":
        blob, symbols, _ = loader_image(args.input)
        print(f"loader bytes={len(blob)} symbols={symbols} imports=0 relocations=0 writable-globals=0")
        return
    require(args.profile is None or args.map is not None, "--profile needs --map")
    source = args.input.read_bytes()
    if args.mode == "pack":
        require(args.output is not None, "pack needs an output path")
        profile = None
        if args.profile:
            profile = (args.map.read_text(errors="replace"), args.profile.read_text())
        start = time.perf_counter()
        packed, expanded, report = pack_image(source, tools / "loader.exe", profile=profile)
        elapsed = (time.perf_counter() - start) * 1000
        restored = unpack_image(packed, tools / "decode.exe", tools)
        require(restored == source, "independent full-file round trip")
        atomic_write(args.output, packed)
        report.update(Executable=args.output.name, NativeExeBytes=len(source), PackedExeBytes=len(packed),
            PackingMode="cdpack", Packer="cdpack-1", Integrity="PASS", ResourcePayloadsMatch=True,
            CodeDataUnwindRelocationsMatch=True, SecurityFlagsMatch=True,
            LinkedSha256=hashlib.sha256(source).hexdigest(), Sha256=hashlib.sha256(packed).hexdigest(),
            LoaderSha256=hashlib.sha256((tools / "loader.exe").read_bytes()).hexdigest(), PackMs=elapsed)
        print(json.dumps(report))
        if args.report:
            atomic_write(args.report, (json.dumps(report, indent=2) + "\n").encode())
    else:
        restored = unpack_image(source, tools / "decode.exe", tools)
        if args.mode == "unpack":
            require(args.output is not None, "unpack needs an output path")
            atomic_write(args.output, restored)
        print(f"PASS native-file bytes={len(restored)} sha256={hashlib.sha256(restored).hexdigest()}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, struct.error, subprocess.CalledProcessError, lzma.LZMAError) as error:
        raise SystemExit(f"cdpack: {error}")
