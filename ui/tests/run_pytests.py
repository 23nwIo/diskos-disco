#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run every ui/tests/*_test.py, one after another, and print one line per test plus a summary.

Usually started through `make -C ui/tests pytests` (which first builds host-render, sendspin-host and mpd_test, the
binaries several of these tests use). Each test's whole output goes to a log file; the summary names it for every
test that did not pass. Exit status 0 only when every test passed, was skipped, or is a known failure.

  python3 ui/tests/run_pytests.py                 all tests
  python3 ui/tests/run_pytests.py queue history   only tests whose name contains one of the words

Results: PASS (exit 0), SKIP (exit 0 with a line starting with "SKIP", e.g. an optional Python module is missing),
FAIL, TIMEOUT, KNOWN (fails, listed in KNOWN_FAILURES with the reason) and FIXED (listed there but now passes: take it
off the list). The tests share /tmp/sdcard, ui/tests/host_build and a few other paths, so they never run in parallel.
"""
import os, subprocess, sys, time
from pathlib import Path

TESTS = Path(__file__).resolve().parent
LOGS = Path(os.environ.get('DISKOS_TEST_LOGS', '/tmp/diskos-tests'))
TIMEOUT = int(os.environ.get('DISKOS_TEST_TIMEOUT', '900'))   # seconds per test; braun2_test takes ~2 min

# Tests that fail for a reason that is understood but not fixed yet. Each entry says why; remove it when fixed.
# A listed test that passes is reported as FIXED and fails the run, so this list cannot go stale.
KNOWN_FAILURES = {
    # the first five already failed in upstream 3.4.1 (1cb1828), the version this fork started from
    'display_pass_test': 'host-render REVIEW_DISPLAY: no "Navigation" heading on the Display pages (upstream 3.4.1)',
    'render_test': 'host-render immersive: no 364 px sharp cover image found (upstream 3.4.1)',
    'review_ui_test': 'host-render preview-home does not end on Home (upstream 3.4.1)',
    'theme_efficiency_test': 'pixel baseline fixtures/theme-efficiency-baseline.json older than the rendering (upstream 3.4.1)',
    'zune_scroll_test': 'pixel baseline fixtures/zune-scroll-3.0.0.txt is from 3.0.0 (upstream 3.4.1)',
    'settings_alignment_test': 'Display rows changed (upstream 3.4.1); the Outputs page is listed without the own player and '
                               'gained Headphone Volume (bb8c0d2, 06b4c07)',
}

# Extra arguments some tests need
ARGS = {
    'sendspin_interop_test': [str(TESTS / 'host_build/sendspin')],   # make -C ui CROSS= sendspin-host
}


def run(path):
    name = path.stem
    log = LOGS / f'{name}.log'
    start = time.monotonic()
    try:
        with open(log, 'w') as out:
            r = subprocess.run([sys.executable, str(path), *ARGS.get(name, [])], stdout=out, stderr=subprocess.STDOUT,
                               stdin=subprocess.DEVNULL, timeout=TIMEOUT, cwd=TESTS.parents[1])
        rc = r.returncode
    except subprocess.TimeoutExpired:
        rc = None
    secs = time.monotonic() - start
    text = log.read_text(errors='replace')
    if rc is None:
        status = 'TIMEOUT'
    elif rc == 0:
        status = 'SKIP' if any(l.startswith('SKIP') for l in text.splitlines()) else 'PASS'
    else:
        status = 'FAIL'
    if name in KNOWN_FAILURES:
        status = 'FIXED' if status in ('PASS', 'SKIP') else 'KNOWN'
    return name, status, secs, log, text


def main(words):
    LOGS.mkdir(parents=True, exist_ok=True)
    tests = sorted(TESTS.glob('*_test.py'))
    if words:
        tests = [t for t in tests if any(w in t.stem for w in words)]
        if not tests:
            sys.exit(f'no test matches {" ".join(words)}')
    results = []
    for t in tests:
        name, status, secs, log, text = run(t)
        results.append((name, status, log, text))
        note = ''
        if status == 'SKIP':
            note = next(l for l in text.splitlines() if l.startswith('SKIP'))
        elif status == 'KNOWN':
            note = KNOWN_FAILURES[name]
        elif status == 'FIXED':
            note = 'passes now: remove it from KNOWN_FAILURES in run_pytests.py'
        print(f'{status:7} {name:36} {secs:6.1f} s  {note}', flush=True)
    bad = [r for r in results if r[1] in ('FAIL', 'TIMEOUT', 'FIXED')]
    for name, status, log, text in bad:
        if status == 'FIXED':
            continue
        print(f'\n===== {name}: {status} (whole output: {log})')
        print('\n'.join(text.splitlines()[-25:]))
    counts = {}
    for r in results:
        counts[r[1]] = counts.get(r[1], 0) + 1
    print('\n' + ', '.join(f'{n} {s}' for s, n in sorted(counts.items())) + f'  (logs in {LOGS})')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main(sys.argv[1:])
