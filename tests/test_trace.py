"""Host tests for ui/trace (diskos-trace.so, the LD_PRELOAD recorder for the stock player).

The library is built for the host glibc with a temporary card path and preloaded into a small program named mq_player
that does what the stock player does (device opens, ioctls, sysfs writes, shell commands). The card is a tmpfs mounted
inside the test folder, so these tests need root; without it they are skipped."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / 'ui' / 'trace' / 'trace.c'

PROG = r'''
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#define REQ_W 0x40014d29UL   /* _IOW('M', 41, unsigned char) on the host */
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "check failed line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
    unsigned char v = 0x5a;
    int fd = open("/dev/full", O_RDWR);   /* /dev/null is on the quiet list */
    CHECK(fd >= 0);
    errno = 0;
    CHECK(ioctl(fd, REQ_W, &v) == -1 && errno == ENOTTY);           /* errno passes through unchanged */
    for (int i = 0; i < 10; i++) ioctl(fd, REQ_W, &v);              /* rate limited */
    CHECK(write(fd, "hello\n", 6) == -1 && errno == ENOSPC);
    CHECK(open("/dev/does-not-exist", O_RDONLY) == -1 && errno == ENOENT);
    for (int i = 0; i < 2; i++) { FILE *s = fopen("/proc/self/status", "r"); CHECK(s); fclose(s); }
    FILE *w = fopen("/dev/full", "w");
    CHECK(w);
    fprintf(w, "vol=%d", 7);
    fputs("x", w);
    fclose(w);
    CHECK(system("exit 3") != 0);
    for (int i = 0; i < 5; i++) { FILE *p = popen("cmd_watchdog feed 2>/dev/null", "r"); if (p) pclose(p); }
    unsigned char g[16]; for (int i = 0; i < 16; i++) g[i] = (unsigned char)(i + 1);
    ioctl(fd, 0x477aUL, g);                                         /* _IO with a struct pointer, like /dev/gpio */
    FILE *h = popen("cat /proc/self/stat /dev/null >/dev/null", "r"); if (h) pclose(h);
    pid_t c = fork();
    if (c == 0) { unsigned char k = 0x11; ioctl(fd, REQ_W, &k); execl("/bin/true", "true", "child-arg", (char *)0); _exit(1); }
    waitpid(c, NULL, 0);
    close(fd);
    return 0;
}
'''


@unittest.skipUnless(shutil.which('gcc') and os.geteuid() == 0, 'needs gcc and root (tmpfs card)')
class TraceTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.card = self.tmp / 'sd'
        self.card.mkdir()
        self.marker = self.tmp / 'ok'
        self.lib = self.tmp / 'diskos-trace.so'
        subprocess.run(['gcc', '-O1', '-shared', '-fPIC', '-Wall', '-Wextra', '-Werror', '-fno-builtin',
                        f'-DTRACE_SD_ROOT="{self.card}"', f'-DTRACE_SD_PARENT="{self.tmp}"',
                        f'-DTRACE_OK_MARKER="{self.marker}"', '-o', str(self.lib), str(SRC), '-ldl'], check=True)
        (self.tmp / 'prog.c').write_text(PROG)
        self.prog = self.tmp / 'mq_player'
        subprocess.run(['gcc', '-O1', '-fno-builtin', '-U_FORTIFY_SOURCE', '-D_FORTIFY_SOURCE=0', '-o', str(self.prog), str(self.tmp / 'prog.c')], check=True)
        self.mounted = False

    def tearDown(self):
        if self.mounted:
            subprocess.run(['umount', str(self.card)], check=False)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def mount(self):
        subprocess.run(['mount', '-t', 'tmpfs', 'tmpfs', str(self.card)], check=True)
        self.mounted = True

    def run_prog(self):
        env = dict(os.environ, LD_PRELOAD=str(self.lib))
        return subprocess.run([str(self.prog)], env=env, capture_output=True, text=True, timeout=30)

    def log(self):
        return (self.card / 'diskos-trace' / 'player-trace.log').read_text()

    def test_records_player_activity(self):
        self.mount()
        r = self.run_prog()
        self.assertEqual(r.returncode, 0, r.stderr)
        log = self.log()
        lines = log.splitlines()
        self.assertTrue(any('=== trace start' in l and 'mq_player' in l for l in lines))
        self.assertTrue(any(' map ' in l for l in lines))
        self.assertTrue(self.marker.exists())
        self.assertIn('open "/dev/full"', log)
        self.assertNotIn('"/dev/null"', log)
        ioctls = [l for l in lines if 'ioctl' in l and '0x40014d29' in l and ' in 5a' in l]
        self.assertEqual(len(ioctls), 4, log)                       # 11 identical calls, 4 per 10 s pass
        self.assertIn("_IOW('M',41,sz1)", ioctls[0])
        self.assertIn('-> -1 errno 25', ioctls[0])
        self.assertEqual(log.count('write "/dev/full" "hello\\n" -> -1 errno 28'), 1, log)
        self.assertIn('open "/dev/does-not-exist"', log)
        self.assertEqual(log.count('"/proc/self/status" "r" (first read)'), 1, log)
        self.assertIn('fprintf "/dev/full" "vol=7"', log)
        self.assertIn('fputs "/dev/full" "x"', log)
        self.assertIn('system "exit 3"', log)
        self.assertIn('status 0x300', log)
        self.assertEqual(log.count('popen "cmd_watchdog feed'), 3, log)
        self.assertIn('(further feeds not recorded)', log)
        child = [l for l in lines if ' in 11 ' in l]  # a forked child that execs: its lines are written at the exec
        self.assertEqual(len(child), 1, log)
        child_pid = child[0].split()[1]
        parent_pid = ioctls[0].split()[1]
        self.assertNotEqual(child_pid, parent_pid)
        # the child must not write the parent's buffered lines a second time
        child_events = [l.split(' ', 3)[3] for l in lines if l.split()[1:2] == [child_pid]]
        self.assertEqual(child_events[1], 'execl "/bin/true" "child-arg"', log)
        self.assertEqual(len(child_events), 2, log)   # the new program loads us again but records nothing worth a line
        mem = [l for l in lines if "0x0000477a" in l]
        self.assertEqual(len(mem), 1, log)
        self.assertIn(' mem 0102030405060708090a0b0c0d0e0f10', mem[0])
        self.assertNotIn('/proc/self/stat"', log)                  # /proc in a helper program
        self.assertIn('close 3 "/dev/full"', log)

    def test_no_card_no_file_and_no_harm(self):
        r = self.run_prog()                                         # card folder exists but nothing is mounted
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse((self.card / 'diskos-trace').exists())

    def test_appends_across_runs(self):
        self.mount()
        self.assertEqual(self.run_prog().returncode, 0)
        self.assertEqual(self.run_prog().returncode, 0)
        self.assertEqual(self.log().count(f'=== trace start "{self.prog}"'), 2)


if __name__ == '__main__':
    unittest.main()
