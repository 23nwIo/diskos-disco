/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 diskOS contributors */
/*
 * diskos-trace.so: an LD_PRELOAD library that records what the stock FiiO player (/usr/bin/mq_player) does to the
 * hardware, so an own player can repeat it (docs/MQ_PLAYER_TRACE.md). It is loaded by the stock player's glibc
 * dynamic loader and records:
 *   - open/open64/openat/openat64/fopen/fopen64 of paths under /dev, /sys and /proc (a read-only open of /sys or /proc
 *     only the first time per path, they are polled),
 *   - every ioctl with its request decoded and up to 64 argument bytes before (write) and after (read) the call,
 *   - write/fwrite/fputs/fputc/fprintf/vfprintf to those files,
 *   - system, popen and the exec family (the shell commands the player runs).
 * Every line carries the time since boot, the pid (and thread), and the caller's return address, which points into
 * the player's code (the start header lists the code mappings).
 *
 * Lines go to the file the launcher names in DISKOS_TRACE_FILE: on the SD card,
 * /tmp/sdcard/diskos-trace/player-trace-<start time>.log, or /tmp/diskos-trace/ (RAM) when the switch file says "tmp".
 * Lines for the card are held in memory until it is mounted. They are appended about once a second with open/append/close. Anything this library does for itself uses raw system calls, never glibc, so errno and
 * glibc's locks stay untouched. It must never take down the player: when anything goes wrong it records less, never
 * more; a hooked call always runs the real function and returns its result and errno unchanged.
 *
 * Build: built by ui/Makefile with the static musl toolchain as a self-contained shared object (-nostdlib), linked
 * only against stub libc.so.6 / libdl.so.2 so the device's glibc supplies dlsym and __errno_location at run time.
 * Nothing of musl is linked in. Host test: tests/test_trace.py builds it for the host glibc.
 */
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/syscall.h>

#if defined(SYS_clock_gettime32)
#define SC_CLOCK SYS_clock_gettime32     /* musl with 64-bit time_t names the o32 call (32-bit timespec) this way */
#else
#define SC_CLOCK SYS_clock_gettime
#endif
#ifndef EINTR
#define EINTR 4
#endif
#ifndef O_TMPFILE
#define O_TMPFILE 020200000
#endif
#ifndef TRACE_DEFAULT_FILE
#define TRACE_DEFAULT_FILE "/tmp/sdcard/diskos-trace/player-trace.log"
#endif
#ifndef TRACE_CARD_ROOT
#define TRACE_CARD_ROOT "/tmp/sdcard"       /* a log below this waits until the card is mounted here */
#endif
#ifndef TRACE_TAIL_DIR
#define TRACE_TAIL_DIR "/usr/data"          /* lines after the card went away (power off), at most TAIL_CAP per start */
#endif
#define TAIL_CAP (64u * 1024u)
#ifndef TRACE_CARD_PARENT
#define TRACE_CARD_PARENT "/tmp"
#endif
#ifndef TRACE_OK_MARKER
#define TRACE_OK_MARKER "/usr/data/diskos-trace.ok"
#endif
#define TRACE_ENV "DISKOS_TRACE_FILE"             /* set by the launcher (trace_launch.h), inherited by helpers */
#define TRACE_CAP_BYTES (16u * 1024u * 1024u)    /* /tmp is RAM: stop appending past this size */
#define BUF_BYTES (512u * 1024u)    /* per buffer, two of them; untouched pages cost no RAM */
#define DUMP_MAX 64
#define DATA_MAX 48
#define LINE_MAX_ 700
#define NFD 1024
#define FDPATH 80

/* ---- glibc symbols supplied at run time (nothing else of the C library is called) ---- */
typedef struct _IO_FILE FILE;
extern void *dlsym(void *handle, const char *name);
extern int *__errno_location(void);
#define RTLD_NEXT_ ((void *)-1l)

/* ---- raw system calls ---- */
#if defined(__mips__)
#define SC_CLOBBER "$1", "$3", "$11", "$12", "$13", "$14", "$15", "$24", "$25", "hi", "lo", "memory"
static long sc4(long n, long a, long b, long c, long d)
{
    register long r4 __asm__("$4") = a;
    register long r5 __asm__("$5") = b;
    register long r6 __asm__("$6") = c;
    register long r7 __asm__("$7") = d;
    register long r2 __asm__("$2");
    __asm__ __volatile__("addu $2,$0,%2 ; syscall"
                         : "=&r"(r2), "+r"(r7)
                         : "ir"(n), "r"(r4), "r"(r5), "r"(r6)
                         : SC_CLOBBER, "$8", "$9", "$10");
    return r7 && r2 > 0 ? -r2 : r2;
}
#else
/* host test build only: glibc's syscall(), with errno saved around it */
extern long syscall(long n, ...);
static long sc6(long n, long a, long b, long c, long d, long e, long f)
{
    int *ep = __errno_location(), se = *ep;
    long r = syscall(n, a, b, c, d, e, f);
    if (r == -1) r = -*ep;
    *ep = se;
    return r;
}
static long sc4(long n, long a, long b, long c, long d) { return sc6(n, a, b, c, d, 0, 0); }
#endif
#define sc3(n, a, b, c) sc4(n, (long)(a), (long)(b), (long)(c), 0)

