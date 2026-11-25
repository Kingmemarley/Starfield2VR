#!/usr/bin/env python3
"""Ports the mod's places in the game from Starfield 1.16.236 to another version (fix188; docs/PORTING.md).

The mod's code gives every place in the game by its 1.16.236 rva, through GameCode (src/CreationEngine/memory/GameCode.h). This
reads what the code asks for, finds each place in the other version's Starfield.exe and writes the port table the other build uses
(src/CreationEngine/memory/GameCode_1_16_244.inc), with a report of what was found and how.

  python bake.py --old <1.16.236 Starfield.exe> --new <1.16.244 Starfield.exe> [--old-al <versionlib-1-16-236-0.bin>]
                 [--new-al <versionlib-1-16-244-0.bin>]
  python bake.py --old <1.16.236 Starfield.exe> --inventory      (only what the code asks for, checked against 1.16.236)
  python bake.py --old <exe> --new <the same exe> --selftest     (everything must be found where it is: tests this tool)

Needs Python 3.9+, capstone 5 and numpy (pip install capstone numpy). Reads the two exes only (never runs them).

How a place is found (each step checked; anything not certain is left out, and the feature that needs it stays off in that build):
- the function it is in (the exe's own function table, .pdata), found in the new exe: by its Address Library id when both libraries
  are given, else by its body: the same instructions with only their jump distances and data references differing (those are
  masked), the one candidate of the same length, the nearest to where the functions around it moved when there are several alike;
  - (fix190) code with no .pdata entry (small leaf functions) is cut into pseudo-functions at its int3 padding and matched the same
    way (Exe.code_without_unwind);
  - (fix190) a switch's jump tables inside a function are read as tables, not instructions: their entries and the instruction
    reading them compare as distances past the function's start; a table in the game's data read the same way (by its rva from
    the image's base) is masked as a data reference (decode);
  - (fix190) a function with twins, when the nearest rule gives nothing: by its place between the nearest functions matched for
    certain before and after it, when the new exe has as many functions between their matches, each alike; with twins between
    them too, its references (code matched for certain, imports, strings, type tables) must fit it and not those twins (bracket);
    when the nearest rule and this one both answer, they must agree;
- a place inside a function: the same distance into the new one when the two bodies are alike; when the body changed, a run of
  code the mod checks there is looked for inside the new function by its masked bytes (once only);
- a place in the game's data (a global, a type table): through an instruction that refers to it in an old function already
  matched: the same instruction in the new function, its target (all such references must agree; fix190), or the same distance
  past a place just before it: (fix190) when two or more different such places agree (and the data from the nearest through it is
  the same in both when the file has it), or from one place when the data from there through it is the same in both, pointers
  going to ports, and the place itself holds a string or a pointer to a port (map_data, same_data);
  - (fix190) a slot holding the only pointer in the old exe's data to a function matched for certain: the only word in the new
    exe's data pointing to its port, the same slot of its type table in both (also a check on the answers above: they must agree);
  - (fix192) a slot of a type table (its start included) is decided by the table's type alone (RTTI: the word before the table
    points to its locator, which names the type): the one table of that type in the new exe, as many slots, the same slot. The
    other rules go by what is near it, and the linker may reorder tables; when they disagree, the type wins (a WARNING) (map_by_type);
  - (fix192) where data is compared, a pointer to a type's locator must go to one of the same type, and a type table referred to
    from code must be of the same type (lambdas' names, numbered by the compiler, are not compared);
- a run the mod checks: its new bytes are what the new build compares (masked: they must be the old ones but for distances).
"""

import argparse
import bisect
import os
import re
import struct
import sys
from collections import defaultdict

try:
    import capstone
    from capstone import x86
except ImportError:  # pragma: no cover
    sys.exit("capstone is needed: pip install capstone")
try:
    import numpy as np
except ImportError:  # pragma: no cover
    np = None  # (fix190) (only slower: every function of a length is compared one by one)


# ---- The exe ----

class Exe:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not an exe")
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        if d[pe:pe + 4] != b"PE\0\0":
            raise ValueError(f"{path}: not a PE file")
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        optsz = struct.unpack_from("<H", d, pe + 20)[0]
        opt = pe + 24
        if struct.unpack_from("<H", d, opt)[0] != 0x20B:
            raise ValueError(f"{path}: not a 64-bit exe")
        self.image_base = struct.unpack_from("<Q", d, opt + 24)[0]
        self.image_size = struct.unpack_from("<I", d, opt + 56)[0]
        ndirs = struct.unpack_from("<I", d, opt + 108)[0]
        dirs = [struct.unpack_from("<II", d, opt + 112 + 8 * i) for i in range(ndirs)]
        self.sections = []
        s = opt + optsz
        for i in range(nsec):
            name = d[s:s + 8].rstrip(b"\0").decode("latin-1")
            vsize, va, rawsize, raw = struct.unpack_from("<IIII", d, s + 8)
            chars = struct.unpack_from("<I", d, s + 36)[0]
            self.sections.append((name, va, vsize, raw, rawsize, chars))
            s += 40
        text = [x for x in self.sections if x[0] == ".text"]
        if not text:
            raise ValueError(f"{path}: no .text")
        self.text = text[0]
        self.text_end = self.text[1] + min(self.text[2], self.text[4])
        # The function table (.pdata): (begin, end, chained)
        real = []
        if len(dirs) > 3 and dirs[3][0]:
            pva, psz = dirs[3]
            po = self.r2o(pva)
            for i in range(psz // 12):
                b, e, u = struct.unpack_from("<III", d, po + 12 * i)
                if b and e > b:
                    real.append((b, e))
        real.sort()
        # (fix190) Code with no entry there (small leaf functions need no unwind entry: no stack frame, no calls) is cut into
        # pseudo-functions, matched as the others are.
        pseudo = self.code_without_unwind(real)
        self.pseudo = {b for b, _ in pseudo}
        self.functions = sorted(real + pseudo)
        self.fbegins = [f[0] for f in self.functions]
        self.index = {b: i for i, (b, _) in enumerate(self.functions)}
        self.by_length = defaultdict(list)
        for b, e in self.functions:
            self.by_length[e - b].append(b)
        # (fix190) The import slots (rva -> "dll!name"): a call through one is compared by the name it imports.
        self.imports = {}
        if len(dirs) > 1 and dirs[1][0]:
            o = self.r2o(dirs[1][0])
            while o is not None and o + 20 <= len(d):
                lookup, _, _, dll_rva, slots = struct.unpack_from("<IIIII", d, o)
                if not slots:
                    break
                dll = self.cstring(dll_rva).lower()
                for k in range(0x10000):
                    v = self.read((lookup or slots) + 8 * k, 8)
                    v = struct.unpack("<Q", v)[0] if v else 0
                    if not v:
                        break
                    self.imports[slots + 8 * k] = f"{dll}!" + (f"#{v & 0xFFFF}" if v >> 63 else self.cstring((v & 0x7FFFFFFF) + 2))
                o += 20
        self.version = self.read_version()

    def cstring(self, rva):
        s = self.read(rva, 256) or b""
        return s.split(b"\0")[0].decode("latin-1")

    def code_without_unwind(self, real):
        """(fix190) The stretches of .text that no .pdata entry covers, cut into pseudo-functions (begin, end). The compiler starts every
        function on a 16-byte boundary and pads the space before it with int3 (0xCC), so a stretch is cut where a run of int3 ends on a
        16-byte boundary, and at its own start (right after a function, which may be a thunk the compiler put there); each piece ends
        before its trailing int3. The cuts depend only on the bytes and their place relative to 16-byte boundaries, which identical
        code keeps in both exes (functions start on them in both), so identical code is cut the same way in both. A cut inside a
        function (an int3 after a call that never returns, the next instruction on a boundary) is made the same way in both too. Only
        bytes no .pdata entry covers are used: a pseudo-function never overlaps a real one."""
        int3s = re.compile(rb"\xcc+")
        out = []
        cur = self.text[1]
        for b, e in real + [(self.text_end, self.text_end)]:
            b = min(b, self.text_end)
            if b > cur:
                data = self.read(cur, b - cur)
                starts = [cur] if data and data[0] != 0xCC else []
                for m in int3s.finditer(data or b""):
                    q = cur + m.end()
                    if q < b and q % 16 == 0:
                        starts.append(q)
                for k, s in enumerate(starts):
                    stop = starts[k + 1] if k + 1 < len(starts) else b
                    body = data[s - cur:stop - cur].rstrip(b"\xcc")
                    if body:
                        out.append((s, s + len(body)))
            cur = max(cur, e)
        return out

    def r2o(self, rva):
        for name, va, vsize, raw, rawsize, chars in self.sections:
            if va <= rva < va + max(vsize, rawsize):
                if rva - va >= rawsize:
                    return None
                return raw + (rva - va)
        return None

    def read(self, rva, n):
        o = self.r2o(rva)
        if o is None or o + n > len(self.data):
            return None
        return self.data[o:o + n]

    def section_of(self, rva):
        for sec in self.sections:
            if sec[1] <= rva < sec[1] + max(sec[2], sec[4]):
                return sec
        return None

    def is_code(self, rva):
        sec = self.section_of(rva)
        return sec is not None and (sec[5] & 0x20000000) != 0  # IMAGE_SCN_MEM_EXECUTE

    def is_data(self, rva):
        """(fix190) Whether rva is in a section of data (not code, nor .pdata, .rsrc, .reloc)."""
        sec = self.section_of(rva)
        return sec is not None and not (sec[5] & 0x20000000) and sec[0] in (".rdata", ".data", "_RDATA")

    def pointer(self, q):
        """(fix190) The rva an 8-byte value points to when it is an address in the image (as the file stores them), else None."""
        return q - self.image_base if self.image_base <= q < self.image_base + self.image_size else None

    def word_pointer(self, rva):
        """(fix192) The rva the 8-byte word at rva points to, or None."""
        q = self.read(rva, 8)
        return self.pointer(struct.unpack_from("<Q", q)[0]) if q else None

    def pointers_to(self, value, most=4):
        """(fix190) The places of the 8-byte words of the exe's data (its sections that are not code, nor .pdata, .rsrc, .reloc: the
        bytes in the file) that are this value."""
        if np is None:
            return []
        if not hasattr(self, "_words"):
            self._words = []
            for name, va, vsize, raw, rawsize, chars in self.sections:
                if chars & 0x20000000 or name in (".pdata", ".rsrc", ".reloc"):
                    continue
                start = (8 - va % 8) % 8
                # (never past the end of the file, for a section cut off there; the review)
                count = min(min(vsize, rawsize) - start, len(self.data) - (raw + start)) // 8
                if count > 0:
                    self._words.append((va + start, np.frombuffer(self.data, dtype=np.uint64, count=count, offset=raw + start)))
        out = []
        for va, words in self._words:
            out += [va + 8 * int(k) for k in np.nonzero(words == np.uint64(value))[0][:most]]
        return out[:most]

    # (fix192) The game's type tables (vtables) carry the compiler's type information (RTTI): the word before a table points to its
    # locator (signature 1, its own rva at +20, the offset of this table's part of the object at +4, the constructor displacement's
    # offset at +8, and at +12 the rva of the type's description, whose name, ".?AV...@@", is at +16). The linker may put the tables
    # in another order in another version, so a table is found by its type, never by its neighbours.

    def col_key(self, c):
        """(fix192) The type the locator at c is for: (its name, the offset of its part, the constructor displacement's offset), else
        None."""
        col = self.read(c, 24) if c is not None and self.is_data(c) else None
        if col is None:
            return None
        sig, off, cd, td, _, me = struct.unpack_from("<IIIIII", col)
        if sig != 1 or me != c:
            return None
        name = self.cstring(td + 16)
        return (name, off, cd) if name.startswith(".?A") else None

    def table_key(self, vt):
        """(fix192) The type of the type table starting at vt (the locator the word before it points to), else None."""
        return self.col_key(self.word_pointer(vt - 8))

    def table_place(self, rva):
        """(fix192) When the word at rva is a slot of a type table with type information: (the table's start, the slot number, its
        type). Each word from the table's start to rva must point to code."""
        if rva % 8:
            return None
        for k in range(4096):
            t = self.word_pointer(rva - 8 * k)
            if t is None or not self.is_code(t):
                return None
            key = self.table_key(rva - 8 * k)
            if key:
                return rva - 8 * k, k, key
        return None

    def slots(self, vt):
        """(fix192) How many words from vt on point to code (a type table's slots, up to the next table's locator)."""
        k = 0
        while k < 4096:
            t = self.word_pointer(vt + 8 * k)
            if t is None or not self.is_code(t):
                break
            k += 1
        return k

    def tables_of(self, key):
        """(fix192) The starts of the type tables of a type (name, offset, constructor displacement's offset): its locators (found
        once for all, from the data), then the words pointing to them."""
        if not hasattr(self, "_cols"):
            self._cols = defaultdict(list)
            for name, va, vsize, raw, rawsize, chars in self.sections:
                if name not in (".rdata", "_RDATA") or np is None:
                    continue
                count = min(min(vsize, rawsize), len(self.data) - raw) // 4
                w = np.frombuffer(self.data, dtype=np.uint32, count=count, offset=raw)
                at = np.arange(count - 5, dtype=np.int64) * 4 + va
                for i in np.nonzero((w[:-5] == 1) & (w[5:] == at))[0]:
                    k = self.col_key(va + 4 * int(i))
                    if k:
                        self._cols[k].append(va + 4 * int(i))
        out = []
        for c in self._cols.get(key, []):
            out += [p + 8 for p in self.pointers_to(self.image_base + c)]
        return sorted(set(out))

    def function_at(self, rva):
        i = bisect.bisect_right(self.fbegins, rva) - 1
        if i >= 0 and self.functions[i][0] <= rva < self.functions[i][1]:
            return self.functions[i]
        return None

    def read_version(self):
        sig = struct.pack("<I", 0xFEEF04BD)
        k = self.data.find(sig)
        while k >= 0:
            ms, ls = struct.unpack_from("<II", self.data, k + 8)
            if (ms >> 16) < 100:
                return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16}.{ls & 0xFFFF}"
            k = self.data.find(sig, k + 4)
        return "unknown"


