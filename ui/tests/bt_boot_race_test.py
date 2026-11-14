#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Boot Bluetooth bring-up must not race the stock player (field report 2026-11-14: mq_player SIGSEGV in libdbus
when hci0 came up, then the stock watchdog started the stock UI next to diskOS).

Production bt_enable() with g_bt_enable_boot = 1, its script run against stand-ins:
  - no bluetoothd/bluealsa is killed at boot;
  - a bluetoothd the player already started is used: ours is not started (BlueALSA likewise);
  - with no other bluetoothd, ours still starts (after the wait), so Bluetooth comes up either way.
A user toggle (boot = 0) keeps the full restart: bt_power_test.py."""
import os, subprocess, tempfile
from pathlib import Path

ui = Path(__file__).resolve().parents[1]
src = (ui / "bt.c").read_text()

def fn(sig):
    start = src.index(sig)
    while src[start:src.index("\n", start)].split("/*")[0].rstrip().endswith(";"):
        start = src.index(sig, start + 1)
    opening = src.index("{", start); depth = 1; end = opening + 1
    while depth:
        depth += (src[end] == "{") - (src[end] == "}"); end += 1
    return src[start:end]

C = r'''
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define BT_ENABLE_MAX_S 60
static char script[4096];
static int fake_system(const char *cmd){ snprintf(script, sizeof script, "%s", cmd); return 0; }
#define system fake_system
static int g_bt_enable_boot;
static const char *cfg_get_str(const char *k, const char *d){ (void)k; return d; }
static int bt_born_for(const char *mac){ (void)mac; return 0; }
static int bt_kind_for(const char *mac){ (void)mac; return 0; }
static void bt_alsa_cmd(char *out, int cap, int born, int kind){ (void)born; (void)kind; snprintf(out, cap, "bluealsa -S"); }
''' + fn("static int bt_enabling(") + "\n" + fn("static void bt_enable(") + r'''
int main(int argc, char **argv){
    unlink("/tmp/bt_enabling"); unlink("/tmp/bt_disabling");
    g_bt_enable_boot = 1; bt_enable();
    if(g_bt_enable_boot){ fprintf(stderr, "boot flag not consumed\n"); return 1; }
    FILE *f = fopen(argv[1], "w"); fputs(script, f); fputs("\nwait\n", f); fclose(f);
    return 0;
}
'''

STUBS = {
    "killall": 'echo "killall $*" >> "$LOG"',
    "rfkill": 'echo "rfkill $*" >> "$LOG"',
    "hciconfig": '[ "$#" -eq 1 ] && echo "UP RUNNING"; exit 0',
    "brcm_patchram_plus": 'echo "patchram" >> "$LOG"',
    "bluealsa": 'echo "bluealsa started" >> "$LOG"',
    "bluetoothctl": ':',
    "pidof": '[ -n "$PLAYER_DAEMONS" ] && echo 123 && exit 0; exit 1',   # the player's bluetoothd/bluealsa, or none
    "sleep": 'exec /bin/sleep 0.01',
}

with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / "t.c").write_text(C)
    subprocess.run(["cc", "-std=gnu11", "-o", str(d / "t"), str(d / "t.c")], check=True)
    subprocess.run([str(d / "t"), str(d / "s.sh")], check=True)
    bin_ = d / "bin"; bin_.mkdir()
    for name, body in STUBS.items():
        (bin_ / name).write_text("#!/bin/sh\n" + body + "\n"); (bin_ / name).chmod(0o755)
    Path("/usr/project").mkdir(parents=True, exist_ok=True)
    btd = Path("/usr/project/bluetoothd"); had = btd.exists()
    if not had:
        btd.write_text('#!/bin/sh\necho "bluetoothd started" >> "$LOG"\n'); btd.chmod(0o755)
    try:
        for player, want_ours in (("1", False), ("", True)):
            log = d / "log"; log.write_text("")
            Path("/tmp/bt_enabling").write_text(open(d / "s.sh").read().split('= ')[1].split(' ]')[0] + "\n")
            env = dict(os.environ, PATH=str(bin_) + ":" + os.environ["PATH"], LOG=str(log), PLAYER_DAEMONS=player)
            subprocess.run(["sh", str(d / "s.sh")], env=env, check=False, timeout=30)
            lines = log.read_text().splitlines()
            kills = [l for l in lines if l.startswith("killall")]
            assert kills and all("bluetoothd" not in l and "bluealsa" not in l for l in kills), "boot killed a daemon:\n" + "\n".join(lines)
            assert "patchram" in lines, "\n".join(lines)
            ours = [l for l in lines if l in ("bluetoothd started", "bluealsa started")]
            assert bool(ours) == want_ours, ("player daemons" if player else "no player daemons", lines)
            print("ok  %-24s -> ours started: %s" % ("player daemons running" if player else "none running", bool(ours)))
    finally:
        if not had: btd.unlink()
        for p in ("/tmp/bt_enabling", "/tmp/bt_disabling"):
            try: os.unlink(p)
            except FileNotFoundError: pass
print("PASS boot bring-up: kills no BT daemon, uses the player's bluetoothd/BlueALSA, starts its own only when none runs")
