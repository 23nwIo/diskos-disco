#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 diskOS contributors
"""Sidecar cover lookup (ui/art.c art_cover_find / art_sidecar_signature) on real directories.

The functions are extracted from art.c and compiled alone. Only the "is this card case-sensitive"
probe is replaced, so both the exFAT path (exact names only) and the ext2/3/4 path (names matched
ignoring case) run on the host's own filesystem.
"""
import os
from pathlib import Path
import subprocess
import tempfile

app = Path(__file__).resolve().parents[1]
src = (app / "art.c").read_text()
a = src.index("static const char *ART_COVER_NAMES[] = {")
b = src.index("/* Render cover/thumb/backdrop for a track.")
body = src[a:b]
probe_a = body.index("static int art_dir_case_sensitive(const char *dir){")
probe_b = body.index("}\n", body.index("return statfs", probe_a)) + 2
body = body[:probe_a] + "static int test_case_sensitive;\nstatic int art_dir_case_sensitive(const char *dir){ (void)dir; return test_case_sensitive; }\n" + body[probe_b:]

harness = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <assert.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statfs.h>
void art_sidecar_signature(const char *track, uint64_t *h);
''' + body + r'''
static void touch(const char *dir, const char *name, const char *data){
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); assert(f); fputs(data, f); fclose(f);
}
static void rm(const char *dir, const char *name){
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name); assert(unlink(p) == 0);
}
static uint64_t sig(const char *track){ uint64_t h = 1469598103934665603ULL; art_sidecar_signature(track, &h); return h; }
int main(int argc, char **argv){
    assert(argc == 2);
    const char *dir = argv[1];
    char track[1024]; snprintf(track, sizeof track, "%s/01 - song.flac", dir);
    art_cover_t cv[ART_COVER_MAX]; char path[1024];
    uint64_t none = sig(track);

    /* exFAT (case-insensitive card): exact names only, no directory read */
    test_case_sensitive = 0;
    touch(dir, "COVER.JPG", "x");
    assert(art_cover_find(track, cv) == 0);
    puts("PASS exFAT path does not read the directory");

    /* ext4: COVER.JPG is found under its real name */
    test_case_sensitive = 1;
    assert(art_cover_find(track, cv) == 1 && cv[0].rank == 0 && !strcmp(cv[0].name, "COVER.JPG"));
    assert(art_cover_path(track, &cv[0], path, sizeof path));
    { char want[1024]; snprintf(want, sizeof want, "%s/COVER.JPG", dir); assert(!strcmp(path, want)); }
    uint64_t upper = sig(track); assert(upper != none);
    puts("PASS ext4 path finds COVER.JPG ignoring case");

    /* ranks follow ART_COVER_NAMES order, whatever the readdir order */
    touch(dir, "Folder.PNG", "yy");
    assert(art_cover_find(track, cv) == 2 && cv[0].rank == 0 && cv[1].rank == 5 && !strcmp(cv[1].name, "Folder.PNG"));
    puts("PASS several case-insensitive matches keep the documented precedence");

    /* an exact name wins and skips the directory read; the signature changes with the set */
    touch(dir, "folder.jpg", "zzz");
    assert(art_cover_find(track, cv) == 1 && cv[0].rank == 1 && !strcmp(cv[0].name, "folder.jpg"));
    assert(sig(track) != upper);
    rm(dir, "folder.jpg");
    assert(sig(track) != upper);   /* Folder.PNG is still part of the set */
    rm(dir, "Folder.PNG");
    assert(sig(track) == upper);
    puts("PASS exact names first; signature follows added and removed covers");

    /* a directory with the cover name is not a cover */
    rm(dir, "COVER.JPG");
    { char p[1024]; snprintf(p, sizeof p, "%s/Cover.JPEG", dir); assert(mkdir(p, 0755) == 0); }
    assert(art_cover_find(track, cv) == 0 && sig(track) == none);
    puts("PASS directories are never covers");

    /* no slash / overlong folder: nothing */
    assert(art_cover_find("song.flac", cv) == 0);
    puts("HARNESS COMPLETE");
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix="diskos-art-cover-") as tmp:
    root = Path(tmp)
    (root / "album").mkdir()
    c = root / "test.c"
    c.write_text(harness)
    exe = root / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-g",
                    str(c), "-o", str(exe)], check=True)
    r = subprocess.run([str(exe), str(root / "album")], capture_output=True, text=True, timeout=60)
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="")
    lines = r.stdout.strip().splitlines()
    if r.returncode != 0 or not lines or lines[-1] != "HARNESS COMPLETE":
        raise SystemExit("FAIL art_cover_test")
print("COMPLETE art_cover_test.py")