# ---- Address Library (format 5: a header, then one u32 rva per id) ----

class AddressLibrary:
    def __init__(self, path):
        with open(path, "rb") as f:
            d = f.read()
        fmt = struct.unpack_from("<I", d, 0)[0]
        if fmt != 5:
            raise ValueError(f"{path}: Address Library format {fmt}, this reads 5")
        self.version = ".".join(str(x) for x in struct.unpack_from("<4I", d, 4))
        n = (len(d) - 0x60) // 4
        self.rvas = struct.unpack_from(f"<{n}I", d, 0x60)
        self.ids = {}
        for i, r in enumerate(self.rvas):
            if r and r not in self.ids:
                self.ids[r] = i

    def id_of(self, rva):
        return self.ids.get(rva)

    def rva_of(self, ident):
        return self.rvas[ident] if ident is not None and 0 <= ident < len(self.rvas) and self.rvas[ident] else None


# ---- Masked code ----

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
MD.detail = True


def decode(exe, rva, n, cache=None):
    """The instructions covering [rva, rva + n): [(offset, length, masked fields [(at, size, base)], rip target or None, branch target
    or None, table or None)], or None when the bytes do not decode as whole instructions up to n (or past it for the last). A masked
    field is zeroed when its base is None, else its 32-bit value becomes its distance past base (the run's start). (fix190) The
    table is the rva of a table the instruction reads by its rva from the image's base (below), so its port can be checked too.

    (fix190) A switch's jump tables, which the compiler puts after the function's code inside its .pdata range: the code reads them
    by their rva, [reg + index * 4 + table] with the image's base in reg (lea reg, [rip - X] to the base). A 32-bit displacement
    without rip that points past the instruction, inside its own function, is such a table. Its code ends where the first table
    starts; each table is one more entry (no rip or branch target) running to the next table or the end. A table of 4-byte entries
    holds the rvas of places in the function, and the reading instruction holds the table's rva: inside the run both become their
    distance past its start (so the same code at another place compares the same, and a change in where they point still shows);
    outside it they are zeroed, as data references are. A table of single bytes (which entry to take) is compared as it is."""
    key = (rva, n)
    if cache is not None and key in cache:
        return cache[key]
    data = exe.read(rva, n + 16)
    if data is None:
        return None
    out = []
    pos = 0
    stop = n
    tables = {}  # (fix190) a jump table's offset in the run -> the size of its entries
    for ins in MD.disasm(data, rva):
        if pos >= stop:
            break
        if ins.address - rva != pos:
            out = None
            break
        masks = []
        rip = None
        branch = None
        read = None  # (fix190) a table read by its rva
        # (fix191) A rip-relative operand's distance is always 4 bytes. capstone 5 gives every instruction with an operand-size 66
        # prefix (written or implied by VEX: mov/cmp word, movdqa, pand, por, vpor [rip+...] ...) a disp_size of 2, which left their
        # distance unmasked: whole functions the same but for distances looked changed (GloveHighlight's scanner, HandGrab's collision
        # update). The 4 bytes at disp_offset are taken when they are the operand's displacement (the same for a table's rva below).
        def disp32(o):
            off = ins.disp_offset
            return 0 < off and off + 4 <= ins.size and struct.unpack_from("<i", ins.bytes, off)[0] == o.mem.disp

        for o in ins.operands:
            if o.type == x86.X86_OP_MEM and o.mem.base == x86.X86_REG_RIP:
                if disp32(o):
                    masks.append((pos + ins.disp_offset, 4, None))
                    rip = ins.address + ins.size + o.mem.disp
                break
        if rip is None:
            for o in ins.operands:
                if o.type != x86.X86_OP_MEM:
                    continue
                if o.mem.base == x86.X86_REG_RIP or not (ins.disp_size == 4 or disp32(o)):
                    break
                table = o.mem.disp & 0xFFFFFFFF
                if ins.address < table < exe.text_end:
                    f = exe.function_at(ins.address)
                    if f and table < f[1]:
                        inside = rva <= table < rva + n
                        masks.append((pos + ins.disp_offset, 4, rva if inside else None))
                        read = table
                        if inside:
                            tables[table - rva] = max(tables.get(table - rva, 0), o.size)
                            stop = min(stop, table - rva)
                elif exe.text_end <= table < exe.image_size and exe.is_data(table):
                    # (fix190) A table in the game's data read the same way, by its rva from the image's base ([reg + index * 8 +
                    # table]): a data reference, masked as one. (Its rva is past the code, tens of MB: never an offset into an object.)
                    masks.append((pos + ins.disp_offset, 4, None))
                    read = table
                break
        if ins.group(capstone.CS_GRP_BRANCH_RELATIVE) and ins.imm_size in (1, 4):
            branch = ins.operands[0].imm if ins.operands else None
            # (only a branch leaving the function or run is masked: one inside it keeps its distance when the code is the same, so a
            # change in its own flow still shows; the review)
            if branch is None or not (rva <= branch < rva + n):
                masks.append((pos + ins.imm_offset, ins.imm_size, None))
        out.append((pos, ins.size, masks, rip, branch, read))
        pos += ins.size
    if out is not None and (pos < stop or (tables and pos != stop)):
        out = None  # (fix190) (with tables: the code must end exactly where the first starts)
    if out is not None and tables:
        offsets = sorted(tables)
        f = exe.function_at(rva + offsets[0])
        for k, t in enumerate(offsets):
            end = offsets[k + 1] if k + 1 < len(offsets) else n
            masks = []
            if tables[t] == 4 and f:
                for at in range(t, end - 3, 4):
                    v = struct.unpack_from("<I", data, at)[0]
                    if f[0] <= v < f[1]:
                        masks.append((at, 4, rva if rva <= v < rva + n else None))
            out.append((t, end - t, masks, None, None, None))
    if cache is not None:
        cache[key] = out
    return out


