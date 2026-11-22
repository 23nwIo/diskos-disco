/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* The card DB's file layer in mq_ui (ui/libdb.h, docs/LIBRARY_DB.md).
 *
 * mq_ui keeps SQLite connections open for a long time (musicdb's writer, the accent writer, one reader per thread, the
 * Up Next reader), and any of them may be in use on another thread at any moment. An open connection keeps its file
 * open, and an open file on the card makes the plain umount of USB storage mode and of Format SD Card fail. Closing
 * the connections from outside is not safe (a thread holding the handle would use freed memory).
 *
 * So the card DB is opened through this small VFS, a pass-through to SQLite's own unix VFS. When SD access is held
 * (sd_io_hold: USB storage, format, card pulled, Restart) libdb_vfs_detach() releases the locks and closes the real
 * files under every open connection. The connections stay valid objects; every further read or write on them fails
 * with an I/O error, which SQLite and the callers already handle as "no data". New opens fail until
 * libdb_vfs_allow(1) (sd_io_resume). A detached file is never reopened: the card may be another one by then, and
 * SQLite's page cache would not notice (both cards can start from the same copy). Instead libdb_vfs_gen() counts the
 * detaches that closed something, and musicdb replaces its connections when it changes.
 *
 * Version 1 I/O methods only: no shared memory (no WAL; the card DB uses journal_mode=TRUNCATE) and no mmap. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "libdb.h"

typedef struct lf {
    sqlite3_file base;            /* must be first: SQLite sees this */
    struct lf *next;
    pthread_mutex_t mu;           /* held around every forwarded call, so a detach never closes a file mid-call */
    int dead;                     /* the real file is closed */
    sqlite3_file *real;           /* the unix VFS's file, szOsFile bytes */
} lf;

static sqlite3_vfs g_vfs, *g_base;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;   /* the list, g_allow, g_gen */
static lf *g_files;
static int g_allow = 1;
static unsigned g_gen;

#define REAL(f) (((lf *)(f))->real)
#define FWD(f, call, deadrc) do { lf *x_ = (lf *)(f); int rc_; pthread_mutex_lock(&x_->mu); \
    rc_ = x_->dead ? (deadrc) : x_->real->pMethods->call; pthread_mutex_unlock(&x_->mu); return rc_; } while(0)

static int l_close(sqlite3_file *f){
    lf *x = (lf *)f;
    pthread_mutex_lock(&g_mu);
    for(lf **p = &g_files; *p; p = &(*p)->next) if(*p == x){ *p = x->next; break; }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_lock(&x->mu);
    int rc = x->dead ? SQLITE_OK : x->real->pMethods->xClose(x->real);
    x->dead = 1;
    pthread_mutex_unlock(&x->mu);
    pthread_mutex_destroy(&x->mu);
    free(x->real); x->real = NULL;
    return rc;
}
static int l_read(sqlite3_file *f, void *b, int n, sqlite3_int64 o){ FWD(f, xRead(REAL(f), b, n, o), SQLITE_IOERR_READ); }
static int l_write(sqlite3_file *f, const void *b, int n, sqlite3_int64 o){ FWD(f, xWrite(REAL(f), b, n, o), SQLITE_IOERR_WRITE); }
static int l_truncate(sqlite3_file *f, sqlite3_int64 s){ FWD(f, xTruncate(REAL(f), s), SQLITE_IOERR_TRUNCATE); }
static int l_sync(sqlite3_file *f, int fl){ FWD(f, xSync(REAL(f), fl), SQLITE_IOERR_FSYNC); }
static int l_size(sqlite3_file *f, sqlite3_int64 *s){ FWD(f, xFileSize(REAL(f), s), SQLITE_IOERR_FSTAT); }
static int l_lock(sqlite3_file *f, int l){ FWD(f, xLock(REAL(f), l), SQLITE_IOERR_LOCK); }
static int l_unlock(sqlite3_file *f, int l){ FWD(f, xUnlock(REAL(f), l), SQLITE_OK); }
static int l_reserved(sqlite3_file *f, int *out){ *out = 0; FWD(f, xCheckReservedLock(REAL(f), out), SQLITE_IOERR_CHECKRESERVEDLOCK); }
static int l_fctl(sqlite3_file *f, int op, void *a){ FWD(f, xFileControl(REAL(f), op, a), SQLITE_NOTFOUND); }
static int l_sector(sqlite3_file *f){ FWD(f, xSectorSize(REAL(f)), 512); }
static int l_devchar(sqlite3_file *f){ FWD(f, xDeviceCharacteristics(REAL(f)), 0); }

static const sqlite3_io_methods g_io = { 1, l_close, l_read, l_write, l_truncate, l_sync, l_size, l_lock, l_unlock,
                                         l_reserved, l_fctl, l_sector, l_devchar, 0, 0, 0, 0, 0, 0 };

