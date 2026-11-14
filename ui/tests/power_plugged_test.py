#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plugged-in detection for idle power-off (#17), production usage_plugged_in() on fake sysfs trees:
- any non-battery power supply with online=1 counts, whatever its name; the battery doesn't;
- the USB-C controller's cc_term (Rdef/R1p5/R3p0 = a source supplies power) counts even with no charge current
  (device-probed 2026-11-14: wall charger + Charging Limit at 80 % -> Rdef, unplugged -> Undefined);
- cc_term "Undefined" overrides the remembered-charger guess; Rd/Ra or no node -> the older guesses decide."""
import subprocess, tempfile
from pathlib import Path
ui = Path(__file__).resolve().parents[1]; src = (ui / "usage.c").read_text()
start = src.index("static int charger_remembered(void){")
end = src.index("\n}\n", src.index("static int plugged_listed(void){\n")) + 3
section = src[start:end]
# (name, power_supply tree, cc_term or None, charge history [(pct, charging)...], usb host configured, expected)
cases = [
    ("unplugged",                {"cw221X-bat": ("Battery", "1"), "usb": ("USB", "0")}, None, [], 0, 0),
    ("plugged into USB",         {"cw221X-bat": ("Battery", "1"), "usb": ("USB", "1")}, None, [], 0, 1),
    ("full, still on a charger", {"cw221X-bat": ("Battery", "1"), "ac": ("Mains", "1"), "usb": ("USB", "0")}, None, [], 0, 1),
    ("other name (FiiO's own)",  {"cw221X-bat": ("Battery", "1"), "fiio-charger": ("USB", "1")}, None, [], 0, 1),
    ("no charger listed",        {"cw221X-bat": ("Battery", "1")}, None, [], 0, -1),
    ("Disc: 80% held, Rdef",     {"cw221X-bat": ("Mains", "")}, "Rdef", [(80, 0)], 0, 1),
    ("Disc: 3 A charger",        {"cw221X-bat": ("Mains", "")}, "R3p0\n", [(60, 0)], 0, 1),
    ("Disc: 1.5 A charger",      {"cw221X-bat": ("Mains", "")}, "R1p5", [], 0, 1),
    ("Disc: unplugged",          {"cw221X-bat": ("Mains", "")}, "Undefined\n", [(81, 0)], 0, 0),
    ("Disc: unplugged after charging", {"cw221X-bat": ("Mains", "")}, "Undefined", [(80, 1), (80, 0)], 0, 0),
    ("Disc: OTG host (Rd)",      {"cw221X-bat": ("Mains", "")}, "Rd", [], 0, -1),
    ("Disc: no node, charging seen", {"cw221X-bat": ("Mains", "")}, None, [(80, 1), (80, 0)], 0, 1),
    ("Disc: Undefined but host configured", {"cw221X-bat": ("Mains", "")}, "Undefined", [], 1, 1),
]
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    for i, (name, tree, cc, hist, host, want) in enumerate(cases):
        root = d / ("c%d" % i); ps = root / "ps"; udc = root / "udc" / "x"; udc.mkdir(parents=True)
        (udc / "state").write_text("configured\n" if host else "not attached\n")
        for sup, (typ, online) in tree.items():
            (ps / sup).mkdir(parents=True); (ps / sup / "type").write_text(typ + "\n")
            if online: (ps / sup / "online").write_text(online + "\n")
        cct = root / "cc_term"
        if cc is not None: cct.write_text(cc)
        feed = "".join("g_batt=%d; g_chg=%d; (void)charger_remembered();\n" % h for h in hist)
        C = ('#include <dirent.h>\n#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n'
             'static uint32_t lv_tick_get(void){ return 1000; }\nstatic int g_batt = -1, g_chg;\n'
             '#define POWER_SUPPLY_DIR "%s"\n#define TYPEC_CC_TERM "%s"\n' % (ps, cct))
        C += section.replace('"/sys/class/udc"', '"%s"' % (root / "udc")).replace('"/sys/class/udc/%.60s/state"', '"%s/%%.60s/state"' % (root / "udc"))
        C += '\nint main(void){\n' + feed + ' printf("%d\\n", usage_plugged_in()); return 0; }\n'
        (d / "t.c").write_text(C)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function", "-o", str(d / "t"), str(d / "t.c")], check=True)
        got = int(subprocess.run([str(d / "t")], capture_output=True, text=True, check=True).stdout)
        assert got == want, (name, got, want)
        print("ok  %-38s -> %d" % (name, got))
print("PASS plugged-in detection: power supplies, USB-C cc_term (Rdef/R1p5/R3p0 in, Undefined out), history fallback")
