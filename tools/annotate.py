#!/usr/bin/env python3
"""annotate.py — disassemble a range of a guest library with managed method
names resolved on call targets.

Built for reading il2cpp-generated ARM64: the disassembly alone tells you
nothing about which managed method a `bl` reaches, and the method list alone
tells you nothing about what a method does. Together they are how the Beat
Saber DLC gating was read (see PORTING.md).

Usage:
    tools/annotate.py <lib> <methods.txt> <start> <end> [all]

    <lib>         the guest .so (e.g. beatsaber/lib/arm64-v8a/libil2cpp.so)
    <methods.txt> "0x<image offset> <name>" per line, produced by
                  KL_DUMP_METHODS=build/methods.txt ./build/t_il2cpp <lib> <data>
    <start>/<end> image offsets (hex) of the range to disassemble
    all           also print instructions with no resolved note (default:
                  only lines whose call target resolves to a known method)

Callers: to find which methods call a target, scan all `bl` sites once and
invert the map (see PORTING.md §"reading the game").
"""
import bisect
import re
import subprocess
import sys


def main():
    lib, methods_path = sys.argv[1], sys.argv[2]
    lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
    show_all = len(sys.argv) > 5 and sys.argv[5] == "all"

    methods = []
    for line in open(methods_path):
        off, name = line.rstrip("\n").split(" ", 1)
        methods.append((int(off, 16), name))
    methods.sort()
    starts = [m[0] for m in methods]

    def exact(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return methods[i][1] if i >= 0 and methods[i][0] == addr else None

    out = subprocess.run(
        ["xcrun", "llvm-objdump", "-d", "--no-show-raw-insn",
         f"--start-address={lo:#x}", f"--stop-address={hi:#x}", lib],
        capture_output=True, text=True).stdout

    for line in out.splitlines():
        m = re.match(r"^\s+([0-9a-f]+):\s+(\S.*?)\s*$", line)
        if not m:
            continue
        addr, ins = m.groups()
        note = ""
        bl = re.match(r"(bl|b|cbnz|cbz|tbnz|tbz)\s+0x([0-9a-f]+)", ins)
        if bl:
            nm = exact(int(bl.group(2), 16))
            if nm:
                note = f"   ; -> {nm}"
            elif bl.group(1) == "bl":
                note = "   ; -> (runtime)"
        if show_all or note:
            print(f"{addr}: {ins}{note}")


if __name__ == "__main__":
    main()
