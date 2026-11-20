#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Now Playing text stays readable over any cover, in every theme, light and dark.

Renders Now Playing (preview-np) with a black, a white, an orange and a busy multicolour cover in each theme preset
and variant. host-render's REVIEW_NP_CONTRAST mode measures the title, artist and album: for each label it compares
the label's text colour with the pixels drawn behind it and prints the 10th percentile of the WCAG contrast ratio.
Every value must reach 4.5 (WCAG AA for normal text). The cover backdrop is washed with the canvas colour by
np_backdrop_wash() in ui.c; this test is what keeps that wash strong enough.

Needs host-render: make -C ui CROSS= host-render
"""
import os, struct, subprocess, sys, tempfile, zlib
from pathlib import Path

EXE = Path(__file__).resolve().parent / 'host_build/host-render'
PRESETS = (6, 8, 9, 10, 11, 12, 13, 14)   # Stone, Ring, Braun, Disco, Null, Squash, Zune, Braun 2.0
MINIMUM = 4.5


def png(w, h, pixel):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in pixel(x, y)) for y in range(h))
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


COVERS = {
    'black': lambda x, y: (0, 0, 0),
    'white': lambda x, y: (255, 255, 255),
    'orange': lambda x, y: (209, 57, 12),
    'busy': lambda x, y: ((x * 8) % 256, (y * 8) % 256, ((x + y) * 4) % 256) if (x // 8 + y // 8) % 2 else (250, 240, 40),
}


def main():
    if not EXE.exists():
        sys.exit(f'{EXE} missing: make -C ui CROSS= host-render')
    bad, runs = [], 0
    with tempfile.TemporaryDirectory(prefix='np-contrast-') as tmp:
        for name, pixel in COVERS.items():
            (Path(tmp) / f'{name}.png').write_bytes(png(64, 64, pixel))   # host-render reads the cover next to the frame
        for preset in PRESETS:
            for variant in (0, 1):
                for name in COVERS:
                    r = subprocess.run([str(EXE), str(preset), str(variant), 'preview-np', '0', str(Path(tmp) / f'{name}.ppm')],
                                       env=dict(os.environ, REVIEW_NP_CONTRAST='1'), capture_output=True, text=True)
                    if r.returncode:
                        sys.exit(f'host-render {preset} {variant} {name} failed:\n{r.stderr[-2000:]}')
                    found = 0
                    for line in r.stdout.splitlines():
                        f = line.split()
                        if not f or f[0] != 'CONTRAST' or f[2] == 'none':
                            continue
                        found += 1
                        p10 = float(f[f.index('p10') + 1])
                        ok = p10 >= MINIMUM
                        print(f"{'ok  ' if ok else 'LOW '} preset {preset} variant {variant} {name:6} {line}")
                        if not ok:
                            bad.append(f'preset {preset} variant {variant} cover {name}: {f[1]} {p10}')
                    if not found:
                        bad.append(f'preset {preset} variant {variant} cover {name}: no title, artist or album found')
                    runs += 1
    if bad:
        print('\nFAIL: contrast below %.1f:\n  ' % MINIMUM + '\n  '.join(bad))
        sys.exit(1)
    print(f'PASS: Now Playing text contrast >= {MINIMUM} in {runs} renders ({len(PRESETS)} presets x 2 variants x {len(COVERS)} covers)')


if __name__ == '__main__':
    main()
