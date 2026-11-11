/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Format SD card, device side: the worker thread behind Settings > System > Maintenance > Format SD Card (system.c
 * draws the dialogs). Steps, each logged to /usr/data/sdformat.log (internal storage, so it survives the restart):
 *   1. ui_sd_format_unmount (main.c): our readers/writers drained, plain umount of /tmp/sdcard. Fails closed: if the
 *      card stays mounted nothing is written.
 *   2. open /dev/mmcblk0 O_RDWR|O_EXCL. The kernel refuses that while any filesystem on the card is mounted, and while
 *      we hold it nobody (the stock player included) can mount the card. A second, independent guard.
 *   3. FAT32: sdf_format (sdformat_core.c) through pwrite, synced every 16 MiB and at the end.
 *      ext4: the firmware's own /sbin/mke2fs (e2fsprogs 1.45.5 on V2.57, Jo 2026-11-11) on the whole card, after the
 *      first 4 MiB and the last MiB were zeroed here (old partition table, so mke2fs needs no prompt). Our own exclusive
 *      handle is closed for that (mke2fs opens the card exclusively itself and refuses a mounted card); g_sd_formatting
 *      keeps diskOS from mounting it in between, and nothing sends the player a card event. sdformat_core.c's own
 *      ext4 writer stays in the tree (and in the host tests) but is not used on the device.
 *   4. read back: the cache is dropped and the new MBR / ext4 superblock is read from the card.
 * The partition table is not re-read here (that would wake the stock player's mount routine mid-way); the restart that
 * follows every format does it. */
#include "sdformat.h"
#include "sdformat_core.h"
#include "command_spawn.h"
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SDF_DEV "/dev/mmcblk0"
#define SDF_LOG "/usr/data/sdformat.log"
#define SDF_MKE2FS "/sbin/mke2fs"
#define SDF_MKE2FS_TIMEOUT_S 900

int  ui_sd_format_unmount(FILE *log, char *why, size_t n);   /* main.c */

static struct {
    _Atomic int state;       /* SDFMT_IDLE / RUNNING / DONE */
    _Atomic int phase;       /* SDFMT_PH_* */
    _Atomic int permille;
    _Atomic int elapsed;     /* seconds in the write phase (mke2fs gives no progress) */
    _Atomic int touched;     /* the card was unmounted (so the format ends in a restart) */
    int rc;                  /* 0 ok; written before state DONE (release) */
    char err[160];
    sdf_kind_t kind;
    pthread_t th;
} g_j;

typedef struct { int fd; uint64_t since_sync; FILE *log; } wctx_t;

static int dev_write(void *ud, uint64_t off, const void *buf, size_t len){
    wctx_t *c = ud;
    const char *p = buf;
    while(len){
        ssize_t n = pwrite(c->fd, p, len, (off_t)off);
        if(n < 0 && errno == EINTR) continue;
        if(n <= 0){ if(c->log) fprintf(c->log, "write at %llu failed: errno %d (%s)\n", (unsigned long long)off, errno, strerror(errno)); return -1; }
        p += n; off += (uint64_t)n; len -= (size_t)n; c->since_sync += (uint64_t)n;
    }
    if(c->since_sync >= (16u << 20)){                 /* keep the progress honest: data really on the card */
        c->since_sync = 0;
        if(fdatasync(c->fd) != 0){ if(c->log) fprintf(c->log, "sync failed: errno %d (%s)\n", errno, strerror(errno)); return -1; }
    }
    return 0;
}
static void dev_progress(void *ud, uint64_t done, uint64_t total){
    (void)ud;
    if(total) atomic_store(&g_j.permille, (int)(done * 1000 / total));
}

static uint64_t sysfs_bytes(void){
    FILE *f = fopen("/sys/block/mmcblk0/size", "r"); if(!f) return 0;
    unsigned long long s = 0; if(fscanf(f, "%llu", &s) != 1) s = 0; fclose(f);
    return (uint64_t)s * 512;
}
uint64_t sdfmt_card_bytes(void){ return sysfs_bytes(); }

static int any_card_mount(void){
    FILE *m = fopen("/proc/mounts", "r"); if(!m) return 1;   /* unreadable: assume mounted (fail closed) */
    char line[512]; int hit = 0;
    while(fgets(line, sizeof line, m)) if(!strncmp(line, SDF_DEV, strlen(SDF_DEV))){ hit = 1; break; }
    fclose(m);
    return hit;
}

/* ext4 through the firmware's mke2fs. fd: our exclusive handle (closed here). Returns 0 = formatted. */
static int run_mke2fs(int fd, uint64_t bytes, FILE *log, char *ferr, size_t fn){
    wctx_t c = { fd, 0, log };
    static char zero[1 << 16];
    for(uint64_t off = 0; off < (4u << 20); off += sizeof zero) if(dev_write(&c, off, zero, sizeof zero)){ snprintf(ferr, fn, "write failed"); close(fd); return -1; }
    for(uint64_t off = (bytes & ~(uint64_t)4095) - (1u << 20); off < (bytes & ~(uint64_t)4095); off += sizeof zero)
        if(dev_write(&c, off, zero, sizeof zero)){ snprintf(ferr, fn, "write failed"); close(fd); return -1; }
    if(fsync(fd) != 0){ snprintf(ferr, fn, "flush failed"); close(fd); return -1; }
    ioctl(fd, BLKFLSBUF, 0);
    close(fd);
    /* -F: no question about a whole-disk device (once only: a mounted card is still refused). -m 0: no blocks reserved
     * for root on a music card. nodiscard: no card-wide erase of unknown duration. Everything else as the firmware's
     * mke2fs.conf (or its built-in defaults) says for ext4. Output goes to the log. */
    char *argv[] = { "/bin/sh", "-c", "exec " SDF_MKE2FS " -t ext4 -F -L DISC -m 0 -E nodiscard " SDF_DEV " 2>&1", NULL };
    int lfd = log ? fileno(log) : -1;
    if(log){ fprintf(log, "running: %s\n", argv[2] + 5); fflush(log); }
    pid_t pid = 0;
    if(command_spawn_io(&pid, argv, lfd, -1) != 0){ snprintf(ferr, fn, "mke2fs could not be started"); return -1; }
    struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);
    for(;;){
        int st = 0; pid_t w = waitpid(pid, &st, WNOHANG);
        if(w == pid){
            if(WIFEXITED(st) && WEXITSTATUS(st) == 0) return 0;
            if(WIFEXITED(st)) snprintf(ferr, fn, "mke2fs exit %d", WEXITSTATUS(st));
            else snprintf(ferr, fn, "mke2fs killed by signal %d", WIFSIGNALED(st) ? WTERMSIG(st) : -1);
            return -1;
        }
        if(w < 0 && errno != EINTR){ snprintf(ferr, fn, "mke2fs lost"); return -1; }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        int el = (int)(now.tv_sec - t0.tv_sec);
        atomic_store(&g_j.elapsed, el);
        if(el >= SDF_MKE2FS_TIMEOUT_S){
            kill(-pid, SIGKILL); kill(pid, SIGKILL);
            while(waitpid(pid, &st, 0) < 0 && errno == EINTR){}
            snprintf(ferr, fn, "mke2fs took longer than %d minutes", SDF_MKE2FS_TIMEOUT_S / 60);
            return -1;
        }
        struct timespec ts = { 0, 200L * 1000 * 1000 }; nanosleep(&ts, NULL);
    }
}