/* ---- small helpers, no libc ---- */
static size_t s_len(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int s_pre(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static int s_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static uint32_t fnv(const char *s, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }

typedef struct { char *p; size_t n, cap; } out_t;
static void o_ch(out_t *o, char c) { if (o->n + 1 < o->cap) o->p[o->n++] = c; }
static void o_s(out_t *o, const char *s) { while (s && *s) o_ch(o, *s++); }
static void o_u(out_t *o, unsigned long v)
{
    char t[24]; int i = 0;
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) o_ch(o, t[--i]);
}
static void o_i(out_t *o, long v) { if (v < 0) { o_ch(o, '-'); o_u(o, (unsigned long)-v); } else o_u(o, (unsigned long)v); }
static void o_x(out_t *o, unsigned long v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    char t[20]; int i = 0;
    do { t[i++] = hx[v & 15]; v >>= 4; } while (v || i < digits);
    o_s(o, "0x");
    while (i) o_ch(o, t[--i]);
}
static void o_hex(out_t *o, const unsigned char *b, size_t n)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { o_ch(o, hx[b[i] >> 4]); o_ch(o, hx[b[i] & 15]); }
}
/* quoted, printable ASCII kept, everything else escaped, at most max input bytes */
static void o_q(out_t *o, const char *s, size_t n, size_t max)
{
    static const char hx[] = "0123456789abcdef";
    o_ch(o, '"');
    for (size_t i = 0; i < n && i < max; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\n') o_s(o, "\\n");
        else if (c == '"' || c == '\\') { o_ch(o, '\\'); o_ch(o, (char)c); }
        else if (c >= 32 && c < 127) o_ch(o, (char)c);
        else { o_s(o, "\\x"); o_ch(o, hx[c >> 4]); o_ch(o, hx[c & 15]); }
    }
    o_ch(o, '"');
    if (n > max) { o_s(o, "+"); o_u(o, n - max); }
}
static void o_qs(out_t *o, const char *s) { if (!s) o_s(o, "(null)"); else o_q(o, s, s_len(s), 200); }

/* ---- state ---- */
static int g_disabled;          /* this process records nothing (a watchdog feed child) */
static int g_main;              /* this process is the player itself */
static int g_pid;
static char g_fdpath[NFD][FDPATH];
static struct { FILE *f; char path[FDPATH]; } g_files[32];

static volatile int g_lock, g_lockpid;
static char g_bufa[BUF_BYTES], g_bufb[BUF_BYTES];
static char *g_cur = g_bufa;
static size_t g_len;
static unsigned g_dropped, g_flushing, g_bufpid, g_stopped;
static uint32_t g_last_flush;

static struct { uint32_t h, win; uint16_t n; uint32_t sup; } g_rate[256];
static uint32_t g_seen[512];    /* first-open-only table for polled read-only /sys and /proc paths */
static unsigned g_wd_feeds;

static uint32_t now_ms(void)
{
    long ts[4] = {0};            /* struct timespec on o32 is two longs; host x86_64 too */
    sc3(SC_CLOCK, 1 /* CLOCK_MONOTONIC */, ts, 0);
    return (uint32_t)ts[0] * 1000u + (uint32_t)(ts[1] / 1000000);
}
static int cur_pid(void) { return (int)sc3(SYS_getpid, 0, 0, 0); }
static int cur_tid(void) { return (int)sc3(SYS_gettid, 0, 0, 0); }

/* A spin lock that never deadlocks the player: it is not re-entered from a signal handler on the same thread, it is
 * taken over after fork (the owner thread does not exist in the child), and a waiter that cannot get it gives up and
 * drops its line. */
static int lock_take(void)
{
    int tid = cur_tid(), pid = cur_pid();
    for (int i = 0; i < 4000; i++) {
        if (__sync_bool_compare_and_swap(&g_lock, 0, tid)) { g_lockpid = pid; goto got; }
        int owner = g_lock;
        if (owner == tid) return 0;                              /* our own signal handler */
        if (owner && g_lockpid != pid && __sync_bool_compare_and_swap(&g_lock, owner, tid)) { g_lockpid = pid; goto got; }
        if ((i & 15) == 15) sc3(SYS_sched_yield, 0, 0, 0);
    }
    return 0;
got:
    if (g_bufpid != (unsigned)pid) {                             /* a forked child: the parent's lines are not ours */
        g_bufpid = (unsigned)pid; g_len = 0; g_dropped = 0; g_flushing = 0;
        g_pid = pid;
    }
    return 1;
}
static void lock_drop(void) { __sync_lock_release(&g_lock); }