LAMBDA = re.compile(r"<lambda_[0-9a-f]+>")
ANON = re.compile(r"\?A0x[0-9a-f]+@")


def without_numbers(name):
    """(fix192) A type's name without the numbers the compiler gives its lambdas and anonymous namespaces (?A0x...@): they change
    from build to build (most of the anonymous namespaces' do between 1.16.236 and 1.16.244; the review)."""
    return ANON.sub("?A0x@", LAMBDA.sub("<lambda>", name))


def same_type(a, b):
    """(fix192) Whether two types (Exe.col_key) are the same: True or False; None when only the numbers the compiler gives its
    lambdas and anonymous namespaces differ, or the old one is not known."""
    if a is None:
        return None
    if a == b:
        return True
    if b is not None and a[1:] == b[1:] and without_numbers(a[0]) == without_numbers(b[0]):
        return None
    return False


def masked_bytes(exe, rva, n, cache=None):
    """The bytes of [rva, rva + n) with jump distances and data references zeroed (a jump table's places: their distance past rva),
    and the decoded instructions (None if they do not decode as whole instructions)."""
    if cache is not None and ("m", rva, n) in cache:
        return cache[("m", rva, n)]
    ins = decode(exe, rva, n, cache)
    data = exe.read(rva, n)
    if ins is None or data is None:
        return None, None
    b = bytearray(data)
    for _, _, masks, _, _, _ in ins:
        for at, size, base in masks:
            if base is not None and at + 4 <= n:
                struct.pack_into("<I", b, at, (struct.unpack_from("<I", data, at)[0] - base) & 0xFFFFFFFF)
                continue
            for k in range(at, min(at + size, n)):
                b[k] = 0
    out = (bytes(b), ins)
    if cache is not None:
        cache[("m", rva, n)] = out
    return out


# ---- What the code asks for ----

CALLS = re.compile(r"\b(GameCode::At|GameCode::Rva|GameCode::Code|GameCode::Same|GameCode::Known|At|SameCode)\s*\(")
CONST = re.compile(r"constexpr\s+(?:std::)?(?:uintptr_t|uint32_t|uint64_t|size_t|int|int32_t|intptr_t|ptrdiff_t|uint16_t)\s+(k\w+)\s*=\s*([^;{]+);")
ARRAY = re.compile(r"constexpr\s+(?:std::)?(?:uint8_t|unsigned\s+char)\s+(k\w+)\s*\[\s*\w*\s*\]\s*=\s*\{([^}]*)\}")
# (fix190) The game's base added to a place by hand: the base by its usual names, as MemoryScan::mod or GetModuleHandle(...), plus a
# constant or a number; or anything plus a constant named ...Rva...
BASE_ADD = re.compile(r"(?:\b(?:g_base|base|g_image|g_module|g_exe|image_base|module_base|game_base|exe_base)\b|\bBase\(\)|MemoryScan::mod\s*\)?"
                      r"|GetModuleHandle\w*\s*\([^()]*\)\s*\)?)\s*\+\s*(k[A-Z0-9]\w*|0x[0-9a-fA-F]+)")
RVA_ADD = re.compile(r"\+\s*(k\w*Rva\w*)\b")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    return text


def args_of(text, start):
    """The top-level arguments of the call whose '(' is at start, and where it ends."""
    depth = 0
    out = []
    cur = ""
    i = start
    while i < len(text):
        c = text[i]
        if c in "([{":
            depth += 1
            if depth > 1:
                cur += c
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                out.append(cur.strip())
                return out, i
            cur += c
        elif c == "," and depth == 1:
            out.append(cur.strip())
            cur = ""
        else:
            if depth >= 1:
                cur += c
        i += 1
    return None, i


class Evaluator:
    def __init__(self, consts, arrays):
        self.consts = consts
        self.arrays = arrays

    def value(self, expr, depth=0):
        if depth > 20:
            return None
        e = expr.strip()
        e = re.sub(r"static_cast<[^>]*>", "", e)
        e = re.sub(r"sizeof\s*\(\s*(k\w+)\s*\)", lambda m: str(len(self.arrays[m.group(1)])) if m.group(1) in self.arrays else "None", e)
        e = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]*\b", r"\1", e)
        names = set(re.findall(r"\b[A-Za-z_]\w*\b", e)) - {"None"}
        env = {}
        for name in names:
            if name not in self.consts:
                return None
            v = self.value(self.consts[name], depth + 1)
            if v is None:
                return None
            env[name] = v
        if not re.fullmatch(r"[\w\s+\-*()]*", e) or "None" in e:
            return None
        try:
            return int(eval(e, {"__builtins__": {}}, env))  # (only names, numbers and + - * ( ) are left)
        except Exception:
            return None


def inventory(src):
    """Every place the code asks GameCode for: {rva: {"files": set, "runs": {n: (bytes, where)}}} and the calls not understood."""
    places = defaultdict(lambda: {"files": set(), "runs": {}})
    unclear = []
    skipped = []  # calls with a variable (inside a wrapper: fine; anywhere else a place the table would not have)
    unconverted = []  # (fix190) places files not converted yet add the game's base to by hand
    for root, _, files in os.walk(src):
        for name in files:
            if not name.endswith((".cpp", ".h", ".hpp")):
                continue
            path = os.path.join(root, name)
            with open(path, encoding="utf-8", errors="replace") as f:
                raw = f.read()
            if name.startswith("GameCode"):
                continue
            text = strip_comments(raw)
            consts = {m.group(1): m.group(2) for m in CONST.finditer(text)}
            arrays = {}
            for m in ARRAY.finditer(text):
                try:
                    arrays[m.group(1)] = bytes(int(x, 0) for x in m.group(2).replace("\n", " ").split(",") if x.strip())
                except ValueError:
                    pass
            ev = Evaluator(consts, arrays)
            rel = os.path.relpath(path, src)
            # (a converted file must not add the game's base to a place itself: in a ported build that would be 1.16.236's address; the
            # review)
            # (a constant of 0x10000 or more: a place, not a field's offset)
            # (fix190) Also in files not converted yet (listed as such), and the other ways the code writes it: the base as
            # MemoryScan::mod or GetModuleHandle(...), any name, a constant named ...Rva..., or a number. (These are patterns: other
            # forms, such as base + k.table_rva, are missed; the review.)
            hand = {}
            for pattern in (BASE_ADD, RVA_ADD):
                for m in pattern.finditer(text):
                    v = ev.value(m.group(1)) if m.group(1).startswith("k") else int(m.group(1), 16)
                    if v is not None and v >= 0x10000:
                        hand.setdefault(m.start(1), f"{rel}:{text.count(chr(10), 0, m.start()) + 1}: '{m.group(0).strip()}'")
            if "GameCode" not in raw:
                unconverted.extend(f"{h}: the game's base added to a place by hand (not converted yet)" for _, h in sorted(hand.items()))
                continue
            unclear.extend(f"{h}: the game's base added to a place by hand (use At/GameCode)" for _, h in sorted(hand.items()))
            for m in CALLS.finditer(text):
                fn = m.group(1)
                args, end = args_of(text, m.end() - 1)
                line = text.count("\n", 0, m.start()) + 1
                if args is None or not args or not args[0]:
                    continue
                # (the helpers' own definitions and other functions' parameters: not places)
                if re.match(r"(?:const\s+)?(?:uintptr_t|uint32_t|size_t|std::)", args[0]):
                    continue
                if re.fullmatch(r"(?!k[A-Z0-9])[a-z_]\w*", args[0]):
                    skipped.append(f"{rel}:{line}: {fn}({args[0]})")
                    continue
                if fn == "GameCode::Known":
                    inner = args[0].strip("{} ")
                    for part in [p for p in inner.split(",") if p.strip()]:
                        v = ev.value(part)
                        if v is None:
                            unclear.append(f"{rel}:{line}: {fn}({part.strip()}) not understood")
                        else:
                            places[v]["files"].add(rel)
                    continue
                rva = ev.value(args[0])
                if rva is None:
                    unclear.append(f"{rel}:{line}: {fn}({', '.join(args)}) not understood")
                    continue
                places[rva]["files"].add(rel)
                if fn in ("SameCode", "GameCode::Code", "GameCode::Same"):
                    arr = args[1] if len(args) > 1 else ""
                    if arr not in arrays:
                        unclear.append(f"{rel}:{line}: {fn}(..., {arr}, ...): its bytes not found")
                        continue
                    n = ev.value(args[2]) if len(args) > 2 else len(arrays[arr])
                    if n is None or n <= 0 or n > len(arrays[arr]):
                        unclear.append(f"{rel}:{line}: {fn}(..., {arr}, {args[2] if len(args) > 2 else ''}): its length not understood")
                        continue
                    places[rva]["runs"][n] = (arrays[arr][:n], f"{rel}:{line} {arr}")
    return places, unclear, skipped, unconverted