static void finish(int rc, const char *why, FILE *log){
    if(why) snprintf(g_j.err, sizeof g_j.err, "%s", why);
    if(log){ fprintf(log, "%ld result: %s%s%s\n\n", (long)time(NULL), rc ? "FAILED" : "ok", rc ? " - " : "", rc ? g_j.err : ""); fclose(log); }
    sync();
    g_j.rc = rc;
    atomic_store_explicit(&g_j.state, SDFMT_DONE, memory_order_release);
}

static void *worker(void *arg){
    (void)arg;
    FILE *log = fopen(SDF_LOG, "a");
    if(log) setvbuf(log, NULL, _IOLBF, 0);
    const char *kn = g_j.kind == SDF_EXT4 ? "ext4 (whole card)" : "FAT32 (MBR partition)";
    if(log) fprintf(log, "%ld format SD card as %s, card %llu bytes\n", (long)time(NULL), kn, (unsigned long long)sysfs_bytes());

    if(g_j.kind == SDF_EXT4 && access(SDF_MKE2FS, X_OK) != 0){
        if(log) fprintf(log, SDF_MKE2FS " not found: errno %d\n", errno);
        finish(-7, "This firmware has no mke2fs, so ext4 is not available. Nothing was changed", log); return NULL;
    }
    atomic_store(&g_j.phase, SDFMT_PH_UNMOUNT);
    char why[160] = "";
    if(ui_sd_format_unmount(log, why, sizeof why) != 0){ finish(-1, why, log); return NULL; }
    atomic_store(&g_j.touched, 1);                    /* from here on the format ends in a restart */

    if(any_card_mount()){ if(log) fprintf(log, "a filesystem on the card is still mounted\n"); finish(-2, "The card is still mounted. Nothing was written", log); return NULL; }
    int fd = open(SDF_DEV, O_RDWR | O_EXCL | O_CLOEXEC);
    if(fd < 0){
        int e = errno; if(log) fprintf(log, "open " SDF_DEV " O_EXCL failed: errno %d (%s)\n", e, strerror(e));
        finish(-3, e == EBUSY ? "The card is still in use. Nothing was written" : "Couldn't open the card. Nothing was written", log); return NULL;
    }
    uint64_t bytes = 0;
    if(ioctl(fd, BLKGETSIZE64, &bytes) != 0 || bytes == 0){
        if(log) fprintf(log, "BLKGETSIZE64 failed: errno %d\n", errno);
        close(fd); finish(-4, "Couldn't read the card size. Nothing was written", log); return NULL;
    }
    if(log) fprintf(log, "card size %llu bytes (sysfs %llu)\n", (unsigned long long)bytes, (unsigned long long)sysfs_bytes());

    sdf_target_t t; memset(&t, 0, sizeof t);
    wctx_t c = { fd, 0, log };
    t.write = dev_write; t.progress = dev_progress; t.ud = &c; t.bytes = bytes; t.now = (uint32_t)time(NULL);
    {   int r = open("/dev/urandom", O_RDONLY | O_CLOEXEC); ssize_t n = r >= 0 ? read(r, t.rnd, sizeof t.rnd) : -1;
        if(r >= 0) close(r);
        if(n != (ssize_t)sizeof t.rnd){                /* never fatal: the values only need to differ between cards */
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            for(size_t i = 0; i < sizeof t.rnd; i++) t.rnd[i] ^= (uint8_t)((uint64_t)ts.tv_nsec >> (i % 8) ^ (uint64_t)t.now >> (i % 24) ^ i * 151);
        }
    }
    t.label = "DISC";

    atomic_store(&g_j.phase, SDFMT_PH_WRITE);
    char ferr[96] = "";
    int rc;
    if(g_j.kind == SDF_EXT4){
        atomic_store(&g_j.permille, -1);                  /* no progress from mke2fs: the UI shows the time instead */
        rc = run_mke2fs(fd, bytes, log, ferr, sizeof ferr);  /* closes fd */
        fd = rc == 0 ? open(SDF_DEV, O_RDWR | O_EXCL | O_CLOEXEC) : -1;
        if(rc == 0 && fd < 0){ rc = -5; snprintf(ferr, sizeof ferr, "card not readable after mke2fs (errno %d)", errno); }
        if(rc == 0) atomic_store(&g_j.phase, SDFMT_PH_VERIFY);
    } else {
        rc = sdf_format(g_j.kind, &t, ferr, sizeof ferr);
        if(rc == 0){
            atomic_store(&g_j.phase, SDFMT_PH_VERIFY);
            if(fsync(fd) != 0){ if(log) fprintf(log, "final sync failed: errno %d (%s)\n", errno, strerror(errno)); rc = -5; snprintf(ferr, sizeof ferr, "flush failed"); }
        }
    }
    if(rc == 0){
        unsigned char b[2048];
        if(ioctl(fd, BLKFLSBUF, 0) != 0 && log) fprintf(log, "BLKFLSBUF failed: errno %d (read-back may come from the cache)\n", errno);
        ssize_t n = pread(fd, b, sizeof b, 0);
        int ok = n == (ssize_t)sizeof b;
        if(ok && g_j.kind == SDF_FAT32) ok = b[510] == 0x55 && b[511] == 0xAA && b[446 + 4] == 0x0C;
        if(ok && g_j.kind == SDF_EXT4)  ok = b[1024 + 56] == 0x53 && b[1024 + 57] == 0xEF;
        if(!ok){ rc = -6; snprintf(ferr, sizeof ferr, "read-back check failed"); }
        if(log) fprintf(log, "read-back: %s\n", ok ? "ok" : "MISMATCH");
    }
    if(fd >= 0 && close(fd) != 0 && rc == 0){ rc = -5; snprintf(ferr, sizeof ferr, "flush failed"); }
    if(rc){
        char m[160]; snprintf(m, sizeof m, "Formatting failed (%s). Format the card again or on a computer", ferr[0] ? ferr : "error");
        if(log) fprintf(log, "sdf_format rc %d: %s\n", rc, ferr);
        finish(rc, m, log);
    } else {
        atomic_store(&g_j.permille, 1000);
        finish(0, NULL, log);
    }
    return NULL;
}

