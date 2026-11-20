#!/usr/bin/env python3
"""Built-in artwork decoder (tools/diskos_artdec.c, ARTDEC_INPROC): over-budget input, cancellation, cleanup after a
failure and two clients at once (roulette_decoder_test.c). Writes its two fixture images (plain BMP, stdlib only) and
builds the C test natively under ASan/UBSan; no device access."""
import struct, subprocess, tempfile
from pathlib import Path

tests = Path(__file__).resolve().parent


def bmp(path, w, h, rgb):
    row = bytes((rgb[2], rgb[1], rgb[0])) * w
    row += b'\0' * ((4 - len(row) % 4) % 4)
    data = row * h
    head = struct.pack('<2sIHHI', b'BM', 54 + len(data), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    path.write_bytes(head + info + data)


with tempfile.TemporaryDirectory() as tmp:
    t = Path(tmp)
    bmp(t / 'small.bmp', 300, 300, (209, 57, 12))     # the test checks this colour in the decoded BGRA output
    bmp(t / 'large.bmp', 1500, 1500, (209, 57, 12))   # 9 MB decoded: over the test's 4 MB budget
    (t / 'out').mkdir()
    subprocess.run(['cc', '-g', '-O1', '-Wall', '-Wextra', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                    '-D_GNU_SOURCE', '-I' + str(tests.parent), str(tests / 'roulette_decoder_test.c'),
                    '-lm', '-lpthread', '-o', str(t / 'test')], check=True)
    subprocess.run([str(t / 'test'), str(t / 'small.bmp'), str(t / 'large.bmp'), str(t / 'out')], check=True,
                   timeout=120)
