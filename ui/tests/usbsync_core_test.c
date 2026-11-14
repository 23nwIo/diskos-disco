/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host tests for USB import/export: usbdrive_core.c (block-device choice, ext label, sizes) and usbsync_core.c
 * (rsync command line, output parsing, backup/log names). With USC_TEST_RSYNC=/path/to/rsync the built command lines
 * also run through that rsync against temp folders, checking what really happens: Only missing never touches an
 * existing file, Update replaces it and keeps the old one in _changedN, nothing is ever deleted, and the backup/log/
 * cache folders are never copied.
 *   make -C ui/tests usbsync_core_test && USC_TEST_RSYNC=$(which rsync) ui/tests/usbsync_core_test */
#include "usbdrive_core.h"
#include "usbsync_core.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char T[256];
static void mkdirs(const char *p){ char b[1024]; snprintf(b, sizeof b, "%s", p); for(char *s = b + 1; *s; s++) if(*s == '/'){ *s = 0; mkdir(b, 0755); *s = '/'; } mkdir(b, 0755); }
static void put(const char *rel, const char *txt, long mtime){
    char p[1024]; snprintf(p, sizeof p, "%s/%s", T, rel);
    char d[1024]; snprintf(d, sizeof d, "%s", p); *strrchr(d, '/') = 0; mkdirs(d);
    FILE *f = fopen(p, "w"); assert(f); fputs(txt, f); fclose(f);
    if(mtime){ struct timespec ts[2] = { { mtime, 0 }, { mtime, 0 } }; assert(utimensat(AT_FDCWD, p, ts, 0) == 0); }
}
static char *get(const char *rel){
    static char b[256]; char p[1024]; snprintf(p, sizeof p, "%s/%s", T, rel);
    FILE *f = fopen(p, "r"); if(!f) return NULL;
    size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f); return b;
}
static int exists(const char *rel){ char p[1024]; snprintf(p, sizeof p, "%s/%s", T, rel); struct stat st; return stat(p, &st) == 0; }

/* ---------------------------------------------------------------- usbdrive_core */
static void sys_put(const char *rel, const char *val){ put(rel, val, 0); }
static void test_scan(void){
    sys_put("sys/sda/size", "0\n");                       /* empty reader slot */
    sys_put("sys/sdb/size", "249737216\n");               /* card with two partitions */
    sys_put("sys/sdb/sdb1/partition", "1\n"); sys_put("sys/sdb/sdb1/size", "1048576\n");
    sys_put("sys/sdb/sdb2/partition", "2\n"); sys_put("sys/sdb/sdb2/size", "248000000\n");
    sys_put("sys/sdb/sdb3/partition", "3\n"); sys_put("sys/sdb/sdb3/size", "2\n");   /* extended container: tiny */
    sys_put("sys/sdb/queue/x", "\n");
    sys_put("sys/sdc/size", "15523840\n");                /* unpartitioned (the other Disc's ext4 card) */
    sys_put("sys/mmcblk0/size", "1000000\n");             /* our own card: never */
    sys_put("sys/loop0/size", "1000\n");
    char sb[512]; snprintf(sb, sizeof sb, "%s/sys", T);
    udc_dev_t d[8]; int n = udc_scan(sb, d, 8);
    assert(n == 3);
    assert(!strcmp(d[0].name, "sdb1") && d[0].bytes == 1048576ULL * 512);
    assert(!strcmp(d[1].name, "sdb2"));
    assert(!strcmp(d[2].name, "sdc") && d[2].bytes == 15523840ULL * 512);
    assert(udc_scan("/nonexistent", d, 8) == 0);
    assert(udc_scan(sb, d, 1) == 1);
}
static void test_label(void){
    char p[512]; snprintf(p, sizeof p, "%s/ext.img", T);
    unsigned char img[4096]; memset(img, 0, sizeof img);
    img[1024 + 0x38] = 0x53; img[1024 + 0x39] = 0xEF; memcpy(img + 1024 + 0x78, "DISC", 4);
    FILE *f = fopen(p, "w"); fwrite(img, 1, sizeof img, f); fclose(f);
    char l[24]; assert(udc_ext_label(p, l, sizeof l) == 1 && !strcmp(l, "DISC"));
    memset(img + 1024 + 0x78, 'A', 16); f = fopen(p, "w"); fwrite(img, 1, sizeof img, f); fclose(f);
    assert(udc_ext_label(p, l, sizeof l) == 1 && strlen(l) == 16);
    img[1024 + 0x38] = 0; f = fopen(p, "w"); fwrite(img, 1, sizeof img, f); fclose(f);
    assert(udc_ext_label(p, l, sizeof l) == 0 && l[0] == 0);
    assert(udc_ext_label("/nonexistent", l, sizeof l) == 0);
}
static void test_size(void){
    char b[24];
    udc_fmt_size(127865454592ULL, b, sizeof b); assert(!strcmp(b, "128 GB"));
    udc_fmt_size(7948206080ULL, b, sizeof b);   assert(!strcmp(b, "7.9 GB"));
    udc_fmt_size(512000000ULL, b, sizeof b);    assert(!strcmp(b, "512 MB"));
    assert(udc_is_windows_fs("vfat") && udc_is_windows_fs("exfat") && udc_is_windows_fs("ntfs") && !udc_is_windows_fs("ext4") && !udc_is_windows_fs(NULL));
}