int sdfmt_start(sdf_kind_t kind){
    int idle = SDFMT_IDLE;
    if(!atomic_compare_exchange_strong(&g_j.state, &idle, SDFMT_RUNNING)) return -1;
    g_j.kind = kind; g_j.rc = 0; g_j.err[0] = 0;
    atomic_store(&g_j.phase, SDFMT_PH_UNMOUNT); atomic_store(&g_j.permille, 0); atomic_store(&g_j.elapsed, 0); atomic_store(&g_j.touched, 0);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 128 * 1024);
    int r = pthread_create(&g_j.th, &a, worker, NULL);
    pthread_attr_destroy(&a);
    if(r != 0){ atomic_store(&g_j.state, SDFMT_IDLE); return -1; }
    return 0;
}
int sdfmt_poll(sdfmt_status_t *s){
    int st = atomic_load_explicit(&g_j.state, memory_order_acquire);
    if(s){
        s->state = st; s->phase = atomic_load(&g_j.phase); s->permille = atomic_load(&g_j.permille); s->elapsed = atomic_load(&g_j.elapsed);
        s->touched = atomic_load(&g_j.touched); s->rc = st == SDFMT_DONE ? g_j.rc : 0;
        snprintf(s->err, sizeof s->err, "%s", st == SDFMT_DONE ? g_j.err : "");
    }
    if(st == SDFMT_DONE){ pthread_join(g_j.th, NULL); atomic_store(&g_j.state, SDFMT_IDLE); }
    return st;
}