/* ---- the log file: on the card (default) or in /tmp (RAM) ---- */
static char g_file[200], g_dir[200];   /* set by the constructor; until then lines stay in memory */
static int g_on_card, g_card_seen;
static char g_tail[200];               /* TRACE_TAIL_DIR/diskos-trace-tail-<start time>.log */
static long fs_type(const char *p)
{
    long b[64];                  /* struct statfs starts with f_type on mips o32 and on the host */
    b[0] = 0;
    if (sc3(SYS_statfs, p, b, 0) < 0) return -1;
    return b[0];
}
/* the card is mounted when its folder is on another file system type than its parent (tmpfs) */
static int card_mounted(void)
{
    long a = fs_type(TRACE_CARD_ROOT), b = fs_type(TRACE_CARD_PARENT);
    return a != -1 && b != -1 && a != b;
}
static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        long r = sc3(SYS_write, fd, p, n);
        if (r == -EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
static unsigned count_lines(const char *p, size_t n) { unsigned c = 0; while (n--) if (*p++ == '\n') c++; return c; }

/* Write what is buffered. force: regardless of the one-second pace (exec, exit). */
static void flush(int force)
{
    if (g_disabled || g_stopped) return;
    uint32_t t = now_ms();
    if (!force && t - g_last_flush < 1000 && g_len < BUF_BYTES / 2) return;    /* unlocked peek, re-checked below */
    if (!g_file[0]) return;
    /* The card log waits for the card. Once it has been written and the card goes away again (the player unmounts it
     * before poweroff -f; diskOS's "Shut down player" earlier still; USB storage mode), the lines go to a small tail
     * file in internal flash instead, so the end of a power off is not lost. */
    const char *path = g_file, *dir = g_dir;
    unsigned long cap = TRACE_CAP_BYTES;
    int tail = 0;
    if (g_on_card && !card_mounted()) {
        if (!g_card_seen || !g_tail[0]) return;
        path = g_tail; dir = 0; cap = TAIL_CAP; tail = 1;
    }
    if (!lock_take()) return;
    if (g_flushing || (!g_len && !g_dropped) || (!force && t - g_last_flush < 1000 && g_len < BUF_BYTES / 2)) { lock_drop(); return; }
    g_last_flush = t;
    char *out = g_cur; size_t n = g_len; unsigned dropped = g_dropped;
    g_cur = (g_cur == g_bufa) ? g_bufb : g_bufa; g_len = 0; g_dropped = 0; g_flushing = 1;
    lock_drop();

    unsigned lost = 0;
    if (dir) sc3(SYS_mkdir, dir, 0755, 0);
    int fd = (int)sc4(SYS_open, (long)path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644, 0);
    if (fd < 0) lost = count_lines(out, n) + dropped;
    else {
        long size = sc3(SYS_lseek, fd, 0, 2 /* SEEK_END */);
        if (tail && size >= 0 && (unsigned long)size + n > cap) {
            static const char m[] = "=== trace: tail file limit reached, lines dropped\n";
            if ((unsigned long)size < cap) write_all(fd, m, sizeof m - 1);
            lost = count_lines(out, n) + dropped;
        } else if (size >= 0 && (unsigned long)size >= cap) {
            static const char m[] = "=== trace: file size limit reached, recording stopped\n";
            write_all(fd, m, sizeof m - 1);
            g_stopped = 1;
        } else {
            if (dropped) {
                char lb[96]; out_t o = { lb, 0, sizeof lb };
                o_s(&o, "=== trace: "); o_u(&o, dropped); o_s(&o, " lines lost (log file not writable or buffer full)\n");
                write_all(fd, lb, o.n);
            }
            if (write_all(fd, out, n) < 0) lost = count_lines(out, n);
            else if (!tail && g_on_card) g_card_seen = 1;
        }
        sc3(SYS_close, fd, 0, 0);
    }
    if (lock_take()) { g_flushing = 0; g_dropped += lost; lock_drop(); }
    else g_flushing = 0;
}

/* Add one line: prefix, rate limit on the event text, buffer. */
static char g_hdr[400];          /* a helper program's start line, written only once it records something */
static size_t g_hdr_len;
static void emit_x(void *caller, const char *ev, size_t evn, int nolimit)
{
    if (g_disabled || g_stopped) return;
    if (g_hdr_len) { size_t n = g_hdr_len; g_hdr_len = 0; emit_x(0, g_hdr, n, 1); }
    uint32_t t = now_ms();
    uint32_t h = fnv(ev, evn);
    char line[LINE_MAX_ + 64]; out_t o = { line, 0, sizeof line };
    if (!lock_take()) return;
    unsigned sup = 0;
    {   /* at most 4 identical events per 10 s; the rest are counted and reported with the next one that passes */
        unsigned i = h & 255;
        if (g_rate[i].h != h || t - g_rate[i].win >= 10000) {
            if (g_rate[i].h == h) sup = g_rate[i].sup;
            g_rate[i].h = h; g_rate[i].win = t; g_rate[i].n = 1; g_rate[i].sup = 0;
        } else if (++g_rate[i].n > 4 && !nolimit) { g_rate[i].sup++; lock_drop(); return; }
    }
    int tid = cur_tid();
    o_u(&o, t / 1000); o_ch(&o, '.'); { unsigned ms = t % 1000; o_ch(&o, (char)('0' + ms / 100)); o_ch(&o, (char)('0' + ms / 10 % 10)); o_ch(&o, (char)('0' + ms % 10)); }
    o_ch(&o, ' '); o_i(&o, g_pid); if (tid != g_pid) { o_ch(&o, '/'); o_i(&o, tid); }
    o_ch(&o, ' '); o_x(&o, (unsigned long)caller, 0); o_ch(&o, ' ');
    if (sup) { o_s(&o, "[+"); o_u(&o, sup); o_s(&o, " like this in 10 s] "); }
    for (size_t i = 0; i < evn; i++) o_ch(&o, ev[i]);
    o_ch(&o, '\n');
    if (o.n && line[o.n - 1] != '\n') line[o.n - 1] = '\n';
    if (g_len + o.n <= BUF_BYTES) { for (size_t i = 0; i < o.n; i++) g_cur[g_len + i] = line[i]; g_len += o.n; }
    else g_dropped++;
    lock_drop();
}

static void emit(void *caller, const char *ev, size_t evn) { emit_x(caller, ev, evn, 0); }

/* ---- real functions ---- */
#define REAL(ret, name, args) \
    static ret (*real_##name) args; \
    static ret (*get_##name(void)) args { if (!real_##name) real_##name = (ret (*) args)dlsym(RTLD_NEXT_, #name); return real_##name; }
REAL(int, open, (const char *, int, ...))
REAL(int, open64, (const char *, int, ...))
REAL(int, openat, (int, const char *, int, ...))
REAL(int, openat64, (int, const char *, int, ...))
REAL(int, close, (int))
REAL(int, ioctl, (int, unsigned long, ...))
REAL(long, write, (int, const void *, size_t))
REAL(FILE *, fopen, (const char *, const char *))
REAL(FILE *, fopen64, (const char *, const char *))
REAL(int, fclose, (FILE *))
REAL(size_t, fwrite, (const void *, size_t, size_t, FILE *))
REAL(int, fputs, (const char *, FILE *))
REAL(int, fputc, (int, FILE *))
REAL(int, vfprintf, (FILE *, const char *, va_list))
REAL(int, vsnprintf, (char *, size_t, const char *, va_list))
REAL(int, system, (const char *))
REAL(FILE *, popen, (const char *, const char *))
REAL(int, execv, (const char *, char *const[]))
REAL(int, execvp, (const char *, char *const[]))
REAL(int, execve, (const char *, char *const[], char *const[]))
#define RA() __builtin_extract_return_addr(__builtin_return_address(0))

/* ---- classification and tables ---- */
enum { C_NONE, C_DEV, C_SYS, C_PROC };
static int classify(const char *p)
{
    if (!p) return C_NONE;
    if (s_pre(p, "/dev/")) return C_DEV;
    if (s_pre(p, "/sys/")) return C_SYS;
    if (s_pre(p, "/proc/")) return C_PROC;
    return C_NONE;
}
/* Files never worth a line: shell redirections and random numbers; and /proc in helper programs (killall and
 * start-stop-daemon walk all of /proc). Helper programs are the ones the player starts, which load us anew. */
static int quiet_path(int c, const char *p)
{
    if (c == C_PROC && !g_main) return 1;
    return s_eq(p, "/dev/null") || s_eq(p, "/dev/zero") || s_eq(p, "/dev/urandom") || s_eq(p, "/dev/random") ||
           s_eq(p, "/dev/tty") || s_eq(p, "/dev/console") || s_pre(p, "/dev/pts/");
}
static void copy_path(char *dst, const char *p) { size_t i = 0; for (; p[i] && i < FDPATH - 1; i++) dst[i] = p[i]; dst[i] = 0; }
static const char *fd_path(int fd) { return (fd >= 0 && fd < NFD && g_fdpath[fd][0]) ? g_fdpath[fd] : 0; }
static int first_seen(const char *p)
{
    uint32_t h = fnv(p, s_len(p)) | 1u;
    for (unsigned i = 0, k = h % 512; i < 512; i++, k = (k + 1) % 512) {
        if (g_seen[k] == h) return 0;
        if (!g_seen[k]) { g_seen[k] = h; return 1; }
    }
    return 0;
}
static void o_err(out_t *o, long r, int e) { o_s(o, " -> "); o_i(o, r); if (r < 0) { o_s(o, " errno "); o_i(o, e); } }

static void note_open(void *ra, const char *fn, const char *p, int flags, int r, int e)
{
    int c = classify(p);
    if (r >= 0 && r < NFD) { if (c != C_NONE && !quiet_path(c, p)) copy_path(g_fdpath[r], p); else g_fdpath[r][0] = 0; }
    if (c == C_NONE || g_disabled || quiet_path(c, p)) return;
    if (c != C_DEV && (flags & O_ACCMODE) == O_RDONLY && !first_seen(p)) return;
    char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
    o_s(&o, fn); o_ch(&o, ' '); o_qs(&o, p); o_s(&o, " flags "); o_x(&o, (unsigned)flags, 0);
    if (c != C_DEV && (flags & O_ACCMODE) == O_RDONLY) o_s(&o, " (first read)");
    o_err(&o, r, e);
    emit(ra, ev, o.n);
}

#define OPEN_BODY(getter, call)                                                        \
    unsigned mode = 0;                                                                 \
    if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {                       \
        va_list ap; va_start(ap, flags); mode = va_arg(ap, unsigned); va_end(ap);      \
    }                                                                                  \
    if (!getter()) { *__errno_location() = ENOSYS_; return -1; }                       \
    int r = call; int e = *__errno_location();                                         \
    note_open(RA(), __func__, path, flags, r, e);                                      \
    flush(0);                                                                          \
    *__errno_location() = e;                                                           \
    return r;
#define ENOSYS_ 38

int open(const char *path, int flags, ...) { OPEN_BODY(get_open, real_open(path, flags, mode)) }
int open64(const char *path, int flags, ...) { OPEN_BODY(get_open64, real_open64(path, flags, mode)) }
int openat(int dfd, const char *path, int flags, ...) { OPEN_BODY(get_openat, real_openat(dfd, path, flags, mode)) }
int openat64(int dfd, const char *path, int flags, ...) { OPEN_BODY(get_openat64, real_openat64(dfd, path, flags, mode)) }

int close(int fd)
{
    if (!get_close()) { *__errno_location() = ENOSYS_; return -1; }
    const char *p = fd_path(fd);
    char saved[FDPATH]; saved[0] = 0;
    if (p) copy_path(saved, p);
    if (fd >= 0 && fd < NFD) g_fdpath[fd][0] = 0;
    int r = real_close(fd); int e = *__errno_location();
    if (saved[0] && classify(saved) == C_DEV && !g_disabled) {
        char ev[160]; out_t o = { ev, 0, sizeof ev };
        o_s(&o, "close "); o_i(&o, fd); o_ch(&o, ' '); o_qs(&o, saved); o_err(&o, r, e);
        emit(RA(), ev, o.n);
    }
    *__errno_location() = e;
    return r;
}

/* ioctl request layout differs between MIPS and the host */
#if defined(__mips__)
#define IOC_SIZEBITS 13
#define IOC_DIRSHIFT 29
#define IOC_NONE 1u
#define IOC_READ 2u
#define IOC_WRITE 4u
#else
#define IOC_SIZEBITS 14
#define IOC_DIRSHIFT 30
#define IOC_NONE 0u
#define IOC_READ 2u
#define IOC_WRITE 1u
#endif

/* Copy up to n (<= 64) bytes from our own memory without faulting on a bad pointer: the kernel copies them into a
 * pipe of our own, and a bad pointer comes back as EFAULT. The pipe lives only for this call, so its descriptors can
 * never be someone else's file. (The first version used process_vm_readv; on the Disc every call failed, device log
 * 2026-11-14, cause not checked.) Returns the bytes copied or a negative errno. */
static long safe_read(void *dst, const void *src, size_t n)
{
    int pf[2];
    long r = sc3(SYS_pipe2, pf, O_CLOEXEC | O_NONBLOCK, 0);
    if (r < 0) return r;
    r = sc3(SYS_write, pf[1], src, n);
    if (r > 0) r = sc3(SYS_read, pf[0], dst, (size_t)r);
    sc3(SYS_close, pf[0], 0, 0);
    sc3(SYS_close, pf[1], 0, 0);
    return r;
}
static int alsa_hot(const char *p, unsigned type, unsigned nr)
{
    if (!p || type != 'A' || !s_pre(p, "/dev/snd/pcm")) return 0;
    return (nr >= 0x20 && nr <= 0x24) || (nr >= 0x50 && nr <= 0x53);   /* status/delay/hwsync/sync_ptr, read/write frames */
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap; va_start(ap, req); void *arg = va_arg(ap, void *); va_end(ap);
    if (!get_ioctl()) { *__errno_location() = ENOSYS_; return -1; }
    unsigned nr = req & 0xff, type = (req >> 8) & 0xff;
    unsigned size = (req >> 16) & ((1u << IOC_SIZEBITS) - 1), dir = (unsigned)(req >> IOC_DIRSHIFT);
    const char *p = fd_path(fd);
    if (g_disabled || alsa_hot(p, type, nr) || (!p && !g_main)) return real_ioctl(fd, req, arg);   /* sockets in helpers */
    int known = dir == IOC_NONE || dir == IOC_READ || dir == IOC_WRITE || dir == (IOC_READ | IOC_WRITE);
    unsigned char in[DUMP_MAX], out[DUMP_MAX];
    size_t dn = size < DUMP_MAX ? size : DUMP_MAX;
    long inok = -1, outok = -1;
    /* FiiO drivers pass structs with plain _IO requests (e.g. /dev/gpio 'G' 122): show 16 bytes behind an argument
     * that looks like a pointer, before and after the call */
    int memarg = (!known || dir == IOC_NONE || !size) && (unsigned long)arg >= 0x10000ul;
    if (memarg) dn = 16;
    if (dn && arg && (memarg || (known && (dir & IOC_WRITE) && dir != IOC_NONE))) inok = safe_read(in, arg, dn);
    int r = real_ioctl(fd, req, arg); int e = *__errno_location();
    if (dn && arg && r >= 0 && (memarg || (known && (dir & IOC_READ) && dir != IOC_NONE))) outok = safe_read(out, arg, dn);
    if (memarg && inok > 0 && outok == inok) { size_t i = 0; while (i < (size_t)inok && in[i] == out[i]) i++; if (i == (size_t)inok) outok = -1; }

    char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
    o_s(&o, "ioctl "); o_i(&o, fd); o_ch(&o, ' '); if (p) o_qs(&o, p); else o_ch(&o, '?');
    o_ch(&o, ' '); o_x(&o, req, 8);
    if (known) {
        o_ch(&o, ' ');
        o_s(&o, dir == IOC_NONE ? "_IO" : dir == IOC_READ ? "_IOR" : dir == IOC_WRITE ? "_IOW" : "_IOWR");
        o_s(&o, "('"); if (type >= 32 && type < 127) o_ch(&o, (char)type); else o_x(&o, type, 2);
        o_s(&o, "',"); o_u(&o, nr); if (dir != IOC_NONE) { o_s(&o, ",sz"); o_u(&o, size); } o_ch(&o, ')');
    }
    if (!known || dir == IOC_NONE || !size) { o_s(&o, " arg "); o_x(&o, (unsigned long)arg, 0); }
    if (inok > 0) { o_s(&o, memarg ? " mem " : " in "); o_hex(&o, in, (size_t)inok); }
    else if (!memarg && (inok == 0 || (inok < 0 && known && (dir & IOC_WRITE) && dir != IOC_NONE && dn && arg))) { o_s(&o, " in ?"); if (inok < 0) o_i(&o, inok); }
    if (outok > 0) { o_s(&o, memarg ? " mem-after " : " out "); o_hex(&o, out, (size_t)outok); }
    o_err(&o, r, e);
    /* DAC writes are what this is for: never folded away by the rate limit */
    emit_x(RA(), ev, o.n, p && s_pre(p, "/dev/cs43131"));
    flush(0);
    *__errno_location() = e;
    return r;
}

static void note_data(void *ra, const char *fn, const char *path, const void *data, size_t n, long r, int e)
{
    char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
    o_s(&o, fn); o_ch(&o, ' '); o_qs(&o, path); o_ch(&o, ' ');
    if (data) o_q(&o, (const char *)data, n, DATA_MAX); else o_s(&o, "?");
    o_err(&o, r, e);
    emit(ra, ev, o.n);
    flush(0);
}

long write(int fd, const void *b, size_t n)
{
    if (!get_write()) { *__errno_location() = ENOSYS_; return -1; }
    long r = real_write(fd, b, n);
    const char *p = fd_path(fd);
    if (!p || g_disabled) return r;
    int e = *__errno_location();
    unsigned char tmp[DATA_MAX]; size_t dn = n < DATA_MAX ? n : DATA_MAX;
    long ok = dn ? safe_read(tmp, b, dn) : 0;
    char path[FDPATH]; copy_path(path, p);
    note_data(RA(), "write", path, ok == (long)dn ? (const void *)tmp : 0, ok == (long)dn ? dn : 0, r, e);
    *__errno_location() = e;
    return r;
}

static const char *file_path(FILE *f)
{
    if (!f) return 0;
    for (unsigned i = 0; i < 32; i++) if (g_files[i].f == f) return g_files[i].path;
    return 0;
}
static void file_track(FILE *f, const char *p)
{
    for (unsigned i = 0; i < 32; i++) if (g_files[i].f == f || !g_files[i].f) { g_files[i].f = f; copy_path(g_files[i].path, p); return; }
}
static FILE *fopen_common(void *ra, const char *fn, FILE *(*real)(const char *, const char *), const char *path, const char *mode)
{
    FILE *f = real(path, mode); int e = *__errno_location();
    int c = classify(path);
    if (c != C_NONE && !g_disabled && !quiet_path(c, path)) {
        int rd = mode && mode[0] == 'r' && !(mode[1] == '+' || (mode[1] && mode[2] == '+'));
        if (f) {
            for (unsigned i = 0; i < 32; i++) if (g_files[i].f == f) g_files[i].f = 0;    /* a reused FILE * */
            if (!rd) file_track(f, path);
        }
        if (c == C_DEV || !rd || first_seen(path)) {
            char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
            o_s(&o, fn); o_ch(&o, ' '); o_qs(&o, path); o_ch(&o, ' '); o_qs(&o, mode);
            if (c != C_DEV && rd) o_s(&o, " (first read)");
            o_err(&o, f ? 0 : -1, e);
            emit(ra, ev, o.n);
            flush(0);
        }
    }
    *__errno_location() = e;
    return f;
}
FILE *fopen(const char *path, const char *mode)
{
    if (!get_fopen()) { *__errno_location() = ENOSYS_; return 0; }
    return fopen_common(RA(), "fopen", real_fopen, path, mode);
}
FILE *fopen64(const char *path, const char *mode)
{
    if (!get_fopen64()) { *__errno_location() = ENOSYS_; return 0; }
    return fopen_common(RA(), "fopen64", real_fopen64, path, mode);
}
int fclose(FILE *f)
{
    if (!get_fclose()) { *__errno_location() = ENOSYS_; return -1; }
    for (unsigned i = 0; i < 32; i++) if (g_files[i].f == f) g_files[i].f = 0;
    return real_fclose(f);
}
size_t fwrite(const void *b, size_t sz, size_t n, FILE *f)
{
    if (!get_fwrite()) { *__errno_location() = ENOSYS_; return 0; }
    size_t r = real_fwrite(b, sz, n, f);
    const char *p = file_path(f);
    if (p && !g_disabled) { int e = *__errno_location(); char path[FDPATH]; copy_path(path, p); note_data(RA(), "fwrite", path, b, sz * n, (long)r, e); *__errno_location() = e; }
    return r;
}
int fputs(const char *s, FILE *f)
{
    if (!get_fputs()) { *__errno_location() = ENOSYS_; return -1; }
    int r = real_fputs(s, f);
    const char *p = file_path(f);
    if (p && s && !g_disabled) { int e = *__errno_location(); char path[FDPATH]; copy_path(path, p); note_data(RA(), "fputs", path, s, s_len(s), r, e); *__errno_location() = e; }
    return r;
}
int fputc(int c, FILE *f)
{
    if (!get_fputc()) { *__errno_location() = ENOSYS_; return -1; }
    int r = real_fputc(c, f);
    const char *p = file_path(f);
    if (p && !g_disabled) { int e = *__errno_location(); char ch = (char)c, path[FDPATH]; copy_path(path, p); note_data(RA(), "fputc", path, &ch, 1, r, e); *__errno_location() = e; }
    return r;
}
static int vfprintf_common(void *ra, FILE *f, const char *fmt, va_list ap)
{
    const char *p = file_path(f);
    if (p && !g_disabled && get_vsnprintf()) {
        char tmp[256]; va_list cp; va_copy(cp, ap);
        int n = real_vsnprintf(tmp, sizeof tmp, fmt, cp); va_end(cp);
        int r = real_vfprintf(f, fmt, ap); int e = *__errno_location();
        char path[FDPATH]; copy_path(path, p);
        note_data(ra, "fprintf", path, tmp, n < 0 ? 0 : (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1, r, e);
        *__errno_location() = e;
        return r;
    }
    return real_vfprintf(f, fmt, ap);
}
int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    if (!get_vfprintf()) { *__errno_location() = ENOSYS_; return -1; }
    return vfprintf_common(RA(), f, fmt, ap);
}
int fprintf(FILE *f, const char *fmt, ...)
{
    if (!get_vfprintf()) { *__errno_location() = ENOSYS_; return -1; }
    va_list ap; va_start(ap, fmt);
    int r = vfprintf_common(RA(), f, fmt, ap);
    va_end(ap);
    return r;
}

/* the stock player feeds the watchdog once a second: record the first few, then only count */
static int is_wd_feed(const char *cmd) { return cmd && s_pre(cmd, "cmd_watchdog feed"); }
static void note_cmd(void *ra, const char *fn, const char *cmd, const char *extra)
{
    if (g_disabled) return;
    if (is_wd_feed(cmd) && ++g_wd_feeds > 3) return;
    char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
    o_s(&o, fn); o_ch(&o, ' '); o_qs(&o, cmd); if (extra) { o_ch(&o, ' '); o_qs(&o, extra); }
    if (is_wd_feed(cmd) && g_wd_feeds == 3) o_s(&o, " (further feeds not recorded)");
    emit(ra, ev, o.n);
}
int system(const char *cmd)
{
    if (!get_system()) { *__errno_location() = ENOSYS_; return -1; }
    void *ra = RA();
    note_cmd(ra, "system", cmd, 0);
    flush(1);
    int r = real_system(cmd); int e = *__errno_location();
    if (r != 0 && !g_disabled && !is_wd_feed(cmd)) {
        char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
        o_s(&o, "system "); o_qs(&o, cmd); o_s(&o, " status "); o_x(&o, (unsigned)r, 0);
        emit(ra, ev, o.n);
    }
    *__errno_location() = e;
    return r;
}
FILE *popen(const char *cmd, const char *type)
{
    if (!get_popen()) { *__errno_location() = ENOSYS_; return 0; }
    note_cmd(RA(), "popen", cmd, type);
    flush(1);
    return real_popen(cmd, type);
}
static void note_exec(void *ra, const char *fn, const char *path, char *const argv[])
{
    if (g_disabled) return;
    if (!g_main) { flush(1); return; }   /* a helper's own execs repeat the command the player already logged */
    char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
    o_s(&o, fn); o_ch(&o, ' '); o_qs(&o, path);
    for (int i = 1; argv && argv[i] && i < 16; i++) { o_ch(&o, ' '); o_qs(&o, argv[i]); }
    emit(ra, ev, o.n);
    flush(1);
}
int execv(const char *path, char *const argv[])
{
    if (!get_execv()) { *__errno_location() = ENOSYS_; return -1; }
    note_exec(RA(), "execv", path, argv);
    return real_execv(path, argv);
}
int execvp(const char *file, char *const argv[])
{
    if (!get_execvp()) { *__errno_location() = ENOSYS_; return -1; }
    note_exec(RA(), "execvp", file, argv);
    return real_execvp(file, argv);
}
int execve(const char *path, char *const argv[], char *const envp[])
{
    if (!get_execve()) { *__errno_location() = ENOSYS_; return -1; }
    note_exec(RA(), "execve", path, argv);
    return real_execve(path, argv, envp);
}
int execl(const char *path, const char *arg0, ...)
{
    char *argv[64]; int n = 0;
    argv[n++] = (char *)arg0;
    va_list ap; va_start(ap, arg0);
    if (arg0) {
        char *a;
        while ((a = va_arg(ap, char *)) != 0) { if (n < 63) argv[n++] = a; else { va_end(ap); *__errno_location() = 7 /* E2BIG */; return -1; } }
    }
    va_end(ap);
    argv[n] = 0;
    if (!get_execv()) { *__errno_location() = ENOSYS_; return -1; }
    note_exec(RA(), "execl", path, argv);
    return real_execv(path, argv);
}

/* ---- start and end of every process that loads us ---- */
static size_t read_small(const char *p, char *b, size_t cap)
{
    int fd = (int)sc4(SYS_open, (long)p, O_RDONLY | O_CLOEXEC, 0, 0);
    if (fd < 0) return 0;
    size_t n = 0;
    while (n < cap) { long r = sc3(SYS_read, fd, b + n, cap - n); if (r == -EINTR) continue; if (r <= 0) break; n += (size_t)r; }
    sc3(SYS_close, fd, 0, 0);
    return n;
}

__attribute__((constructor)) static void trace_init(void)
{
    g_pid = cur_pid(); g_bufpid = (unsigned)g_pid;
    {   /* where to write: DISKOS_TRACE_FILE (an absolute path with the player's start time, from the launcher) */
        static char env[65536];      /* BSS: costs RAM only for the bytes read */
        size_t m = read_small("/proc/self/environ", env, sizeof env - 1); env[m] = 0;
        const char *f = TRACE_DEFAULT_FILE;
        for (size_t i = 0; i < m; i += s_len(env + i) + 1)
            if (s_pre(env + i, TRACE_ENV "=")) {
                const char *v = env + i + sizeof TRACE_ENV;
                if (v[0] == '/' && s_len(v) < sizeof g_file) f = v;
                break;
            }
        size_t k = s_len(f), slash = 0;
        for (size_t i = 0; i < k; i++) if (f[i] == '/') slash = i;
        for (size_t i = 0; i < slash; i++) g_dir[i] = f[i];
        g_dir[slash] = 0;
        g_on_card = s_pre(f, TRACE_CARD_ROOT "/");
        if (g_on_card) {
            const char *b = f + slash + 1;
            if (s_pre(b, "player-trace-")) b += sizeof "player-trace-" - 1;
            out_t t = { g_tail, 0, sizeof g_tail };
            o_s(&t, TRACE_TAIL_DIR "/diskos-trace-tail-"); o_s(&t, b);
            g_tail[t.n] = 0;
        }
        for (size_t i = 1; i <= k; i++) g_file[i] = f[i];
        __sync_synchronize();
        g_file[0] = f[0];                                    /* last: flush() starts writing once this is set */
    }
    char cl[512]; size_t n = read_small("/proc/self/cmdline", cl, sizeof cl - 1); cl[n] = 0;
    /* argv as one line; and the program name */
    const char *a0 = cl, *base = a0;
    for (const char *s = a0; *s; s++) if (*s == '/') base = s + 1;
    char joined[512]; size_t j = 0;
    for (size_t i = 0; i < n && j < sizeof joined - 1; i++) joined[j++] = cl[i] ? cl[i] : ' ';
    while (j && joined[j - 1] == ' ') j--;
    joined[j] = 0;
    /* a watchdog feed (the tool itself or the shell around it) is known and once a second: record nothing */
    for (size_t i = 0; joined[i]; i++) if (s_pre(joined + i, "cmd_watchdog feed")) { g_disabled = 1; return; }
    g_main = s_eq(base, "mq_player");

    char ev[LINE_MAX_]; out_t o = { ev, 0, sizeof ev };
    long rt[4] = {0}; sc3(SC_CLOCK, 0 /* CLOCK_REALTIME */, rt, 0);
    o_s(&o, "=== trace start "); o_qs(&o, joined); o_s(&o, " ppid "); o_i(&o, sc3(SYS_getppid, 0, 0, 0));
    o_s(&o, " realtime "); o_u(&o, (unsigned long)rt[0]);
    if (g_main) emit(0, ev, o.n);
    else { for (size_t i = 0; i < o.n && i < sizeof g_hdr; i++) g_hdr[i] = ev[i]; g_hdr_len = o.n < sizeof g_hdr ? o.n : sizeof g_hdr; }
    if (g_main) {
        /* code mappings, to place the caller addresses */
        static char maps[16384];
        size_t m = read_small("/proc/self/maps", maps, sizeof maps - 1); maps[m] = 0;
        for (char *s = maps; *s;) {
            char *e = s; while (*e && *e != '\n') e++;
            int x = 0; for (char *q = s; q < e; q++) if (q[0] == ' ' && q + 4 < e && q[1] == 'r' && q[3] == 'x') { x = 1; break; }
            if (x) { out_t l = { ev, 0, sizeof ev }; o_s(&l, "map "); for (char *q = s; q < e; q++) o_ch(&l, *q); emit(0, ev, l.n); }
            s = *e ? e + 1 : e;
        }
        /* tells mq_ui the library loaded, so a library the loader rejects is switched off on the next start */
        int fd = (int)sc4(SYS_open, (long)TRACE_OK_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0644, 0);
        if (fd >= 0) sc3(SYS_close, fd, 0, 0);
    }
}
__attribute__((destructor)) static void trace_fini(void) { flush(1); }