/* ---------------------------------------------------------------- usbsync_core */
static int has(char **argv, const char *a){ for(int i = 0; argv[i]; i++) if(!strcmp(argv[i], a)) return 1; return 0; }
static void test_argv(void){
    char *argv[USC_ARGV_MAX]; char buf[4096];
    usc_opts_t o = { .rsync = "/r", .src = "/u/sdb2/A", .src_is_dir = 1, .dst = "/tmp/sdcard/A" };
    int n = usc_argv(&o, argv, buf, sizeof buf);
    assert(n > 5 && argv[n] == NULL && !strcmp(argv[0], "/r"));
    assert(has(argv, "--ignore-existing") && !has(argv, "--checksum") && !has(argv, "--backup") && has(argv, "--info=progress2"));
    assert(!strcmp(argv[n - 2], "/u/sdb2/A/") && !strcmp(argv[n - 1], "/tmp/sdcard/A/") && !strcmp(argv[n - 3], "--"));
    for(int i = 0; argv[i]; i++) assert(!strstr(argv[i], "delete"));
    o.update = 1; o.checksum = 1; o.backup_dir = "/tmp/sdcard/_changed0/A"; o.log_file = "/tmp/sdcard/synclog/x.log";
    n = usc_argv(&o, argv, buf, sizeof buf);
    assert(!has(argv, "--ignore-existing") && has(argv, "--checksum") && has(argv, "--backup") && has(argv, "--backup-dir=/tmp/sdcard/_changed0/A"));
    assert(has(argv, "--log-file=/tmp/sdcard/synclog/x.log"));
    o.dry_run = 1; n = usc_argv(&o, argv, buf, sizeof buf);
    assert(has(argv, "--dry-run") && has(argv, "--itemize-changes") && !has(argv, "--info=progress2"));
    o.src = "/u/sdb2/A/x.flac"; o.src_is_dir = 0; o.dst = "/tmp/sdcard/A/"; n = usc_argv(&o, argv, buf, sizeof buf);
    assert(!strcmp(argv[n - 2], "/u/sdb2/A/x.flac") && !strcmp(argv[n - 1], "/tmp/sdcard/A/"));
    o.src = "relative"; assert(usc_argv(&o, argv, buf, sizeof buf) == -1);
    o.src = "/a"; assert(usc_argv(&o, argv, buf, 40) == -1);   /* doesn't fit */
}
static void test_parse(void){
    usc_count_t c = { 0 };
    usc_count_line(">f+++++++++ ALBUM2/c.flac", &c);
    usc_count_line(">f.st...... ALBUM1/b.flac", &c);
    usc_count_line(">fc........ ALBUM1/x.flac", &c);
    usc_count_line("cd+++++++++ ALBUM2/", &c);
    usc_count_line("Number of files: 7 (reg: 5, dir: 2)", &c);
    usc_count_line("Total transferred file size: 1,234,567 bytes", &c);
    assert(c.files_new == 1 && c.files_changed == 2 && c.bytes == 1234567ULL && c.have_bytes);
    usc_progress_t p = { 0 };
    assert(usc_parse_progress("    163,840,000  42%   15.21MB/s    0:00:10 (xfr#3, to-chk=7/12)", &p));
    assert(p.bytes == 163840000ULL && p.pct == 42 && p.xfr == 3 && p.chk_left == 7 && p.chk_total == 12);
    assert(usc_parse_progress("  0   0%    0.00kB/s    0:00:00 (xfr#0, ir-chk=1000/1001)", &p) && p.chk_total == 1001);
    assert(!usc_parse_progress("rsync: [sender] link_stat failed", &p));
    assert(!usc_parse_progress("sending incremental file list", &p));
    assert(!strcmp(usc_exit_text(0), "Done") && strstr(usc_exit_text(23), "synclog") && strstr(usc_exit_text(11), "full"));
}
static void test_paths(void){
    char r[256];
    assert(usc_rel("/tmp/sdcard", "/tmp/sdcard/A/B", r, sizeof r) == 0 && !strcmp(r, "/A/B"));
    assert(usc_rel("/tmp/sdcard", "/tmp/sdcard", r, sizeof r) == 0 && !strcmp(r, ""));
    assert(usc_rel("/tmp/sdcard/", "/tmp/sdcard/", r, sizeof r) == 0 && !strcmp(r, ""));
    assert(usc_rel("/tmp/sdcard", "/tmp/sdcardX/A", r, sizeof r) == -1);
    assert(usc_rel("/tmp/usbdrive/sdb2", "/tmp/usbdrive/sdb2/A/", r, sizeof r) == 0 && !strcmp(r, "/A"));
    char root[512]; snprintf(root, sizeof root, "%s/bk", T); mkdirs(root);
    assert(usc_backup_index(root) == 0);
    char d[600]; snprintf(d, sizeof d, "%s/_changed0", root); mkdir(d, 0755);
    snprintf(d, sizeof d, "%s/_changed1", root); mkdir(d, 0755);
    assert(usc_backup_index(root) == 2);
    setenv("TZ", "UTC", 1); tzset();
    char l[600]; usc_log_name("/tmp/sdcard", 1794677220L, "import", l, sizeof l);
    assert(!strcmp(l, "/tmp/sdcard/synclog/2026-11-14_172700-import.log"));
}