static int l_open(sqlite3_vfs *v, sqlite3_filename name, sqlite3_file *f, int flags, int *outfl){
    (void)v;
    lf *x = (lf *)f;
    x->base.pMethods = NULL;      /* NULL = SQLite won't call xClose after a failed open */
    pthread_mutex_lock(&g_mu); int ok = g_allow; pthread_mutex_unlock(&g_mu);
    if(!ok) return SQLITE_CANTOPEN;
    x->real = calloc(1, (size_t)g_base->szOsFile);
    if(!x->real) return SQLITE_NOMEM;
    int rc = g_base->xOpen(g_base, name, x->real, flags, outfl);
    if(rc != SQLITE_OK){ if(x->real->pMethods) x->real->pMethods->xClose(x->real); free(x->real); x->real = NULL; return rc; }
    pthread_mutex_init(&x->mu, NULL);
    x->dead = 0;
    pthread_mutex_lock(&g_mu);
    if(!g_allow){                 /* held while it opened: give it back */
        pthread_mutex_unlock(&g_mu);
        x->real->pMethods->xClose(x->real); free(x->real); x->real = NULL; pthread_mutex_destroy(&x->mu);
        return SQLITE_CANTOPEN;
    }
    x->next = g_files; g_files = x;
    x->base.pMethods = &g_io;
    pthread_mutex_unlock(&g_mu);
    return SQLITE_OK;
}
static int l_delete(sqlite3_vfs *v, const char *name, int sync){
    (void)v;
    pthread_mutex_lock(&g_mu); int ok = g_allow; pthread_mutex_unlock(&g_mu);
    return ok ? g_base->xDelete(g_base, name, sync) : SQLITE_IOERR_DELETE;
}
static int l_access(sqlite3_vfs *v, const char *name, int fl, int *out){
    (void)v;
    pthread_mutex_lock(&g_mu); int ok = g_allow; pthread_mutex_unlock(&g_mu);
    if(!ok){ *out = 0; return SQLITE_OK; }
    return g_base->xAccess(g_base, name, fl, out);
}
static int l_full(sqlite3_vfs *v, const char *n, int cap, char *out){ (void)v; return g_base->xFullPathname(g_base, n, cap, out); }
static int l_rand(sqlite3_vfs *v, int n, char *out){ (void)v; return g_base->xRandomness(g_base, n, out); }
static int l_sleep(sqlite3_vfs *v, int us){ (void)v; return g_base->xSleep(g_base, us); }
static int l_time(sqlite3_vfs *v, double *t){ (void)v; return g_base->xCurrentTime(g_base, t); }
static int l_err(sqlite3_vfs *v, int n, char *out){ (void)v; return g_base->xGetLastError ? g_base->xGetLastError(g_base, n, out) : 0; }
static int l_time64(sqlite3_vfs *v, sqlite3_int64 *t){ (void)v; return g_base->xCurrentTimeInt64(g_base, t); }

int libdb_vfs_register(void){
    if(sqlite3_vfs_find(LIBDB_VFS)) return 0;
    g_base = sqlite3_vfs_find(NULL);
    if(!g_base || g_base->iVersion < 2) return -1;
    memset(&g_vfs, 0, sizeof g_vfs);
    g_vfs.iVersion = 2;
    g_vfs.szOsFile = (int)sizeof(lf);
    g_vfs.mxPathname = g_base->mxPathname;
    g_vfs.zName = LIBDB_VFS;
    g_vfs.xOpen = l_open; g_vfs.xDelete = l_delete; g_vfs.xAccess = l_access; g_vfs.xFullPathname = l_full;
    g_vfs.xRandomness = l_rand; g_vfs.xSleep = l_sleep; g_vfs.xCurrentTime = l_time; g_vfs.xGetLastError = l_err;
    g_vfs.xCurrentTimeInt64 = l_time64;
    return sqlite3_vfs_register(&g_vfs, 0) == SQLITE_OK ? 0 : -1;
}

int libdb_vfs_detach(void){
    int n = 0;
    pthread_mutex_lock(&g_mu);
    g_allow = 0;
    for(lf *x = g_files; x; x = x->next){
        pthread_mutex_lock(&x->mu);
        if(!x->dead){
            /* unlock first: the unix VFS keeps a file open while another connection to the same file holds a lock */
            x->real->pMethods->xUnlock(x->real, SQLITE_LOCK_NONE);
            x->real->pMethods->xClose(x->real);
            x->dead = 1; n++;
        }
        pthread_mutex_unlock(&x->mu);
    }
    if(n) g_gen++;
    pthread_mutex_unlock(&g_mu);
    return n;
}
void libdb_vfs_allow(int on){ pthread_mutex_lock(&g_mu); g_allow = on; pthread_mutex_unlock(&g_mu); }
unsigned libdb_vfs_gen(void){ pthread_mutex_lock(&g_mu); unsigned g = g_gen; pthread_mutex_unlock(&g_mu); return g; }
