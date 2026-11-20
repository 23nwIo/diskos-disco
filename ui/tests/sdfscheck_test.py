#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""sdfscheck.c: is a shared card's filesystem still marked in use by the computer?

ext4: real images from mke2fs; when run as root, also loop-mounted, so the "in use" mark is the one the running Linux
kernel really writes. Jo's card from 2026-11-20 (superblock from his hexdump: needs_recovery + orphan_present) must
read as in use. FAT and exFAT: synthetic boot sectors (no mkfs.vfat/exfat or vfat driver in the cloud VM).
"""
import os, struct, subprocess, sys, tempfile
from pathlib import Path

app = Path(__file__).resolve().parents[1]
DRIVER = r'''
#include "sdfscheck.h"
#include <stdio.h>
#include <stdlib.h>
int main(int c, char **v){
    if(c != 3) return 2;
    FILE *f = fopen(v[1], "rb"); if(!f) return 2;
    unsigned char b[SDFS_HEAD] = {0};
    if(fseek(f, atol(v[2]), SEEK_SET) != 0) return 2;
    size_t n = fread(b, 1, sizeof b, f); fclose(f);
    printf("%s %d\n", sdfs_kind(b, n), sdfs_state(b, n));
    return 0;
}
'''

def run(*a, **k):
    return subprocess.run(list(a), capture_output=True, text=True, **k)

def main():
    with tempfile.TemporaryDirectory(prefix="diskos-sdfs-") as t:
        t = Path(t)
        (t / "d.c").write_text(DRIVER)
        exe = t / "d"
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(app),
                        str(t / "d.c"), str(app / "sdfscheck.c"), "-o", str(exe)], check=True)
        def state(path, off=0):
            r = run(str(exe), str(path), str(off)); assert r.returncode == 0, r
            k, s = r.stdout.split(); return k, int(s)

        img = t / "e.img"
        with open(img, "wb") as f: f.truncate(64 << 20)
        subprocess.run(["mke2fs", "-q", "-t", "ext4", "-F", str(img)], check=True)
        assert state(img) == ("ext", 0), state(img)
        print("PASS fresh ext4 is clean")
        for feat in ("needs_recovery", "orphan_present"):
            r = run("debugfs", "-w", "-R", "feature " + feat, str(img))
            if "Unknown feature" in r.stdout + r.stderr:
                print("SKIP debugfs does not know " + feat); continue
            assert state(img) == ("ext", 1), (feat, state(img))
            run("debugfs", "-w", "-R", "feature -" + feat, str(img))
            assert state(img) == ("ext", 0), (feat, state(img))
            print("PASS ext4 with %s is in use" % feat)

        if os.geteuid() == 0:
            mnt = t / "mnt"; mnt.mkdir()
            if run("mount", "-o", "loop", str(img), str(mnt)).returncode == 0:
                try:
                    (mnt / "x").write_text("x")
                    assert state(img) == ("ext", 1), state(img)
                finally:
                    subprocess.run(["umount", str(mnt)], check=True)
                assert state(img) == ("ext", 0), state(img)
                print("PASS ext4 loop-mounted by this kernel is in use, clean again after umount")
            else:
                print("SKIP loop mount not available")
        else:
            print("SKIP loop mount (not root)")

        # Jo's card, superblock bytes 0x400..0x47F as dumped on the device 2026-11-20
        jo = bytes.fromhex(
            "00207700 0056dc01 19d11700 5b95b001 a61b7700 00000000 02000000 02000000"
            "00800000 00800000 00200000 f507fe6a f507fe6a 5d00ffff 53ef0100 01000000"
            "c6ebf86a 00000000 00000000 01000000 00000000 0b000000 00010000 3c100000"
            "c6220000 6b040100 b9a77b34 8c3f4d02 b9440ba7 5d76e4fc 00000000 00000000")
        p = t / "jo.img"; p.write_bytes(bytes(1024) + jo + bytes(1024 - len(jo)))
        assert state(p) == ("ext", 1), state(p)
        cleaned = bytearray(p.read_bytes())
        struct.pack_into("<II", cleaned, 1024 + 0x60, 0x22c6 & ~0x4, 0x1046b & ~0x10000)
        p.write_bytes(cleaned); assert state(p) == ("ext", 0), state(p)
        print("PASS Jo's card reads as in use, clean once recovered")

        def boot(kind, dirty):
            b = bytearray(512); b[510:512] = b"\x55\xaa"
            if kind == "fat32": b[82:90] = b"FAT32   "; b[0x41] = 1 if dirty else 0
            if kind == "fat16": b[54:62] = b"FAT16   "; b[0x25] = 1 if dirty else 0
            if kind == "exfat": b[3:11] = b"EXFAT   "; struct.pack_into("<H", b, 106, 2 if dirty else 0)
            return bytes(b)
        for kind, name in (("fat32", "fat"), ("fat16", "fat"), ("exfat", "exfat")):
            for dirty in (0, 1):
                p = t / ("%s%d.img" % (kind, dirty)); p.write_bytes(bytes(4 << 20) + boot(kind, dirty))
                assert state(p, 4 << 20) == (name, dirty), (kind, dirty, state(p, 4 << 20))
        p = t / "zero.img"; p.write_bytes(bytes(4096))
        assert state(p) == ("unknown", -1)
        print("PASS FAT16/FAT32/exFAT dirty bits; an unknown filesystem is unknown")
    print("COMPLETE sdfscheck_test.py")

main()