/* ---------------------------------------------------------------- real rsync */
static const char *RS;
static usc_count_t g_cnt;
static int run(usc_opts_t *o){
    char *argv[USC_ARGV_MAX]; char buf[8192];
    o->rsync = RS;
    assert(usc_argv(o, argv, buf, sizeof buf) > 0);
    int fds[2]; assert(pipe(fds) == 0);
    pid_t pid = fork();
    if(!pid){ dup2(fds[1], 1); dup2(fds[1], 2); close(fds[0]); execv(argv[0], argv); _exit(127); }
    close(fds[1]);
    memset(&g_cnt, 0, sizeof g_cnt);
    char line[2048]; size_t ln = 0; char ch; int prog = 0;
    while(read(fds[0], &ch, 1) == 1){
        if(ch == '\n' || ch == '\r'){ line[ln] = 0; if(ln){ usc_count_line(line, &g_cnt); usc_progress_t p = { 0 }; if(usc_parse_progress(line, &p)) prog++; } ln = 0; }
        else if(ln + 1 < sizeof line) line[ln++] = ch;
    }
    close(fds[0]);
    int st; waitpid(pid, &st, 0);
    if(!o->dry_run) assert(prog > 0);                     /* progress2 lines arrive and parse */
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static void test_rsync(void){
    const long OLD = 1700000000L, NEW = 1750000000L;
    /* A = the drive, B = the player */
    put("A/ALBUM1/a.flac", "aaaa", OLD); put("A/ALBUM1/b.flac", "bbbb", NEW);
    put("A/ALBUM2/c.flac", "cccc", OLD); put("A/ALBUM2/sub/d.flac", "dddd", OLD);
    put("A/_changed0/old.flac", "x", OLD); put("A/synclog/x.log", "x", OLD); put("A/.diskos/cache", "x", OLD);
    put("A/ALBUM1/_changed3/y.flac", "x", OLD); put("A/lost+found/z", "x", OLD);
    put("B/ALBUM1/a.flac", "aaaa", OLD); put("B/ALBUM1/b.flac", "BBBB", OLD);   /* same size, different content and time */
    put("B/ALBUM3/e.flac", "eeee", OLD);
    char A[512], B[512], Bl[600], bk[600];
    snprintf(A, sizeof A, "%s/A", T); snprintf(B, sizeof B, "%s/B", T);

    /* Only missing, whole card: ALBUM2 arrives (with its subfolder), b stays B's own, ALBUM3 stays */
    usc_opts_t o = { .src = A, .src_is_dir = 1, .dst = B, .dry_run = 1 };
    assert(run(&o) == 0 && g_cnt.files_new == 2 && g_cnt.files_changed == 0 && g_cnt.bytes == 8);
    o.dry_run = 0; snprintf(Bl, sizeof Bl, "%s/B/synclog/run1.log", T); mkdirs(strcat(strcpy(bk, B), "/synclog")); o.log_file = Bl;
    assert(run(&o) == 0);
    assert(!strcmp(get("B/ALBUM2/c.flac"), "cccc") && !strcmp(get("B/ALBUM2/sub/d.flac"), "dddd"));
    assert(!strcmp(get("B/ALBUM1/b.flac"), "BBBB") && !strcmp(get("B/ALBUM3/e.flac"), "eeee"));
    assert(!exists("B/_changed0") && !exists("B/.diskos") && !exists("B/lost+found") && !exists("B/ALBUM1/_changed3") && exists("B/synclog/run1.log") && !exists("B/synclog/x.log"));
    o.dry_run = 1; o.log_file = NULL; assert(run(&o) == 0 && g_cnt.files_new == 0 && g_cnt.files_changed == 0);   /* idempotent */

    /* Update by size+time: b differs in time -> replaced, B's version kept in _changed0/ALBUM1 */
    o.update = 1; assert(run(&o) == 0 && g_cnt.files_new == 0 && g_cnt.files_changed == 1);
    snprintf(bk, sizeof bk, "%s/_changed%d", B, usc_backup_index(B)); assert(usc_backup_index(B) == 0);
    o.dry_run = 0; o.backup_dir = bk;
    assert(run(&o) == 0);
    assert(!strcmp(get("B/ALBUM1/b.flac"), "bbbb") && !strcmp(get("B/_changed0/ALBUM1/b.flac"), "BBBB"));
    assert(!strcmp(get("B/ALBUM3/e.flac"), "eeee") && !strcmp(get("B/ALBUM1/a.flac"), "aaaa"));

    /* Same size and time, different content: only Compare thoroughly sees it */
    put("A/ALBUM2/c.flac", "CCCC", 0); put("B/ALBUM2/c.flac", "cccc", 0);
    { char p1[600], p2[600]; snprintf(p1, sizeof p1, "%s/A/ALBUM2/c.flac", T); snprintf(p2, sizeof p2, "%s/B/ALBUM2/c.flac", T);
      struct timespec ts[2] = { { OLD, 0 }, { OLD, 0 } }; utimensat(AT_FDCWD, p1, ts, 0); utimensat(AT_FDCWD, p2, ts, 0); }
    o.dry_run = 1; o.backup_dir = NULL; assert(run(&o) == 0 && g_cnt.files_changed == 0);
    o.checksum = 1; assert(run(&o) == 0 && g_cnt.files_changed == 1);
    /* one folder: ALBUM2 only, backups to _changed1/ALBUM2 (the path inside the folder is kept) */
    assert(usc_backup_index(B) == 1);
    char sa[600], sb[600]; snprintf(sa, sizeof sa, "%s/ALBUM2", A); snprintf(sb, sizeof sb, "%s/ALBUM2", B);
    snprintf(bk, sizeof bk, "%s/_changed1/ALBUM2", B);
    usc_opts_t f = { .src = sa, .src_is_dir = 1, .dst = sb, .update = 1, .checksum = 1, .backup_dir = bk };
    assert(run(&f) == 0 && !strcmp(get("B/ALBUM2/c.flac"), "CCCC") && !strcmp(get("B/_changed1/ALBUM2/c.flac"), "cccc"));
    /* one file into a folder the target doesn't have yet */
    put("A/ALBUM4/one.flac", "1111", OLD);
    char sf[600], df[600]; snprintf(sf, sizeof sf, "%s/ALBUM4/one.flac", A); snprintf(df, sizeof df, "%s/ALBUM4", B);
    usc_opts_t g = { .src = sf, .src_is_dir = 0, .dst = df };
    assert(run(&g) == 0 && !strcmp(get("B/ALBUM4/one.flac"), "1111"));
    /* nothing was ever deleted on B */
    assert(exists("B/ALBUM3/e.flac") && exists("B/ALBUM1/a.flac"));
    printf("rsync semantics: ok (%s)\n", RS);
}

int main(void){
    snprintf(T, sizeof T, "/tmp/usbsync_test_%d", (int)getpid());
    mkdirs(T);
    test_scan(); test_label(); test_size();
    test_argv(); test_parse(); test_paths();
    RS = getenv("USC_TEST_RSYNC");
    if(RS && *RS) test_rsync(); else printf("rsync semantics: skipped (set USC_TEST_RSYNC)\n");
    char cmd[300]; snprintf(cmd, sizeof cmd, "rm -rf %s", T); if(system(cmd)){}
    printf("usbsync_core_test: ok\n");
    return 0;
}