# ---- Finding places ----

class Porter:
    def __init__(self, old, new, old_al=None, new_al=None):
        self.old = old
        self.new = new
        self.old_al = old_al
        self.new_al = new_al
        self.cache_old = {}
        self.cache_new = {}
        self.fmap = {}       # old function begin -> (new begin, how) or (None, why)
        self.anchors = []    # (old begin, new begin) of functions matched, for guesses
        self.twins = {}
        self.rows = {}       # (fix190) (exe, length) -> the bytes of all its functions of that length, for a quick first look
        self.sure = {}       # (fix190) old begin -> new begin of a function matched for certain, or None (the bracket rule's ends)
        self.back = {}       # (fix190) the same the other way: new begin -> old begin, or None

    def old_twins(self, ob, oe):
        """The old exe's functions alike to this one (masked), itself included: a function with twins is matched only by place."""
        if ob not in self.twins:
            self.twins[ob] = self.alike_among(self.old, ob, oe - ob)
        return self.twins[ob]

    def alike_among(self, exe_b, a, n, most=None, ref=None):
        """(fix190) The functions of exe_b of length n alike to [a, a + n) of ref (the old exe unless given), at most `most` of them.
        A quick first look (numpy) over all their bytes at once, ref's masked fields left out; then each one left is compared in full."""
        ref = ref or self.old
        begins = exe_b.by_length.get(n, [])
        want, ins = masked_bytes(ref, a, n, self.cache_old if ref is self.old else self.cache_new)
        if want is None or not begins:
            return []
        if len(begins) > 32 and np is not None:
            key = (exe_b is self.new, n)
            if key not in self.rows:
                self.rows[key] = np.frombuffer(b"".join(exe_b.read(b, n) or bytes(n) for b in begins), dtype=np.uint8).reshape(len(begins), n)
            keep = np.ones(n, dtype=bool)
            for _, _, masks, _, _, _ in ins:
                for at, size, _ in masks:
                    keep[at:at + size] = False
            w = np.frombuffer(want, dtype=np.uint8)
            begins = [begins[k] for k in np.nonzero((self.rows[key][:, keep] == w[keep]).all(axis=1))[0]]
        out = []
        for b in begins:
            if self.alike(ref, a, exe_b, b, n):
                out.append(b)
                if most and len(out) >= most:
                    break
        return out

    def predict(self, rva):
        if not self.anchors:
            return rva
        self.anchors.sort()
        i = bisect.bisect_left(self.anchors, (rva, -1))
        near = [self.anchors[j] for j in (i - 1, i) if 0 <= j < len(self.anchors)]
        best = min(near, key=lambda a: abs(a[0] - rva))
        return rva + (best[1] - best[0])

    def alike(self, exe_a, a, exe_b, b, n):
        """Whether [a, a + n) of exe_a and [b, b + n) of exe_b are the same code but for their jump distances and data references."""
        cache_a = self.cache_old if exe_a is self.old else self.cache_new
        cache_b = self.cache_old if exe_b is self.old else self.cache_new
        want, ins = masked_bytes(exe_a, a, n, cache_a)
        if want is None:
            return False
        # (a quick look first: the other's bytes with this one's masked fields zeroed must already be the same)
        raw = exe_b.read(b, n)
        if raw is None:
            return False
        quick = bytearray(raw)
        for _, _, masks, _, _, _ in ins:
            for at, size, base in masks:
                if base is not None and at + 4 <= n:
                    # (fix190) (a jump table's place: its distance past the other's start)
                    struct.pack_into("<I", quick, at, (struct.unpack_from("<I", raw, at)[0] - (b + base - a)) & 0xFFFFFFFF)
                    continue
                quick[at:at + size] = bytes(min(size, len(quick) - at))
        if bytes(quick) != want:
            return False
        got, _ = masked_bytes(exe_b, b, n, cache_b)
        return want == got

    def same_body(self, ob, oe, nb, ne):
        return oe - ob == ne - nb and self.alike(self.old, ob, self.new, nb, oe - ob)

    def map_function(self, ob, oe, allow_nearest=True):
        if ob in self.fmap:
            return self.fmap[ob]
        result = (None, "not found")
        if self.old_al and self.new_al:
            ident = self.old_al.id_of(ob)
            nb = self.new_al.rva_of(ident) if ident is not None else None
            if nb is not None:
                f = self.new.function_at(nb)
                if f and f[0] == nb:
                    result = (nb, f"Address Library id {ident}" + ("" if self.same_body(ob, oe, f[0], f[1]) else " (its body changed)"))
                else:
                    result = (None, f"Address Library id {ident} gives {nb:#x}, not a function start")
        if result[0] is None:
            alike = self.alike_among(self.new, ob, oe - ob)
            twins = self.old_twins(ob, oe)
            if len(alike) == 1 and len(twins) == 1:
                result = (alike[0], "the one function alike (one of its kind in both)")
            elif alike and len(alike) == len(twins):
                # (as many alike in both: the one nearest to where the functions around it moved, when that is clear; the review)
                if not allow_nearest:
                    return (None, "deferred")
                guess = self.predict(ob)
                alike.sort(key=lambda b: abs(b - guess))
                gap0 = abs(alike[0] - guess)
                gap1 = abs(alike[1] - guess) if len(alike) > 1 else None
                if gap0 < 0x2000 and (gap1 is None or gap1 > 8 * max(gap0, 0x100)):
                    result = (alike[0], f"the nearest of {len(alike)} functions alike (as many in both) to where its neighbours moved")
                    # (fix190) When the bracket rule gives another one, neither is certain; nor when its references go elsewhere than
                    # their ports (the review).
                    other = self.bracket(ob, f"{len(alike)} functions alike")
                    against = self.fits(ob, oe - ob, alike[0])[1]
                    if other[0] is not None and other[0] != alike[0]:
                        result = (None, f"the nearest of {len(alike)} functions alike is {alike[0]:#x}, but by its place it is "
                                        f"{other[0]:#x} ({other[1]})")
                    elif against:
                        result = (None, f"the nearest of {len(alike)} functions alike is {alike[0]:#x}, but {against} of its "
                                        f"references there go elsewhere than their ports")
                else:
                    result = self.bracket(ob, f"{len(alike)} functions alike, none clearly the one")
            elif alike:
                if not allow_nearest:
                    return (None, "deferred")
                result = self.bracket(ob, f"{len(alike)} function(s) alike in the new exe but {len(twins)} in the old: not certain which")
        self.fmap[ob] = result
        if result[0] is not None and "changed" not in result[1]:
            self.anchors.append((ob, result[0]))
        return result

    def sure_match(self, ob, oe):
        """(fix190) The new begin of an old function matched for certain (one of its kind in both, or by its Address Library id with
        its body the same), else None: the ends of the bracket rule. Kept apart from the functions matched and their places (fmap,
        anchors), so looking at neighbours changes no other answer."""
        if ob in self.sure:
            return self.sure[ob]
        nb = None
        known = self.fmap.get(ob)
        if known is not None:
            if known[0] is not None and (known[1].startswith("the one function alike")
                                         or (known[1].startswith("Address Library") and "changed" not in known[1])):
                nb = known[0]
        else:
            # (as map_function: a function the Address Library places counts only with its body the same; without one, by its body)
            ident = self.old_al.id_of(ob) if self.old_al and self.new_al else None
            at = self.new_al.rva_of(ident) if ident is not None else None
            f = self.new.function_at(at) if at is not None else None
            if f and f[0] == at:
                nb = at if self.same_body(ob, oe, f[0], f[1]) else None
            else:
                alike = self.alike_among(self.new, ob, oe - ob, most=2)
                if len(alike) == 1 and len(self.alike_among(self.old, ob, oe - ob, most=2)) == 1:
                    nb = alike[0]
        self.sure[ob] = nb
        return nb

    REACH = 512  # (fix190) how many functions the bracket rule looks at on each side for its ends (runs of a game's settings' makers)

    def bracket(self, ob, why):
        """(fix190) A function with twins, by its place between the functions around it: the nearest old functions before and after it
        matched for certain (sure_match) are its bracket. When the new exe has as many functions between their two matches, each of
        the same length as the old one at the same place and alike (masked) or the same bytes, this one is the one at its place
        between them. Twins elsewhere do not matter: the whole stretch between the two ends must be the same code in both."""
        fo, fn = self.old.functions, self.new.functions
        i = self.old.index.get(ob)
        if i is None:
            return (None, why)
        ends = []
        for step in (-1, 1):
            j = i + step
            while 0 <= j < len(fo) and abs(j - i) <= self.REACH:
                nb = self.sure_match(*fo[j])
                if nb is not None:
                    ends.append((j, self.new.index[nb]))
                    break
                j += step
            else:
                return (None, f"{why}; no function matched for certain within {self.REACH} functions {'before' if step < 0 else 'after'} it")
        (ja, ka), (jb, kb) = ends
        if kb - ka != jb - ja:
            return (None, f"{why}; between the functions matched for certain around it ({fo[ja][0]:#x}, {fo[jb][0]:#x}) the old exe has "
                          f"{jb - ja - 1} functions, the new {kb - ka - 1}")
        n = fo[i][1] - ob
        inside = []  # its twins between the two ends
        for t in range(1, jb - ja):
            (b0, e0), (b1, e1) = fo[ja + t], fn[ka + t]
            if e0 - b0 != e1 - b1 or not (self.alike(self.old, b0, self.new, b1, e0 - b0) or self.old.read(b0, e0 - b0) == self.new.read(b1, e1 - b1)):
                return (None, f"{why}; between the functions matched for certain around it ({fo[ja][0]:#x}, {fo[jb][0]:#x}) "
                              f"function {t} differs ({b0:#x}, {b1:#x})")
            if b0 != ob and e0 - b0 == n and self.alike(self.old, ob, self.old, b0, n):
                inside.append(b0)
        nb = fn[ka + i - ja][0]
        place = (f"by its place: number {i - ja} of the {jb - ja - 1} functions between {fo[ja][0]:#x} and {fo[jb][0]:#x} (each matched "
                 f"for certain), all alike and as many in both")
        # (Its references must fit. With twins between the ends, one function taken out and one put in between them would keep the
        # count and shift the ones in between onto their twins: then its references (code matched for certain, strings, a type
        # table's functions) must fit it and must not fit any of those twins, or it is left out.)
        yes, no = self.fits(ob, n, nb)
        if no:
            return (None, f"{why}; {place}, but {no} of its references there go elsewhere than their ports")
        if inside:
            if not yes:
                return (None, f"{why}; {place}, but it has {len(inside)} twin(s) between them and none of its references can be checked")
            for g in inside:
                if not self.fits(g, n, nb)[1]:
                    return (None, f"{why}; {place}, but its references do not tell it from its twin {g:#x} between them")
            place += f"; its references fit it ({yes}) and not its {len(inside)} twin(s) between them"
        elif yes:
            place += f"; its references fit it ({yes})"
        return (nb, f"{why} by its body; {place}")

    def fits(self, ob, n, nb, pairs=(), depth=0):
        """(fix190) What the old function [ob, ob + n)'s references say of nb being its port: (how many fit, how many do not). Each
        reference leaving it (a call or jump, a data reference) is compared with the same instruction's in the new function."""
        pairs = pairs + ((ob, nb),)
        ins_o = decode(self.old, ob, n, self.cache_old)
        ins_n = decode(self.new, nb, n, self.cache_new)
        yes = no = 0
        for io, inew in zip(ins_o or [], ins_n or []):
            for t_old, t_new in ((io[3], inew[3]), (io[4], inew[4])):
                if t_old is None or t_new is None or ob <= t_old < ob + n:
                    continue
                v = self.target_fits(t_old, t_new, pairs, depth)
                yes += v is True
                no += v is False
        return yes, no

    def target_fits(self, t_old, t_new, pairs, depth=0):
        """(fix190) Whether a reference's new target is the port of its old one. True when sure: code matched for certain; the same
        import; a string the same; a type table whose functions are such ports. False when surely not: code matched for certain
        elsewhere (either way); another import or string. None when it cannot be told: code with twins (whose own references are
        looked at, one step down), data with no bytes in the file, plain numbers (the data around a global may change). `pairs` are
        the (old, new) functions being tried: a reference to one of them must go to the other (which says nothing more: it is what
        is being tried)."""
        for po, pn in pairs:
            if t_old == po or t_new == pn:
                return None if (t_old == po and t_new == pn) else False
        if t_old in self.old.imports:
            return self.new.imports.get(t_new) == self.old.imports[t_old]
        if self.old.text[1] <= t_old < self.old.text_end:
            f = self.old.function_at(t_old)
            g = self.new.function_at(t_new)
            if f is None or f[0] != t_old or g is None or g[0] != t_new:
                return None
            x = self.sure_match(*f)
            if x is not None:
                return t_new == x
            y = self.sure_preimage(*g)
            if y is not None:
                return y == t_old  # (the new target is the port of that old function for certain)
            if depth >= 2 or f[1] - f[0] != g[1] - g[0] or not self.alike(self.old, f[0], self.new, g[0], f[1] - f[0]):
                return None
            yes, no = self.fits(f[0], f[1] - f[0], g[0], pairs, depth + 1)
            return False if no else (True if yes else None)
        if depth:
            return None  # (one step into data only)
        a = self.old.read(t_old, 256) or self.old.read(t_old, 32)
        b = self.new.read(t_new, 256) or self.new.read(t_new, 32)
        if a is None or b is None or len(a) != len(b):
            return None
        end = a.find(b"\0")
        if end >= 4 and all(32 <= c < 127 for c in a[:end]):
            return a[:end + 1] == b[:end + 1]  # a string, to its end
        if t_old % 8 or t_new % 8:
            return None
        verdicts = []
        # (fix192) A type table: of the same type (its RTTI), then its first functions.
        ka = self.old.table_key(t_old)
        if ka:
            v = same_type(ka, self.new.table_key(t_new))
            if v is False:
                return False
            verdicts.append(v)
        for k in range(0, 32, 8):  # (a type table's first functions: up to four pointers, while both are pointers to code)
            pa = self.old.pointer(struct.unpack_from("<Q", a, k)[0])
            pb = self.new.pointer(struct.unpack_from("<Q", b, k)[0])
            # (fix190) (a pointer to data ends it: the next table's type pointer, whose own slots the linker may have put elsewhere;
            # the review)
            if pa is None or pb is None or not self.old.is_code(pa) or not self.new.is_code(pb):
                break
            verdicts.append(self.target_fits(pa, pb, pairs, depth + 1))
        return False if False in verdicts else (True if True in verdicts else None)

    def pointer_fits(self, pa, pb):
        """(fix192) Whether a pointer in the game's data goes to the port of its old target: to a type's locator, one of the same
        type (its RTTI); else as target_fits (code)."""
        ka = self.old.col_key(pa)
        if ka:
            return same_type(ka, self.new.col_key(pb))
        return self.target_fits(pa, pb, (), 1)

    def sure_preimage(self, nb, ne):
        """(fix190) The old function a new one is the port of for certain (one of its kind in both, by its body), else None."""
        if nb not in self.back:
            olds = self.alike_among(self.old, nb, ne - nb, most=2, ref=self.new)
            self.back[nb] = olds[0] if len(olds) == 1 and len(self.alike_among(self.new, nb, ne - nb, most=2, ref=self.new)) == 1 else None
        return self.back[nb]

    def find_run_in(self, run_old, nb, ne):
        """A checked run (masked) inside the new function [nb, ne): its place if found once."""
        rva, n = run_old
        want, _ = masked_bytes(self.old, rva, n, self.cache_old)
        if want is None:
            return None
        hits = []
        data = self.new.read(nb, ne - nb)
        if data is None:
            return None
        first = want[0]
        for k in range(0, len(data) - n + 1):
            if data[k] != first and want[0] != 0:
                continue
            got, _ = masked_bytes(self.new, nb + k, n, self.cache_new)
            if got == want:
                hits.append(nb + k)
                if len(hits) > 1:
                    return None
        return hits[0] if hits else None

    def map_code(self, rva, runs):
        f = self.old.function_at(rva)
        if f is None:
            return None, "not inside a function of the old exe"
        nb, how = self.map_function(*f)
        if f[0] in self.old.pseudo:
            how = f"(no unwind entry: code {f[0]:#x}-{f[1]:#x} between int3 paddings) {how}"  # (fix190)
        if nb is None:
            return None, f"its function {f[0]:#x}: {how}"
        if "changed" not in how:
            return nb + (rva - f[0]), f"{how}, {rva - f[0]:#x} in"
        ne = (self.new.function_at(nb) or (nb, nb))[1]
        # (the body changed: only a checked run found again inside it)
        if runs:
            longest = max(runs)
            at = self.find_run_in((rva, longest), nb, ne)
            if at is not None:
                return at, f"{how}; its checked run found again {at - nb:#x} in"
        return None, f"{how}; its body changed and no checked run of it found again"

    def map_data(self, rva, functions_old):
        """A place in the game's data: by its Address Library id when both are given; else through an instruction referring to it in a
        function matched (one referring to it exactly is enough; one referring to a place just before it only when two such agree,
        since the data around it may have moved apart; the review). Gives (place or None, how, whether that is from references to it
        exactly: then nothing else is looked for).

        (fix190) Every reference to it exactly must agree. References just before it count only by the places they refer to: two or
        more different places must agree, and the data from the nearest one through it must be the same in both when the file has
        those bytes (two references to one place cannot show that nothing between it and the place moved; the review). With one
        place only, or when further ones disagree, the data from the nearest through it must be the same, and there must be
        something at the place itself to check (same_data)."""
        if self.old_al and self.new_al:
            ident = self.old_al.id_of(rva)
            nr = self.new_al.rva_of(ident) if ident is not None else None
            if nr is not None:
                return nr, f"Address Library id {ident}", True
        refs = []
        for ob, oe in functions_old:
            ins = decode(self.old, ob, oe - ob, self.cache_old)
            for pos, size, masks, rip, branch, _ in ins or []:
                if rip is not None and 0 <= rva - rip < 0x400:
                    refs.append((rva - rip, ob, oe, pos))
        if not refs:
            return None, "no instruction refers to it in the functions looked at", False
        refs.sort()
        found = []
        exact = []
        for extra, ob, oe, pos in refs:
            nb, how = self.map_function(ob, oe)
            if nb is None or "changed" in how:
                continue
            new_rip = None
            for p, size, masks, rip, branch, _ in decode(self.new, nb, oe - ob, self.cache_new) or []:
                if p == pos:
                    new_rip = rip
                    break
            if new_rip is None:
                continue
            if extra == 0:
                exact.append((new_rip, f"{ob:#x}+{pos:#x}", how))
            else:
                found.append((new_rip + extra, f"{ob:#x}+{pos:#x}", rva - extra, new_rip))
        if exact:
            answers = sorted({v for v, _, _ in exact})
            if len(answers) > 1:
                return None, (f"{len(exact)} references to it exactly disagree ({', '.join(f'{w} -> {v:#x}' for v, w, _ in exact[:4])}): "
                              f"not certain"), True
            more = f"; {len(exact) - 1} more reference(s) to it agree" if len(exact) > 1 else ""
            return answers[0], f"referred to at {exact[0][1]} ({exact[0][2]}){more}", True
        if not found:
            return None, f"{len(refs)} instruction(s) refer to it or just before it, none in a function matched as it was", False
        values = defaultdict(list)
        for value, where, x_old, x_new in found:
            values[value].append((where, x_old, x_new))
        x_near = max(x for _, _, x, _ in found)
        near = [f for f in found if f[2] == x_near]
        value, where, _, x_new = near[0]
        if len(values) == 1:
            places = sorted({x for _, x, _ in values[value]})
            if len(places) >= 2:
                same, words = self.same_data(x_near, x_new, rva, value, need_own=False)
                if same is not False:
                    return value, (f"{len(places)} places just before it referred to agree ({', '.join(f'{x:#x}' for x in places[-3:])}); "
                                   f"{words}"), False
                return None, (f"{len(places)} places just before it referred to agree, but from the nearest ({x_near:#x}): {words}: "
                              f"check by hand"), False
        if len({f[0] for f in near}) == 1:
            same, words = self.same_data(x_near, x_new, rva, value)
            if same:
                return value, (f"referred to only just before it: {rva - x_near:#x} past {x_near:#x}, the nearest place referred to "
                               f"({len(near)} reference(s), {where}); {words}"), False
            return None, (f"referred to only just before it ({len(found)} reference(s), {len(values)} answer(s)); the nearest "
                          f"({x_near:#x}, {where}): {words}: check by hand"), False
        return None, f"referred to only just before it ({len(found)} reference(s), {len(values)} answer(s)): check by hand", False

    def map_by_pointer(self, rva):
        """(fix190) A place that holds a pointer to a function (a type table's slot): when it is the only 8-byte word of the old exe's
        data pointing to that function, the function is matched for certain, and the new exe's data has one word only pointing to its
        port, that word. Gives (place or None, how)."""
        q = self.old.read(rva, 8)
        t = self.old.pointer(struct.unpack_from("<Q", q)[0]) if q and rva % 8 == 0 else None
        f = self.old.function_at(t) if t is not None and self.old.text[1] <= t < self.old.text_end else None
        if f is None or f[0] != t:
            return None, "it holds no pointer to a function"
        x = self.sure_match(*f)
        if x is None:
            return None, f"it holds a pointer to {t:#x}, not matched for certain"
        olds = self.pointers_to(self.old, self.old.image_base + t)
        news = self.pointers_to(self.new, self.new.image_base + x)
        if olds != [rva] or len(news) != 1:
            return None, f"{len(olds)} word(s) of the old exe's data point to {t:#x}, {len(news)} of the new's to its port {x:#x}"
        # (fix190) The same slot of its type table in both, not only the slot holding that function (the review).
        k_old, k_new = self.table_slot(self.old, rva), self.table_slot(self.new, news[0])
        if k_old is None or k_old != k_new:
            return None, (f"the only word of the new exe's data pointing to {x:#x}, the port of {t:#x}, is {news[0]:#x}, but it is "
                          f"slot {k_new} of its type table there and slot {k_old} in the old")
        return news[0], (f"the only word of the new exe's data pointing to {x:#x}, the port of {t:#x} (matched for certain), which "
                         f"only it points to in the old; slot {k_old} of its type table in both")

    def table_slot(self, exe, rva):
        """(fix190) Which slot of its type table the word at rva is: how many pointers to code come before it, back to the table's
        start, where the word before the first slot points to the type's description (its RTTI locator: signature 1, pointing to
        itself). None when it does not look like that."""
        k = 0
        while k < 4096:
            q = exe.read(rva - 8 * (k + 1), 8)
            t = exe.pointer(struct.unpack_from("<Q", q)[0]) if q else None
            if t is None:
                return None
            if exe.is_code(t):
                k += 1
                continue
            col = exe.read(t, 24)
            if not exe.is_data(t) or col is None:
                return None
            sig, _, _, _, _, me = struct.unpack_from("<IIIIII", col)
            return k if sig == 1 and me == t else None
        return None

    def pointers_to(self, exe, value, most=4):
        """(fix190) The places of the 8-byte words of an exe's data that are this value (Exe.pointers_to)."""
        return exe.pointers_to(value, most)

    def map_by_type(self, rva):
        """(fix192) A slot of a type table (its start is slot 0), found by the table's type (RTTI, Exe.table_place): the one table of
        the same type (name, offset of its part, constructor displacement's offset) in the new exe, with as many slots, and the same
        slot in it. When its own slot holds a function matched for certain, the new slot must hold its port. Gives None when the place
        is not such a slot (other rules then), else (place or None, how, firm): for such a place this rule decides, but when firm is
        False (a slot past the start whose function is not matched for certain) it does not overrule another answer."""
        if np is None:
            return None  # (without numpy no tables are found: the other rules decide; the review)
        got = self.old.table_place(rva)
        if got is None:
            return None
        vt, k, key = got
        name = key[0] + (f" (its part at +{key[1]:#x})" if key[1] else "")
        olds = self.old.tables_of(key)
        if olds != [vt]:
            return None, f"slot {k} of a type table of {name}, but the old exe has {len(olds)} tables of that type: check by hand", True
        news = self.new.tables_of(key)
        if not news and without_numbers(key[0]) != key[0]:
            return None  # (a lambda's or an anonymous namespace's type: its name has this build's numbers; the other rules decide)
        if len(news) != 1:
            return None, f"slot {k} of the type table of {name}, but the new exe has {len(news)} tables of that type: check by hand", True
        n_old, n_new = self.old.slots(vt), self.new.slots(news[0])
        if n_old != n_new:
            return None, (f"slot {k} of the type table of {name}, which has {n_old} slots in the old exe and {n_new} in the new: "
                          f"check by hand"), True
        at = news[0] + 8 * k
        t_old, t_new = self.old.word_pointer(rva), self.new.word_pointer(at)
        f = self.old.function_at(t_old)
        x = self.sure_match(*f) if f and f[0] == t_old else None
        if x is not None and x != t_new:
            return None, (f"slot {k} of the type table of {name} (new {news[0]:#x}), but there it holds {t_new:#x}, not {x:#x}, the "
                          f"port of {t_old:#x}: check by hand"), True
        held = f", holding {t_new:#x}, the port of {t_old:#x} (matched for certain)" if x is not None else ""
        return at, (f"slot {k} of the type table of {name}: the one table of that type in each exe ({vt:#x}, {news[0]:#x}), "
                    f"{n_old} slots in both{held}"), (k == 0 or x is not None)

    def same_data(self, x_old, x_new, rva, at, need_own=True):
        """(fix190) Whether the data from x_old through the place rva (old) is the same as from x_new through at (new): (yes, no, or
        None when the files do not have those bytes (data the game only makes at run time); in words). A string at the place counts
        to its end, anything else as one 8-byte word. Everything must be the same bytes, but for 8-byte words that point into the
        image in both (where the two exes keep a pointer); no such pointer may point elsewhere than a port it can check. With
        need_own, the place itself must say something: its string, or its own word a pointer to the port of its old target (code
        matched for certain). Zeros or numbers the same in both prove nothing: the same bytes a few words on would pass too (the
        review)."""
        a0 = self.old.read(rva, 256) or self.old.read(rva, 8)
        if a0 is None:
            return None, "no bytes of it in the file to compare (data made at run time)"
        end = a0.find(b"\0")
        string = end >= 4 and all(32 <= c < 127 for c in a0[:end])
        width = end + 1 if string else 8
        n = rva - x_old + width
        a, b = self.old.read(x_old, n), self.new.read(x_new, n)
        if a is None or b is None:
            return None, "not all of the data from there to it is in the files"
        # (its own word, when it is a pointer, always: the same number could point to other code in the new exe)
        own = None
        if not string and rva % 8 == 0 and (x_old - x_new) % 8 == 0:
            pa = self.old.pointer(struct.unpack_from("<Q", a, rva - x_old)[0])
            if pa is not None:
                pb = self.new.pointer(struct.unpack_from("<Q", b, rva - x_old)[0])
                v = self.pointer_fits(pa, pb) if pb is not None else False
                if v is False or (v is None and need_own):
                    return False, f"its own pointer (to {pa:#x}) does not go to a port it can check"
                if v:
                    own = (pa, pb)
        if need_own and not string and own is None:
            return False, "nothing at the place itself to check: check by hand"
        if a == b:
            words = "the same bytes from there through its string" if string else "the same bytes from there through it"
        elif (x_old - x_new) % 8:
            return False, "the data from there to it differs"
        else:
            pointers = 0
            k = 0
            while k < n:
                if a[k] == b[k]:
                    k += 1
                    continue
                start = k - (x_old + k) % 8  # (the 8-byte word holding it, at its place in the file)
                if start < 0 or start + 8 > n:
                    return False, "the data from there to it differs"
                pa = self.old.pointer(struct.unpack_from("<Q", a, start)[0])
                pb = self.new.pointer(struct.unpack_from("<Q", b, start)[0])
                if pa is None or pb is None:
                    return False, f"the data from there to it differs at {x_old + start:#x}"
                if self.pointer_fits(pa, pb) is False:
                    return False, f"the pointer at {x_old + start:#x} goes elsewhere than its port"
                pointers += 1
                k = start + 8
            words = f"the same from there through it but for {pointers} pointer(s) into the image"
        if own:
            words += f"; its own goes to {own[1]:#x}, the port of {own[0]:#x} (matched for certain)"
        return True, words


