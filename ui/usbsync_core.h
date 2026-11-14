/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_USBSYNC_CORE_H
#define DISKOS_USBSYNC_CORE_H
#include <stddef.h>

/* The pure half of USB import / export (usbsync.c): the rsync command line, reading its output, and the names of the
 * backup and log folders. No LVGL, no threads; host-tested by tests/usbsync_core_test.c, which also runs the built
 * command lines through a real rsync. Nothing is ever deleted: there is no --delete in any mode. */

#define USC_RSYNC "/usr/data/sshd/bin/rsync"   /* static rsync 3.4.1 (docs/SSH_ACCESS.md) */
#define USC_ARGV_MAX 32

typedef struct {
    const char *rsync;       /* the binary (USC_RSYNC on the Disc) */
    const char *src;         /* absolute: a folder (its contents are copied) or one file */
    int src_is_dir;
    const char *dst;         /* absolute: the folder that becomes the copy of src (a folder), or holds it (a file) */
    int update;              /* 0 Only missing: files already on the target are never looked at (--ignore-existing)
                              * 1 Update changed files: a different file is replaced by the source's version */
    int checksum;            /* Update: compare contents (--checksum), else size and time */
    const char *backup_dir;  /* Update with Keep backups: the replaced file moves here (absolute), else NULL */
    int dry_run;             /* count only: -n plus a line per file that would be copied (-i) and the totals */
    const char *log_file;    /* rsync's own per-file log (appended), NULL for none */
} usc_opts_t;

/* Fill argv (NULL-terminated, at most USC_ARGV_MAX entries) from o. `buf` holds the built strings. Returns argc, or -1
 * when a path doesn't fit. */
int usc_argv(const usc_opts_t *o, char **argv, char *buf, size_t cap);

typedef struct {
    long files_new, files_changed;     /* dry run: files that would be created / replaced */
    unsigned long long bytes;          /* dry run: "Total transferred file size" */
    int have_bytes;
} usc_count_t;
/* One output line of a dry run (itemized lines and the --stats block); split on '\n' and '\r'. */
void usc_count_line(const char *line, usc_count_t *c);

typedef struct { unsigned long long bytes; int pct; long xfr, chk_left, chk_total; } usc_progress_t;
/* One --info=progress2 line: "  1,234,567  42%  10.52MB/s  0:01:02 (xfr#5, to-chk=10/200)". 1 = parsed. */
int usc_parse_progress(const char *line, usc_progress_t *p);

/* rsync's exit status as a short sentence for the result dialog */
const char *usc_exit_text(int code);

/* The first free "_changedN" under root (N from 0): the folder this run's replaced files go to. -1 if none below 10000. */
int usc_backup_index(const char *root);

/* path relative to root ("" for root itself, else starting with "/"); -1 when path is not inside root */
int usc_rel(const char *root, const char *path, char *out, size_t cap);

/* "synclog/2026-11-14_172700-import.log" under root, for the time t (local) */
void usc_log_name(const char *root, long t, const char *what, char *out, size_t cap);

#endif
