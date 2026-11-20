#!/usr/bin/env python3
"""Check exact Zune scrolling pixels against a recorded run, settled work, and fitted-font invalidation.
Build first with `make -C ui CROSS= host-render`; no player/device is used. After an INTENDED visual change, look at the
changed screens, then re-record with --record-baseline (see docs/TESTING.md).
"""
from pathlib import Path
import os, subprocess, sys, tempfile
from artdec_test import png
app = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='zune-scroll-') as tmp:
    art = Path(tmp)/'track.png'; art.write_bytes(png(64,64,(209,57,12)))
    result = subprocess.run([str(app/'tests/host_build/host-render'),'13','0','preview-home','0',str(Path(tmp)/'out.ppm')],
        env={**os.environ,'ZNP_PATH':str(art),'REVIEW_ZUNE_SCROLL_PERF':'1','CFG_INT':'disco_hints=2'},text=True,capture_output=True,timeout=45)
    assert result.returncode == 0, result.stdout+result.stderr
    frames = [s for s in result.stdout.splitlines() if s.startswith('FRAME')]
    fixture = app/'tests/fixtures/zune-scroll-baseline.txt'
    if '--record-baseline' in sys.argv[1:]:
        fixture.write_text('\n'.join(frames)+'\n')
    baseline = fixture.read_text().splitlines()
    assert frames == baseline, '\n'.join(f'{a}\n{b}' for a,b in zip(baseline,frames) if a!=b)
    settled = [s for s in result.stdout.splitlines() if s.startswith('SETTLED')]
    assert len(settled)==2 and all('styles=0 ' in s for s in settled)
    print(f'PASS: {len(frames)} pixel-identical scroll frames, both rounding rules, zero settled style events, text/subline cache invalidation')