def code_functions_near(old, rvas):
    out = set()
    for r in rvas:
        f = old.function_at(r)
        if f:
            out.add(f)
    return sorted(out)


def all_referrers(old, rva, limit=8):
    """Functions of the old exe with an instruction whose rip-relative target is rva, or just before it (within 0x400): a scan of
    .text for the 32-bit distance (numpy; without it, none)."""
    try:
        import numpy as np
    except ImportError:
        print("  (numpy not installed: places in data referred to only outside the mod's own functions are not found; pip install numpy)")
        return []
    name, va, vsize, raw, rawsize, chars = old.text
    size = min(vsize, rawsize)
    exact = []
    near = []
    chunk = 1 << 24
    for start in range(0, size - 4, chunk):
        end = min(size - 4, start + chunk)
        buf = np.frombuffer(old.data, dtype=np.uint8, count=end - start + 3, offset=raw + start).astype(np.int64)
        d = buf[:-3] | (buf[1:-2] << 8) | (buf[2:-1] << 16) | (buf[3:] << 24)
        d = np.where(d >= 2**31, d - 2**32, d)
        target = np.arange(start, end, dtype=np.int64) + va + 4 + d
        # (references to it exactly first, then to just before it: chance 4-byte matches do not crowd the real ones out; the review)
        for k in np.nonzero(target == rva)[0]:
            f = old.function_at(va + start + int(k))
            if f and f not in exact:
                exact.append(f)
        if len(near) < limit:
            for k in np.nonzero((target < rva) & (target > rva - 0x400))[0]:
                f = old.function_at(va + start + int(k))
                if f and f not in near:
                    near.append(f)
                    if len(near) >= limit:
                        break
        if len(exact) >= limit:
            break
    return (exact + [f for f in near if f not in exact])[: 2 * limit]


