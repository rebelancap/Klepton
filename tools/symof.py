#!/usr/bin/env python3
"""Name the `lib+0xoffset` addresses in a klepton log.

Why this exists: the runtime prints callers as `libOculusXRPlugin.so+0xf294`
because kl_image only parses `.dynsym`, and the interesting guest libraries keep
a `.symtab` that it never looks at (libOculusXRPlugin.so has 1009 symbols against
its handful of dynamic ones). So the log has the address and the library has the
name, and joining them is the difference between reading

    [ovrp+] 1 ovrp_SetupDisplayObjects2 <- libOculusXRPlugin.so+0x12014

and reading

    ... <- libOculusXRPlugin.so+0x12014 [OculusDisplayProvider::CreateMobileDisplayObjects+0x74]

which is what turned "the display subsystem stops after one frame" into "it stops
because CreateMobileDisplayObjects reported failure" in a single pass. Doing the
join here rather than in the shim keeps a build-time symbol table out of the
runtime, and works on logs captured weeks ago.

    tools/symof.py <log>            # or on a pipe
    tools/symof.py <log> --libs <dir>

Nearest-preceding-symbol, so an address inside a function reports that function
plus an offset. A library with no symbols is left exactly as it was.
"""
import argparse
import bisect
import os
import re
import struct
import subprocess
import sys

PAT = re.compile(r"(lib[\w.+-]*\.so)\+0x([0-9a-fA-F]+)")


def load_elf(path):
    """(sorted addrs, names) read straight out of an ELF64 .symtab/.dynsym.

    This is the primary path, and it exists because `nm` is not portable here:
    Apple's /usr/bin/nm answers `no symbols` for an aarch64 ELF — including
    libUE4.so, which carries 446k of them — and the toolchain llvm-nm is not on
    PATH on every machine. A tool that quietly reports "no names" is worse than
    one that fails, so the join no longer depends on an external nm at all.
    Fifty lines of struct.unpack read exactly what is needed and nothing else.
    """
    try:
        with open(path, "rb") as f:
            d = f.read()
    except OSError:
        return [], []
    if d[:4] != b"\x7fELF" or len(d) < 0x40 or d[4] != 2:      # ELF64 only
        return [], []
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum = struct.unpack_from("<HH", d, 0x3a)
    if not shoff or shoff + shnum * shentsize > len(d):
        return [], []
    # sh_offset of every section, for resolving each symbol table's sh_link.
    sh_off = [struct.unpack_from("<Q", d, shoff + i * shentsize + 0x18)[0]
              for i in range(shnum)]
    out = []
    for i in range(shnum):
        h = shoff + i * shentsize
        typ, = struct.unpack_from("<I", d, h + 4)
        if typ not in (2, 11):                                 # SYMTAB, DYNSYM
            continue
        off, size = struct.unpack_from("<QQ", d, h + 0x18)
        link, = struct.unpack_from("<I", d, h + 0x28)
        if not size or link >= shnum:
            continue
        stroff = sh_off[link]
        for j in range(size // 24):
            o = off + j * 24
            nm, info, _other, _shndx, val, _sz = struct.unpack_from("<IBBHQQ", d, o)
            if not val or (info & 0xF) not in (1, 2):          # OBJECT, FUNC
                continue
            end = d.find(b"\0", stroff + nm)
            out.append((val, d[stroff + nm:end].decode("utf8", "replace")))
    out.sort()
    return [s[0] for s in out], [s[1] for s in out]


def load(path):
    """(sorted addrs, names) for a guest library, or ([], []) if unreadable."""
    if not os.path.exists(path):
        return [], []
    addrs, names = load_elf(path)
    if addrs:
        return addrs, names
    try:
        out = subprocess.run(["nm", "--defined-only", path],
                             capture_output=True, text=True).stdout
    except OSError:
        return [], []
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3 or not parts[0].strip():
            continue
        try:
            syms.append((int(parts[0], 16), parts[2]))
        except ValueError:
            pass
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="log file (default: stdin)")
    ap.add_argument("--libs", default="beatsaber/lib/arm64-v8a",
                    help="directory holding the guest .so files")
    args = ap.parse_args()

    cache = {}

    def name(lib, off):
        if lib not in cache:
            cache[lib] = load(os.path.join(args.libs, lib))
        addrs, names = cache[lib]
        if not addrs:
            return None
        i = bisect.bisect_right(addrs, off) - 1
        if i < 0:
            return None
        return "%s+0x%x" % (names[i], off - addrs[i])

    def sub(m):
        n = name(m.group(1), int(m.group(2), 16))
        return m.group(0) + (" [%s]" % n if n else "")

    src = open(args.log, errors="replace") if args.log else sys.stdin
    for line in src:
        sys.stdout.write(PAT.sub(sub, line))


if __name__ == "__main__":
    main()
