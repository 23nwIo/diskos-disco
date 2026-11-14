"""Host tests for src/watchdog (diskos-watchdog, the cmd_watchdog replacement).

The tool is built for the host with a recording ioctl (WD_TEST_IOCTL) and a temporary device path, so every test
checks the exact request codes the stock libhardware2.so uses without touching a real watchdog."""
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'src' / 'watchdog'

SHIM = r'''
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
int wd_test_ioctl(int fd, unsigned long req, void *arg)
{
    const char *log = getenv("WD_LOG"), *failreq = getenv("WD_FAIL");
    FILE *f;
    (void)fd;
    if (log && (f = fopen(log, "a"))) {
        fprintf(f, "%08lx %d\n", req, arg ? *(int *)arg : -1);
        fclose(f);
    }
    if (failreq && strtoul(failreq, NULL, 16) == req) {
        errno = getenv("WD_ERRNO") ? atoi(getenv("WD_ERRNO")) : EIO;
        return -1;
    }
    return 0;
}
'''

START, STOP, FEED, RESET = 0x8004570d, 0x2000570e, 0x2000570f, 0x20005710


@unittest.skipUnless(shutil.which('cc'), 'needs a host C compiler')
class WatchdogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='diskos-wd-')
        d = Path(cls.tmp.name)
        cls.dev = d / 'jz_watchdog'
        cls.dev.write_bytes(b'')
        (d / 'shim.c').write_text(SHIM)
        cls.bin = d / 'diskos-watchdog'
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', '-DWD_TEST_IOCTL', f'-DWD_DEV_PATH="{cls.dev}"',
                        '-o', str(cls.bin), str(SRC / 'main.c'), str(SRC / 'watchdog.c'), str(d / 'shim.c')],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.log = Path(self.tmp.name) / f'log-{self._testMethodName}'
        self.env = dict(os.environ, WD_LOG=str(self.log))
        self.dev.write_bytes(b'')

    def run_wd(self, *args, env=None):
        return subprocess.run([str(self.bin), *args], env=env or self.env, capture_output=True, text=True, timeout=10)

    def calls(self):
        if not self.log.exists():
            return []
        return [(int(r, 16), int(a)) for r, a in (ln.split() for ln in self.log.read_text().splitlines())]

    def test_request_codes_match_stock_library(self):
        for args, want in ((['start', '10000'], [(START, 10000)]), (['stop'], [(STOP, -1)]),
                           (['feed'], [(FEED, -1)]), (['reset'], [(RESET, -1)])):
            with self.subTest(args=args):
                self.log.unlink(missing_ok=True)
                p = self.run_wd(*args)
                self.assertEqual(p.returncode, 0, p.stderr)
                self.assertEqual(p.stdout, '')       # the stock caller reads nothing; stay silent on success
                self.assertEqual(self.calls(), want)

    def test_bad_usage_touches_nothing(self):
        for args in ([], ['start'], ['start', '0'], ['start', '-5'], ['start', '10x'], ['start', '99999999999'],
                     ['feed', 'x'], ['bogus'], ['run'], ['run', '1000', '1000'], ['-h', 'x']):
            with self.subTest(args=args):
                self.log.unlink(missing_ok=True)
                p = self.run_wd(*args)
                self.assertEqual(p.returncode, 2)
                self.assertIn('Usage', p.stderr)
                self.assertEqual(self.calls(), [])

    def test_help(self):
        for a in ('-h', '--help'):
            p = self.run_wd(a)
            self.assertEqual(p.returncode, 0)
            self.assertIn('Usage', p.stdout)

    def test_ioctl_failure_is_reported(self):
        p = self.run_wd('feed', env=dict(self.env, WD_FAIL=f'{FEED:x}'))
        self.assertEqual(p.returncode, 1)
        self.assertIn('feed failed', p.stderr)

    def test_missing_device(self):
        self.dev.unlink()
        try:
            p = self.run_wd('feed')
            self.assertEqual(p.returncode, 1)
            self.assertIn('open', p.stderr)
        finally:
            self.dev.write_bytes(b'')

    def test_run_feeds_then_stops_on_sigterm(self):
        proc = subprocess.Popen([str(self.bin), 'run', '1000', '50'], env=self.env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.4)
        proc.send_signal(signal.SIGTERM)
        out, err = proc.communicate(timeout=5)
        self.assertEqual(proc.returncode, 0, err)
        c = self.calls()
        self.assertEqual(c[0], (START, 1000))
        self.assertEqual(c[-1], (STOP, -1))
        self.assertGreaterEqual(sum(1 for r, _ in c if r == FEED), 3)
        self.assertEqual({r for r, _ in c[1:-1]}, {FEED})


    def test_run_keeps_feeding_an_already_running_watchdog(self):
        env = dict(self.env, WD_FAIL=f'{START:x}', WD_ERRNO=str(1))   # EPERM, as the Disc's kernel answers
        proc = subprocess.Popen([str(self.bin), 'run', '1000', '50'], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)
        proc.send_signal(signal.SIGTERM)
        out, err = proc.communicate(timeout=5)
        self.assertEqual(proc.returncode, 0, err)
        self.assertIn('already running', err)
        c = self.calls()
        self.assertGreaterEqual(sum(1 for r, _ in c if r == FEED), 2)
        self.assertEqual(c[-1], (STOP, -1))

    def test_run_aborts_on_other_start_errors(self):
        p = self.run_wd('run', '1000', '50', env=dict(self.env, WD_FAIL=f'{START:x}'))
        self.assertEqual(p.returncode, 1)
        self.assertNotIn(FEED, [r for r, _ in self.calls()])


if __name__ == '__main__':
    unittest.main()
