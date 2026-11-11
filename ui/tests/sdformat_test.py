#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""SD card formatter (ui/sdformat_core.c) on sparse image files.

ext4: e2fsck -fn must be clean, on empty images of several sizes and on an image full of random bytes (an old card).
      As root (loop devices available) the image is also mounted, filled with files, unmounted and checked again.
FAT32: no fsck.fat here, so the structure is checked against the FAT32 specification: MBR, BPB, backup boot sector,
      FSInfo, both FATs, cluster count in the FAT32 range, 4 MiB alignment, the label entry; blkid must see vfat/FAT32.

Run: python3 ui/tests/sdformat_test.py   (needs cc, e2fsck, blkid)"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
UI = os.path.dirname(HERE)


def build(tmp):
    exe = os.path.join(tmp, "sdformat_test")
    subprocess.run(["cc", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-I" + UI, os.path.join(HERE, "sdformat_test.c"),
                    os.path.join(UI, "sdformat_core.c"), "-o", exe], check=True)
    return exe


def image(path, size, random_mb=0):
    with open(path, "wb") as f:
        f.truncate(size)
        if random_mb:
            f.seek(0)
            f.write(os.urandom(random_mb << 20))


def fmt(exe, kind, path):
    r = subprocess.run([exe, kind, path], capture_output=True, text=True)
    assert r.returncode == 0 and r.stdout.startswith("OK"), (kind, path, r.stdout, r.stderr)


def e2fsck(path):
    r = subprocess.run(["e2fsck", "-fn", path], capture_output=True, text=True)
    lines = [l for l in r.stdout.splitlines() if l and not l.startswith(("Pass ", "e2fsck ", "DISC:"))]
    assert r.returncode == 0 and not lines, (path, r.returncode, r.stdout, r.stderr)
    return r.stdout.strip().splitlines()[-1]


def check_fat32(path, size):
    with open(path, "rb") as f:
        mbr = f.read(512)
        assert mbr[510:512] == b"\x55\xaa"
        status, ptype, start, count = mbr[446], mbr[450], *struct.unpack_from("<II", mbr, 454)
        assert status == 0 and ptype == 0x0C, (status, ptype)
        assert start == 8192 and start + count == size // 512, (start, count)
        assert mbr[462:510] == bytes(48), "only one partition"
        assert struct.unpack_from("<I", mbr, 440)[0] != 0, "disk signature"
        p0 = start * 512
        f.seek(p0); bs = f.read(512)
        f.seek(p0 + 6 * 512); assert f.read(512) == bs, "backup boot sector"
        (bps, spc, rsv, nfat, rootent, tot16, media, fat16, _spt, _hd, hidden, tot32, fatsz, extf, ver, rootclus,
         fsinfo, bk) = struct.unpack_from("<HBHBHHBHHHIIIHHIHH", bs, 11)
        assert bs[0] == 0xEB and bs[2] == 0x90 and bs[510:512] == b"\x55\xaa"
        assert bps == 512 and spc in (1, 2, 4, 8, 16, 32, 64) and nfat == 2 and rootent == 0 and tot16 == 0
        assert media == 0xF8 and fat16 == 0 and hidden == start and tot32 == count
        assert extf == 0 and ver == 0 and rootclus == 2 and fsinfo == 1 and bk == 6
        assert bs[66] == 0x29 and bs[71:82] == b"DISC       " and bs[82:90] == b"FAT32   "
        data = rsv + 2 * fatsz
        clusters = (count - data) // spc
        assert 65525 <= clusters <= 0x0FFFFFF4, clusters           # FAT32 by the specification's own rule
        assert fatsz * 128 >= clusters + 2
        assert (start + data) % 8192 == 0, "data area on a 4 MiB boundary"
        f.seek(p0 + 512); fi = f.read(512)
        f.seek(p0 + 7 * 512); assert f.read(512) == fi, "backup FSInfo"
        lead, struc, free, nxt, trail = struct.unpack_from("<I", fi, 0)[0], *struct.unpack_from("<III", fi, 484), struct.unpack_from("<I", fi, 508)[0]
        assert (lead, struc, trail) == (0x41615252, 0x61417272, 0xAA550000)
        assert free == clusters - 1 and nxt == 3
        fats = []
        for k in range(2):
            f.seek(p0 + (rsv + k * fatsz) * 512)
            fats.append(f.read(fatsz * 512))
        assert fats[0] == fats[1], "FAT copies differ"
        assert struct.unpack_from("<III", fats[0], 0) == (0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF)
        assert fats[0][12:] == bytes(len(fats[0]) - 12), "FAT not empty"
        f.seek(p0 + data * 512); root = f.read(spc * 512)
        assert root[0:11] == b"DISC       " and root[11] == 0x08 and root[32:] == bytes(len(root) - 32)
    r = subprocess.run(["blkid", "-p", "-O", str(p0), path], capture_output=True, text=True)
    assert 'TYPE="vfat"' in r.stdout and 'VERSION="FAT32"' in r.stdout, r.stdout
    return spc, clusters


def main():
    tmp = tempfile.mkdtemp(prefix="sdformat_")
    exe = build(tmp)
    img = os.path.join(tmp, "card.img")
    G = 1 << 30
    for size in (64 << 20, 300 << 20, 2 * G, 33 * G, 129 * G, 1024 * G):
        image(img, size); fmt(exe, "ext4", img)
        print("ext4 %6d MiB: %s" % (size >> 20, e2fsck(img)))
    for size in (64 << 20, 2 * G, 8 * G, 31 * G, 33 * G, 129 * G):
        image(img, size); fmt(exe, "fat32", img)
        spc, cl = check_fat32(img, size)
        print("fat32 %6d MiB: %d sectors/cluster, %d clusters" % (size >> 20, spc, cl))
    # an old card: random bytes everywhere the formatter might rely on zeros; then the other format over it
    image(img, 600 << 20, random_mb=600)
    fmt(exe, "fat32", img); check_fat32(img, 600 << 20)
    fmt(exe, "ext4", img); e2fsck(img)
    if os.geteuid() == 0:
        mnt = os.path.join(tmp, "mnt"); os.mkdir(mnt)
        if subprocess.run(["mount", "-o", "loop", img, mnt]).returncode == 0:
            try:
                for i in range(2000):
                    d = os.path.join(mnt, "Artist %d" % (i % 40)); os.makedirs(d, exist_ok=True)
                    with open(os.path.join(d, "%04d.flac" % i), "wb") as f: f.write(os.urandom(i * 37 % 20000))
                with open(os.path.join(mnt, "big.dsf"), "wb") as f:
                    for _ in range(300): f.write(os.urandom(1 << 20))
            finally:
                subprocess.run(["umount", mnt], check=True)
            print("ext4 after kernel writes: %s" % e2fsck(img))
        else:
            print("ext4 kernel mount: skipped (no loop device)")
    # too small / too large
    image(img, 32 << 20)
    r = subprocess.run([exe, "ext4", img], capture_output=True, text=True); assert r.returncode == 1 and "too small" in r.stdout
    os.remove(img)
    print("sdformat: all checks passed")


if __name__ == "__main__":
    sys.exit(main())
