#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bluetooth names people can tell apart: a local name per device (btname_<12 hex> in diskos.conf, nothing sent to the
device), the device's own name otherwise, and " (EE:FF)" on any name two listed devices would both show. Production
functions from bt.c against a fake config store."""
import subprocess, tempfile
from pathlib import Path
ui = Path(__file__).resolve().parents[1]; src = (ui / "bt.c").read_text()
def fn(sig):
    start = src.index(sig)
    while src[start:src.index("\n", start)].split("/*")[0].rstrip().endswith(";"): start = src.index(sig, start + 1)
    o = src.index("{", start); d = 1; e = o + 1
    while d: d += (src[e] == "{") - (src[e] == "}"); e += 1
    return src[start:e]
def define(name):
    line = next(l for l in src.splitlines() if l.startswith("#define " + name + " "))
    return line.split("/*")[0]
C = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
''' + define("BT_ALIAS_MAX") + "\n" + define("BT_SHOWN_CAP") + r'''
typedef struct { char mac[20]; char name[128]; int connected; char codec[16]; } bt_scan_dev_t;
static char keys[16][32], vals[16][48]; static int nk, fail_save;
static const char *cfg_get_str(const char *k, const char *d){ for(int i = 0; i < nk; i++) if(!strcmp(keys[i], k)) return vals[i]; return d; }
static int cfg_set_str(const char *k, const char *v){ if(fail_save || strlen(v) >= 48) return -1;
    for(int i = 0; i < nk; i++) if(!strcmp(keys[i], k)){ snprintf(vals[i], 48, "%s", v); return 0; }
    snprintf(keys[nk], 32, "%s", k); snprintf(vals[nk++], 48, "%s", v); return 0; }
static int cfg_remove_if(int (*m)(const char *)){ if(fail_save) return -1; int w = 0;
    for(int i = 0; i < nk; i++) if(!m(keys[i])){ if(w != i){ memcpy(keys[w], keys[i], 32); memcpy(vals[w], vals[i], 48); } w++; }
    nk = w; return 0; }
''' + "\n".join(fn(s) for s in (
    "static int bt_mac_valid(const char *mac)",
    "static void bt_mac_cfg_key(",
    "static void bt_alias_get(",
)) + "\nstatic char g_alias_drop[32];\n" + "\n".join(fn(s) for s in (
    "static int bt_alias_drop_match(",
    "static int bt_alias_set(",
    "static void bt_shown_names(",
)) + r'''
static void dev(bt_scan_dev_t *d, const char *mac, const char *name){ memset(d, 0, sizeof *d); snprintf(d->mac, 20, "%s", mac); snprintf(d->name, 128, "%s", name); }
int main(void){
    bt_scan_dev_t d[4]; char out[4][BT_SHOWN_CAP];
    dev(&d[0], "00:11:22:33:A1:B2", "BT Receiver"); dev(&d[1], "00:11:22:33:C3:D4", "BT Receiver"); dev(&d[2], "AA:BB:CC:DD:EE:FF", "Speaker");
    bt_shown_names(d, 3, out);                                  /* twins: the address end tells them apart */
    assert(!strcmp(out[0], "BT Receiver (A1:B2)")); assert(!strcmp(out[1], "BT Receiver (C3:D4)")); assert(!strcmp(out[2], "Speaker"));
    assert(bt_alias_set("00:11:22:33:A1:B2", "Kitchen") == 0);  /* one renamed: both read plainly */
    assert(!strcmp(cfg_get_str("btname_00112233A1B2", ""), "Kitchen"));
    bt_shown_names(d, 3, out);
    assert(!strcmp(out[0], "Kitchen")); assert(!strcmp(out[1], "BT Receiver")); assert(!strcmp(out[2], "Speaker"));
    assert(bt_alias_set("AA:BB:CC:DD:EE:FF", "Kitchen") == 0);  /* a local name that matches another: suffix again */
    bt_shown_names(d, 3, out);
    assert(!strcmp(out[0], "Kitchen (A1:B2)")); assert(!strcmp(out[2], "Kitchen (EE:FF)"));
    assert(bt_alias_set("AA:BB:CC:DD:EE:FF", "") == 0);        /* "" removes the key, not an empty entry */
    assert(nk == 1 && !strcmp(cfg_get_str("btname_AABBCCDDEEFF", "none"), "none"));
    char longname[64]; memset(longname, 'x', BT_ALIAS_MAX + 1); longname[BT_ALIAS_MAX + 1] = 0;
    assert(bt_alias_set("AA:BB:CC:DD:EE:FF", longname) == -1); /* never stored cut */
    longname[BT_ALIAS_MAX] = 0; assert(bt_alias_set("AA:BB:CC:DD:EE:FF", longname) == 0);
    assert(bt_alias_set("not-a-mac", "x") == -1);
    fail_save = 1; assert(bt_alias_set("00:11:22:33:C3:D4", "Desk") == -1); fail_save = 0;
    char big[128]; memset(big, 'n', 127); big[127] = 0;         /* the longest device name still gets its suffix */
    dev(&d[0], "00:11:22:33:A1:B2", big); dev(&d[1], "00:11:22:33:C3:D4", big); nk = 0;
    bt_shown_names(d, 2, out);
    assert(strlen(out[0]) == 127 + 8 && !strcmp(out[0] + 127, " (A1:B2)"));
    bt_shown_names(d, 1, out); assert(!strcmp(out[0], big));    /* alone: no suffix */
    puts("bt names: ok"); return 0;
}
'''
with tempfile.TemporaryDirectory() as t:
    c = Path(t) / "t.c"; c.write_text(C); exe = Path(t) / "t"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-O1", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