def bake(args):
    old = Exe(args.old)
    places, unclear, skipped, unconverted = inventory(args.src)
    print(f"old exe {args.old}: version {old.version}, {len(old.functions) - len(old.pseudo)} functions, {len(old.pseudo)} stretches of "
          f"code without an unwind entry")
    print(f"the code asks for {len(places)} places ({sum(len(p['runs']) for p in places.values())} checked runs); {len(unclear)} calls not understood")
    for u in unclear:
        print("  not understood:", u)

    # Every checked run must be what the old exe has (else the code or this tool is wrong).
    wrong = []
    for rva, p in sorted(places.items()):
        for n, (want, where) in p["runs"].items():
            if old.read(rva, n) != want:
                wrong.append(f"{rva:#x} ({where}): not the 1.16.236 bytes")
    for w in wrong:
        print("  WRONG:", w)
    print(f"{len(unconverted)} places in files not converted to GameCode yet (in a ported build their features read 1.16.236's places)")
    if args.inventory:
        for u in unconverted:
            print("  not converted:", u)
        for s in skipped:
            print("  a variable (must be inside a wrapper over GameCode):", s)
        for rva, p in sorted(places.items()):
            print(f"  {rva:#09x} {'code' if old.is_code(rva) else 'data'} runs {sorted(p['runs'])} {', '.join(sorted(p['files']))}")
        return 0 if not wrong and not unclear else 1

    new = Exe(args.new)
    print(f"new exe {args.new}: version {new.version}, {len(new.functions) - len(new.pseudo)} functions, {len(new.pseudo)} stretches of "
          f"code without an unwind entry")
    old_al = AddressLibrary(args.old_al) if args.old_al else None
    new_al = AddressLibrary(args.new_al) if args.new_al else None
    porter = Porter(old, new, old_al, new_al)

    results = {}
    code = [r for r in sorted(places) if old.is_code(r)]
    data = [r for r in sorted(places) if not old.is_code(r)]
    # (first the functions one of a kind in both exes: they place the others, which are matched by where their neighbours moved; the
    # review)
    for rva in code:
        f = old.function_at(rva)
        if f:
            porter.map_function(*f, allow_nearest=False)
    for rva in code:
        at, how = porter.map_code(rva, places[rva]["runs"])
        results[rva] = (at, how)
    matched_funcs = [f for f in code_functions_near(old, code) if porter.fmap.get(f[0], (None,))[0] is not None]
    for rva in data:
        at, how, exact = porter.map_data(rva, matched_funcs)
        if not exact:
            # (fix190) Also when the functions the mod uses refer only to places just before it: an exact reference anywhere is
            # better, and the other answer must agree with it (the review).
            refs = all_referrers(old, rva)
            if refs:
                at2, how2, exact2 = porter.map_data(rva, refs)
                if exact2 and at is not None and at2 is not None and at2 != at:
                    at, how = None, f"{how2}; but {how}, which gives {at:#x}: not certain"
                elif exact2 or at is None:
                    at, how = at2, how2
        # (fix190) A slot holding the only pointer to a function matched for certain: found by that pointer when nothing refers to
        # it, and else it must agree.
        by_pointer, why = porter.map_by_pointer(rva)
        if by_pointer is not None:
            if at is None:
                at, how = by_pointer, why
            elif at != by_pointer:
                at, how = None, f"{how}; but it is not {by_pointer:#x}, {why}: not certain"
            else:
                how += "; and it is the only word pointing to its function's port"
        # (fix192) A slot of a type table (its start included) is decided by the table's type alone: the rules above go by what is
        # near it, and the linker may put tables of one kind in another order (1.16.244 swapped two blur passes' tables; the review).
        # When they gave another answer, that is said (a WARNING). A slot past the start whose function is not matched for certain
        # does not overrule another answer: then neither is certain (the review).
        by_type = porter.map_by_type(rva)
        if by_type is not None:
            t_at, t_how, firm = by_type
            if at is not None and t_at is not None and at != t_at and not firm:
                print(f"  WARNING: {rva:#x}: by its type {t_at:#x}, but the other rules gave {at:#x}: left out")
                t_at, t_how = None, (f"{t_how}; but the other rules gave {at:#x} ({how}), and its slot's function is not matched for "
                                     f"certain: check by hand")
            elif at is not None and at != t_at:
                print(f"  WARNING: {rva:#x}: by its type {'not found' if t_at is None else hex(t_at)}, but the other rules gave {at:#x}")
                t_how += f"; WARNING: the other rules gave {at:#x} ({how})"
            elif at is not None:
                t_how += "; the other rules agree"
            elif "not certain" in how:
                t_how += f"; the other rules: {how}"
            at, how = t_at, t_how
        results[rva] = (at, how)

    # (fix190) Places matched by hand (tools/port/manual_<version>.txt): code the new version changed, so no rule above can match it,
    # but read by hand in both exes and the same in what the mod needs. Each line gives the new place, its bytes there (they must
    # cover the code's longest checked run there) and the start of the new function it is in; the bytes and the function are checked
    # against the new exe (a few bytes alone are found in many places: a mistyped place must not pass; the review), and those runs
    # are then the new bytes. When the tool found the place itself somewhere else, it says so.
    manual = {}
    manual_path = args.manual or os.path.join(os.path.dirname(os.path.abspath(__file__)), f"manual_{'_'.join(new.version.split('.')[:3])}.txt")
    if not args.selftest and os.path.exists(manual_path):
        with open(manual_path, encoding="utf-8") as f:
            for number, line in enumerate(f, 1):
                body, _, why = line.partition("#")
                parts = body.split()
                if not parts:
                    continue
                try:
                    rva, at, want, fn_start = int(parts[0], 16), int(parts[1], 16), bytes.fromhex(parts[2]), int(parts[3], 16)
                except (IndexError, ValueError):
                    print(f"  manual line {number} not understood (old rva, new rva, bytes, new function start): {line.strip()}")
                    continue
                fn_new = new.function_at(at)
                if rva not in places:
                    print(f"  manual line {number}: {rva:#x} is not asked for by the code")
                elif new.read(at, len(want)) != want:
                    print(f"  manual line {number}: the bytes at {at:#x} in the new exe are not {want.hex()}: left out")
                elif fn_new is None or fn_new[0] != fn_start:
                    print(f"  manual line {number}: {at:#x} is not in the new exe's function at {fn_start:#x}"
                          f" ({'in ' + hex(fn_new[0]) if fn_new else 'in no function'}): left out")
                elif p_runs := [k for k in places[rva]["runs"] if k > len(want)]:
                    print(f"  manual line {number}: its bytes are shorter than the code's checked run ({max(p_runs)}): left out")
                else:
                    manual[rva] = at
                    auto = results.get(rva, (None,))[0]
                    note = ""
                    if auto is not None and auto != at:
                        print(f"  manual line {number}: WARNING: the tool found {rva:#x} at {auto:#x} itself, not at {at:#x}")
                        note = f"; the tool itself found it at {auto:#x}"
                    results[rva] = (at, f"by hand ({os.path.basename(manual_path)} line {number}: {why.strip()}){note}")

    # The checked runs: the new bytes there, alike but for distances.
    entries = []
    pool = bytearray()
    report = []
    for rva in sorted(places):
        at, how = results[rva]
        p = places[rva]
        if at is None:
            report.append((rva, None, f"NOT FOUND: {how}", p))
            continue
        n = max(p["runs"]) if p["runs"] else 0
        if n and rva in manual:
            entries.append((rva, at, len(pool), n))
            pool += new.read(at, n)
        elif n:
            ok = True
            for k, (want, where) in p["runs"].items():
                a, ins_old = masked_bytes(old, rva, k, porter.cache_old)
                b, ins_new = masked_bytes(new, at, k, porter.cache_new)
                if a is None or a != b:
                    ok = False
                    how += f"; its checked run {where} ({k} bytes) differs there"
                    continue
                # (its calls and data references must go where the places they went to were found: a call to a place ported must call
                # that place's new address; the review)
                for io, inew in zip(ins_old, ins_new):
                    for t_old, t_new, table in ((io[3], inew[3], False), (io[4], inew[4], False), (io[5], inew[5], True)):
                        if t_old is None or t_new is None:
                            continue
                        expect = results.get(t_old, (None,))[0]
                        if expect is None:
                            f = porter.fmap.get(t_old)
                            expect = f[0] if f and f[0] is not None and "changed" not in f[1] else None
                        if expect is None and table:
                            # (fix190) a jump table inside a function: as far into the port of its function as into the old one
                            f = old.function_at(t_old)
                            m = porter.fmap.get(f[0]) if f and old.text[1] <= t_old < old.text_end else None
                            expect = m[0] + (t_old - f[0]) if m and m[0] is not None and "changed" not in m[1] else None
                        if expect is not None and t_new != expect:
                            ok = False
                            how += f"; in its checked run {where} a reference to {t_old:#x} goes to {t_new:#x}, not to its port {expect:#x}"
            if not ok:
                report.append((rva, at, f"NOT PORTED: {how}", p))
                continue
            entries.append((rva, at, len(pool), n))
            pool += new.read(at, n)
        else:
            entries.append((rva, at, 0, 0))
        report.append((rva, at, how, p))

    if args.selftest:
        bad = [(r, a) for r, a, _, _ in entries if r != a]
        missing = [r for r, a, h, p in report if a is None or h.startswith("NOT")]
        print(f"selftest: {len(entries)} places found, {len(bad)} not where they are, {len(missing)} not found")
        for r, a in bad[:20]:
            print(f"  {r:#x} -> {a:#x}")
        for r in missing[:20]:
            print(f"  missing {r:#x}")
        return 0 if not bad and not missing and not wrong else 1

    write_table(args.out, entries, pool, old, new, old_al, new_al)
    write_report(args.report, report, unclear, wrong, old, new, unconverted)
    print(f"{len(entries)} of {len(places)} places ported; the table: {args.out}; the report: {args.report}")
    return 0


def write_table(path, entries, pool, old, new, old_al, new_al):
    lines = [
        "// The port table for Starfield " + new.version + " (GameCode.cpp; docs/PORTING.md). Made by tools/port/bake.py from the two versions'",
        "// Starfield.exe: do not edit by hand. Each entry: the place's rva in 1.16.236, its rva in the new version, and for a run of code the",
        "// mod checks, where its new bytes start in kBytes and how many.",
        f"// From: {os.path.basename(old.path)} {old.version} and {os.path.basename(new.path)} {new.version}"
        + (f"; Address Library {old_al.version} and {new_al.version}" if old_al and new_al else "; no Address Library"),
        "#pragma once",
        "",
        "namespace GameCode::Table",
        "{",
        f'    inline constexpr const char* kFrom = "baked from {old.version} and {new.version} by tools/port/bake.py, {len(entries)} places";',
        f"    inline constexpr std::array<Entry, {len(entries)}> kEntries{{ {{",
    ]
    for rva, at, off, n in entries:
        lines.append(f"        {{ {rva:#x}, {at:#x}, {off}, {n} }},")
    lines.append("    } };")
    lines.append(f"    inline constexpr std::array<uint8_t, {len(pool)}> kBytes{{ {{")
    for i in range(0, len(pool), 24):
        lines.append("        " + ", ".join(f"{b:#04x}" for b in pool[i:i + 24]) + ",")
    lines.append("    } };")
    lines.append("}")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def write_report(path, report, unclear, wrong, old, new, unconverted=()):
    found = sum(1 for _, a, h, _ in report if a is not None and not h.startswith("NOT"))
    lines = [f"# Port report: Starfield {old.version} -> {new.version}", "",
             f"{found} of {len(report)} places ported. Calls not understood: {len(unclear)}. Checked runs not as in {old.version}: {len(wrong)}.", ""]
    if unclear or wrong:
        lines.append("## To look at first")
        lines += [f"- {u}" for u in unclear + wrong]
        lines.append("")
    if unconverted:
        lines.append(f"## Not converted to GameCode yet ({len(unconverted)}; in this build their features read 1.16.236's places)")
        lines += [f"- {u}" for u in unconverted]
        lines.append("")
    by_file = defaultdict(list)
    for rva, at, how, p in report:
        for f in sorted(p["files"]):
            by_file[f].append((rva, at, how))
    for f in sorted(by_file):
        rows = by_file[f]
        bad = sum(1 for _, a, h in rows if a is None or h.startswith("NOT"))
        lines.append(f"## {f}: {len(rows) - bad} of {len(rows)}")
        for rva, at, how in rows:
            lines.append(f"- {rva:#x} -> {at:#x}: {how}" if at is not None and not how.startswith("NOT") else f"- {rva:#x}: {how}")
        lines.append("")
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.normpath(os.path.join(here, "..", ".."))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--old", required=True, help="Starfield.exe 1.16.236")
    ap.add_argument("--new", help="Starfield.exe of the version to port to (1.16.244)")
    ap.add_argument("--old-al", help="versionlib-1-16-236-0.bin (Address Library for SFSE Plugins)")
    ap.add_argument("--new-al", help="versionlib-1-16-244-0.bin")
    ap.add_argument("--src", default=os.path.join(repo, "src"))
    ap.add_argument("--out", default=os.path.join(repo, "src", "CreationEngine", "memory", "GameCode_1_16_244.inc"))
    ap.add_argument("--report", default=os.path.join(repo, "docs", "PORT_REPORT_1_16_244.md"))
    ap.add_argument("--manual", help="places matched by hand (default: manual_<new version>.txt next to this tool)")
    ap.add_argument("--inventory", action="store_true", help="only list what the code asks for (checked against --old)")
    ap.add_argument("--selftest", action="store_true", help="--new is --old: everything must be found where it is (nothing written)")
    args = ap.parse_args()
    if not args.inventory and not args.new:
        ap.error("--new is needed (or --inventory)")
    if bool(args.old_al) != bool(args.new_al):
        ap.error("both Address Libraries, or neither")
    sys.exit(bake(args))


if __name__ == "__main__":
    main()
