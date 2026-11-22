/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "shelf.h"
#include "sqlite3.h"
#include "../libdb.h"
#include "sdio.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

atomic_int ds_stopping, ds_abort_job;
extern char ds_server[1024], ds_download_root[1024], ds_state_dir[1100];
extern int ds_config_read(const char *, int, char *, size_t);
static sqlite3 *db, *localdb;
static int lockfd = -1, testmode;
static char response[1200];
static const char *localpath;
static int sql_cancel(void *unused) {
  (void)unused;
  return atomic_load(&ds_stopping) || atomic_load(&ds_abort_job);
}
void shelf_wake(void) {
  pthread_mutex_lock(&ds_shared.mu);
  pthread_cond_signal(&ds_shared.wake);
  pthread_mutex_unlock(&ds_shared.mu);
}
static int execsql(const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK;
}
static int dirs(const char *path) {
  char b[1200];
  if (strlen(path) >= sizeof b)
    return 0;
  strcpy(b, path);
  for (char *p = b + 1;; p++) {
    if (*p == '/' || !*p) {
      char c = *p;
      *p = 0;
      struct stat s;
      if (lstat(b, &s)) {
        if (errno != ENOENT || mkdir(b, 0755))
          return 0;
      } else if (!S_ISDIR(s.st_mode) || S_ISLNK(s.st_mode))
        return 0;
      *p = c;
      if (!c)
        break;
    }
  }
  return 1;
}
int shelf_sd_ready(void) {
  if (testmode) return 1;
  if (!sd_io_allowed()) return 0;
  FILE *f = fopen("/proc/mounts", "r");
  if (!f)
    return 0;
  char line[2048];
  int ok = 0;
  while (fgets(line, sizeof line, f)) {
    char dev[256], mnt[256], fs[64], opts[512];
    if (sscanf(line, "%255s %255s %63s %511s", dev, mnt, fs, opts) == 4 &&
        !strcmp(mnt, "/tmp/sdcard") && !strncmp(dev, "/dev/", 5) &&
        !strncmp(opts, "rw", 2))
      ok = 1;
  }
  fclose(f);
  if (!access("/tmp/.diskos_sd_export_intent", F_OK))
    ok = 0;
  return ok;
}
static void resident_reset(void);
int shelf_init(const char *conf, char *err, size_t cap) {
  response[0]=0;
  testmode = getenv("SHELF_TEST") != NULL;
  resident_reset();
  localpath = testmode ? getenv("SHELF_DB") : NULL;   /* NULL: the library in use at build time (libdb_path) */
  if (!ds_config_read(conf, testmode, err, cap))
    return 0;
  if (!shelf_sd_ready()) {
    snprintf(err, cap, "Insert SD card.\nLeave USB storage mode.");
    return 0;
  }
  lockfd = open(testmode ? "/tmp/disc-shelf-test.lock" : "/tmp/disc-shelf.lock",
                O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB)) {
    snprintf(err, cap, "Disc Shelf is already open.");
    return 0;
  }
  if (!dirs(ds_download_root)) {
    snprintf(err, cap, "Cannot create download folder.");
    return 0;
  }
  snprintf(ds_state_dir, sizeof ds_state_dir, "%s/.shelf", ds_download_root);
  if (!dirs(ds_state_dir)) {
    snprintf(err, cap, "Cannot create app state folder.");
    return 0;
  }
  snprintf(response, sizeof response, "%s/response.tmp", ds_state_dir);
  /* DBs are opened and used on the worker only. No UI-thread library scans. */
  return 1;
}
static int stmt_bind_item(sqlite3_stmt *s, const item_t *r) {
  sqlite3_bind_int(s, 1, r->kind);
  sqlite3_bind_text(s, 2, r->id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, r->name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 4, r->artist, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 5, r->album, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 6, r->suffix, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s, 7, r->track);
  sqlite3_bind_int(s, 8, r->disc);
  sqlite3_bind_int(s, 9, r->count);
  sqlite3_bind_int64(s, 10, r->size);
  sqlite3_bind_text(s, 11, r->cover, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(s,12,r->year);
  return sqlite3_step(s) == SQLITE_DONE;
}
static void row_item(sqlite3_stmt *s, item_t *r) {
  memset(r, 0, sizeof *r);
  r->kind = sqlite3_column_int(s, 0);
#define GET(i, member)                                                         \
  snprintf(r->member, sizeof r->member, "%s",                                  \
           sqlite3_column_text(s, i) ? (const char *)sqlite3_column_text(s, i) \
                                     : "")
  GET(1, id);
  GET(2, name);
  GET(3, artist);
  GET(4, album);
  GET(5, suffix);
  r->track = sqlite3_column_int(s, 6);
  r->disc = sqlite3_column_int(s, 7);
  r->count = sqlite3_column_int(s, 8);
  r->size = sqlite3_column_int64(s, 9);
  GET(10, cover);
  r->year=sqlite3_column_int(s,11);
}
typedef struct {
  sqlite3_stmt *insert;
  unsigned count;
} emit_context_t;
static int emit_row(const item_t *r, void *ctx) {
  emit_context_t *e = ctx;
  if (sql_cancel(NULL) || ((++e->count % 256) == 0 && !ds_storage_headroom(0)))
    return 0;
  int ok = stmt_bind_item(e->insert, r);
  sqlite3_reset(e->insert);
  return ok;
}
static const char *columns =
    "(kind INT,id TEXT PRIMARY KEY,name TEXT,artist TEXT,album TEXT,suffix "
    "TEXT,track INT,disc INT,count INT,size INTEGER,cover TEXT,year INT)";
static int reset_table(const char *name) {
  char sql[500];
  snprintf(sql, sizeof sql, "DROP TABLE IF EXISTS %s; CREATE TABLE %s%s", name,
           name, columns);
  return execsql(sql);
}
static int fetch(const char *endpoint, const char *args, const char *table) {
  if (!shelf_sd_ready())
    return 0;
  if (!ds_api_fetch(endpoint, args, response, 8LL * 1024 * 1024, 0, 0)) {
    if (atomic_load(&ds_abort_job) || atomic_load(&ds_stopping))
      ds_status_text("Cancelled.");
    else {
      pthread_mutex_lock(&ds_shared.mu);
      int explained = strstr(ds_shared.message, "Not enough") != NULL ||
                      strstr(ds_shared.message, "wget unavailable") != NULL;
      pthread_mutex_unlock(&ds_shared.mu);
      if (!explained)
        ds_status_text("Connection or transfer failed.\nCheck server and Wi-Fi.");
    }
    return 0;
  }
  /* Replacing a page and its rows is one transaction: a failed response
   * cannot erase a good cached page or cache a failed first artist request. */
  char sql[512];
  emit_context_t e = {0};
  snprintf(sql, sizeof sql,
           "INSERT OR IGNORE INTO %s VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", table);
  int ok = execsql("BEGIN") && reset_table(table) &&
           sqlite3_prepare_v2(db, sql, -1, &e.insert, NULL) == SQLITE_OK &&
           ds_parse_response(response, emit_row, &e);
  sqlite3_finalize(e.insert);
  if (ok) {
    if (!strcmp(endpoint, "getArtists") || !strcmp(endpoint, "getArtist")) {
      snprintf(sql, sizeof sql,
               "CREATE INDEX %s_name ON %s(name COLLATE NOCASE,id)", table,
               table);
      ok = execsql(sql);
    } else if (!strcmp(endpoint, "getAlbum")) {
      snprintf(sql, sizeof sql, "CREATE INDEX %s_order ON %s(kind,disc,track)",
               table, table);
      ok = execsql(sql);
      /* The album year takes precedence; songs may omit it or carry an
       * original-release year. Metadata is parsed after all nested songs. */
      if(ok){snprintf(sql,sizeof sql,"UPDATE %s SET year=(SELECT year FROM %s WHERE kind=%d AND year>0 LIMIT 1) WHERE kind=%d AND EXISTS(SELECT 1 FROM %s WHERE kind=%d AND year>0)",table,table,ALBUMS,TRACKS,table,ALBUMS);ok=execsql(sql);}
    }
    if (ok)
      ok = execsql("COMMIT");
  }
  if (!ok)
    execsql("ROLLBACK");
  unlink(response);
  if (!ok)
    ds_status_text("Server response rejected or too large.");
  return ok;
}
static void normalize(const char *s, char *out, size_t n) {
  while (*s && isspace((unsigned char)*s))
    s++;
  size_t i = 0;
  while (*s && i + 1 < n) {
    unsigned char c = *s++;
    out[i++] = c < 128 ? (char)tolower(c) : (char)c;
  }
  while (i && isspace((unsigned char)out[i - 1]))
    i--;
  out[i] = 0;
}
static int build_local(void) {
  if ((testmode && !localpath) || libdb_open_v2(localpath ? localpath : libdb_path(), &localdb, SQLITE_OPEN_READONLY,
                                    NULL) != SQLITE_OK) {
    ds_status_text("Local library unavailable; downloads blocked.");
    return 0;
  }
  sqlite3_busy_timeout(localdb, 1000);
  sqlite3_progress_handler(localdb, 1000, sql_cancel, NULL);
  sqlite3_exec(localdb, "PRAGMA cache_size=-128; PRAGMA mmap_size=0", NULL,
               NULL, NULL);
  if (!execsql("BEGIN; DROP TABLE IF EXISTS local; CREATE TABLE local(title "
               "TEXT,artist TEXT,album TEXT,path TEXT)"))
    return 0;
  sqlite3_stmt *read = NULL, *ins = NULL;
  /* Album artist may differ on compilations; older firmware lacks this column.
   */
  int has_album_artist =
      sqlite3_prepare_v2(
          localdb, "SELECT TITLE,ARTIST,ALBUM,PATH,ALBUM_ARTIST FROM SONG", -1,
          &read, NULL) == SQLITE_OK;
  if (!has_album_artist) {
    sqlite3_finalize(read);
    read = NULL;
  }
  int ok =
      (has_album_artist ||
       sqlite3_prepare_v2(localdb, "SELECT TITLE,ARTIST,ALBUM,PATH FROM SONG",
                          -1, &read, NULL) == SQLITE_OK) &&
      sqlite3_prepare_v2(db, "INSERT INTO local VALUES(?,?,?,?)", -1, &ins,
                         NULL) == SQLITE_OK;
  int rc = SQLITE_DONE, n = 0;
  while (ok && (rc = sqlite3_step(read)) == SQLITE_ROW) {
    if (atomic_load(&ds_stopping) || atomic_load(&ds_abort_job)) {
      ok = 0;
      break;
    }
    char title[STR], artist[STR], album[STR];
    const char *t = (const char *)sqlite3_column_text(read, 0),
               *a = (const char *)sqlite3_column_text(read, 1),
               *al = (const char *)sqlite3_column_text(read, 2),
               *p = (const char *)sqlite3_column_text(read, 3);
    if (!t || !a || !al || !p || !t[0])
      continue;
    if (strlen(t) >= STR || strlen(a) >= STR || strlen(al) >= STR) {
      ok = 0;
      break;
    }
    normalize(t, title, sizeof title);
    normalize(a, artist, sizeof artist);
    normalize(al, album, sizeof album);
    sqlite3_bind_text(ins, 1, title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 2, artist, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, p, -1, SQLITE_TRANSIENT);
    ok = sqlite3_step(ins) == SQLITE_DONE;
    sqlite3_reset(ins);
    const char *aa =
        has_album_artist ? (const char *)sqlite3_column_text(read, 4) : NULL;
    if (ok && aa && *aa) {
      if (strlen(aa) >= STR) {
        ok = 0;
        break;
      }
      char album_artist[STR];
      normalize(aa, album_artist, sizeof album_artist);
      if (strcmp(artist, album_artist)) {
        sqlite3_bind_text(ins, 2, album_artist, -1, SQLITE_TRANSIENT);
        ok = sqlite3_step(ins) == SQLITE_DONE;
        sqlite3_reset(ins);
      }
    }
    if (++n % 512 == 0) {
      ds_status_text("Checking local library: %d tracks", n);
      if (!ds_available_memory() || !ds_storage_headroom(0))
        ok = 0;
    }
  }
  sqlite3_finalize(read);
  sqlite3_finalize(ins);
  if (rc != SQLITE_DONE)
    ok = 0;
  if (ok)
    ok = execsql(
        "CREATE INDEX local_lookup ON local(album,artist,title); COMMIT");
  if (!ok)
    execsql("ROLLBACK");
  return ok;
}
static int file_exists(const char *p, int64_t size) {
  struct stat s;
  return p && !lstat(p, &s) && S_ISREG(s.st_mode) && s.st_size > 0 &&
         (size <= 0 || s.st_size == size);
}
static int duplicate(const item_t *r) {
  sqlite3_stmt *s = NULL;
  int present = 0;
  char title[STR], artist[STR], album[STR];
  normalize(r->name, title, sizeof title);
  normalize(r->artist, artist, sizeof artist);
  normalize(r->album, album, sizeof album);
  if (sqlite3_prepare_v2(
          db, "SELECT path,size FROM downloads WHERE server=? AND id=?", -1, &s,
          NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(s, 1, ds_server, -1, SQLITE_STATIC);
  sqlite3_bind_text(s, 2, r->id, -1, SQLITE_STATIC);
  int manifest_rc = sqlite3_step(s);
  if (manifest_rc == SQLITE_ROW)
    present = file_exists((const char *)sqlite3_column_text(s, 0),
                          sqlite3_column_int64(s, 1));
  sqlite3_finalize(s);
  if (manifest_rc != SQLITE_ROW && manifest_rc != SQLITE_DONE)
    return -1;
  if (present)
    return 1;
  if (sqlite3_prepare_v2(
          db, "SELECT l.path FROM local l WHERE title=? AND album=? AND artist=? AND NOT EXISTS(SELECT 1 FROM downloads d WHERE d.path=l.path AND (d.server<>? OR d.id<>?))",
          -1, &s, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(s, 1, title, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 2, album, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s, 3, artist, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(s,4,ds_server,-1,SQLITE_STATIC);sqlite3_bind_text(s,5,r->id,-1,SQLITE_STATIC);
  int rc;
  while ((rc = sqlite3_step(s)) == SQLITE_ROW)
    if (file_exists((const char *)sqlite3_column_text(s, 0), 0)) {
      present = 1;
      break;
    }
  if (!present && rc != SQLITE_DONE)
    present = -1;
  sqlite3_finalize(s);
  return present;
}
static int local_album(const item_t *r) {
  if (r->kind != ALBUMS || r->count <= 0)
    return 0;
  char artist[STR], album[STR];
  normalize(r->artist, artist, sizeof artist);
  normalize(r->name, album, sizeof album);
  sqlite3_stmt *s = NULL;
  int n = 0, members=0, complete=1;
  if(sqlite3_prepare_v2(db,"SELECT path,size FROM album_members WHERE server=? AND album=?",-1,&s,NULL)==SQLITE_OK){
    sqlite3_bind_text(s,1,ds_server,-1,SQLITE_STATIC);sqlite3_bind_text(s,2,r->id,-1,SQLITE_STATIC);
    int rc;while((rc=sqlite3_step(s))==SQLITE_ROW){members++;if(!file_exists((const char*)sqlite3_column_text(s,0),sqlite3_column_int64(s,1)))complete=0;}
    if(rc!=SQLITE_DONE)complete=0;
  }
  sqlite3_finalize(s);s=NULL;
  if(members==r->count && complete)return 1;
  if(members)n=2;
  if (sqlite3_prepare_v2(db,
                         "SELECT path FROM local WHERE album=? AND artist=?",
                         -1, &s, NULL) == SQLITE_OK) {
    sqlite3_bind_text(s, 1, album, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, artist, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(s) == SQLITE_ROW)
      if (file_exists((const char *)sqlite3_column_text(s, 0), 0)) {
        n = 2; /* Some tracks present; only a track match proves completeness. */
        break;
      }
  }
  sqlite3_finalize(s);
  return n;
}
static int publish(const char *table, request_t req) {
  char sql[700], filter[160] = "";
  if (req.view == TRACKS) snprintf(filter, sizeof filter, " WHERE kind=3");
  if(req.jump && req.view!=TRACKS){
    char condition[120];
    if(req.letter=='#')snprintf(condition,sizeof condition,"upper(substr(name,1,1)) NOT BETWEEN 'A' AND 'Z'");
    else if(req.letter>='A'&&req.letter<='Z')snprintf(condition,sizeof condition,"upper(substr(name,1,1))='%c'",req.letter);
    else strcpy(condition,"0");
    snprintf(sql,sizeof sql,"SELECT count(*) FROM %s WHERE name COLLATE NOCASE < (SELECT name FROM %s WHERE %s ORDER BY name COLLATE NOCASE,id LIMIT 1)",table,table,condition);
    sqlite3_stmt*j=NULL;if(sqlite3_prepare_v2(db,sql,-1,&j,NULL)!=SQLITE_OK)return 0;
    if(sqlite3_step(j)!=SQLITE_ROW){sqlite3_finalize(j);return 0;}req.offset=sqlite3_column_int(j,0);sqlite3_finalize(j);
  }
  snprintf(sql,sizeof sql,"SELECT count(*) FROM %s%s",table,filter);
  sqlite3_stmt *counter=NULL;int total_count=-1;
  if(sqlite3_prepare_v2(db,sql,-1,&counter,NULL)==SQLITE_OK && sqlite3_step(counter)==SQLITE_ROW)
    total_count=sqlite3_column_int(counter,0);
  sqlite3_finalize(counter);
  if(total_count<0 || total_count>1000000){ds_status_text("Cannot read collection size.");return 0;}
  snprintf(sql, sizeof sql, "SELECT * FROM %s%s ORDER BY %s LIMIT 33 OFFSET ?",
           table, filter,
           req.view == TRACKS   ? "disc,track,rowid"
           : req.view == ALBUMS ? "name COLLATE NOCASE,id"
                                : "name COLLATE NOCASE,id");
  sqlite3_stmt *s = NULL;
  item_t *items = ds_shared.scratch; /* Worker-owned, allocated only while open. */
  int n = 0, more = 0, rc = SQLITE_ERROR;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
    sqlite3_bind_int(s, 1, req.offset);
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
      if (sql_cancel(NULL)) {
        rc = SQLITE_INTERRUPT;
        break;
      }
      if (n == PAGE) {
        more = 1;
        break;
      }
      row_item(s, &items[n]);
      items[n].local = items[n].kind == TRACKS ? duplicate(&items[n])
                                               : local_album(&items[n]);
      n++;
    }
  }
  sqlite3_finalize(s);
  if (rc != SQLITE_DONE && !(rc == SQLITE_ROW && more)) {
    ds_status_text("Library read interrupted or failed. Retry.");
    return 0;
  }
  pthread_mutex_lock(&ds_shared.mu);
  memcpy(ds_shared.rows, items, n * sizeof *items);
  ds_shared.count = n;
  ds_shared.total_count = total_count;
  ds_shared.result_generation=req.generation;
  ds_shared.more = more;
  ds_shared.offset = req.offset;
  ds_shared.view = req.view;
  ds_shared.letter = req.letter;
  /* Only the worker completion publishes a new revision and clears busy. */
  snprintf(ds_shared.message, sizeof ds_shared.message,
           n ? "" : "No results");
  pthread_mutex_unlock(&ds_shared.mu);
  return 1;
}
static int resident_view = -1, catalogue_ready;
static char resident_id[IDLEN];
static void resident_reset(void){resident_view=-1;resident_id[0]=0;catalogue_ready=0;}
static int browse(request_t req) {
  char args[700], id[500];
  ds_url_encode(req.id, id, sizeof id);
  int ok = 1;
  const char *table = "browse";
  if (req.view == ALBUMS) {
    /* Subsonic's album-list API has no starts-with filter. Stream bounded
     * pages to a private on-disk index once per launch; publish only the
     * selected letter. Never build a whole-library array in RAM. */

    if (!catalogue_ready) {
      ok = reset_table("album_build");
      resident_view = -1;
      for (int off = 0; ok; off += PAGE) {
        if (off > 1000000 || sql_cancel(NULL) || !ds_storage_headroom(0)) {
          ok = 0;
          break;
        }
        snprintf(args, sizeof args, "type=alphabeticalByName&size=32&offset=%d", off);
        ok = fetch("getAlbumList2", args, "browse");
        if (!ok) break;
        sqlite3_stmt *count = NULL;
        int n = -1;
        if (sqlite3_prepare_v2(db, "SELECT count(*) FROM browse", -1, &count, NULL) == SQLITE_OK &&
            sqlite3_step(count) == SQLITE_ROW) n = sqlite3_column_int(count, 0);
        sqlite3_finalize(count);
        ok = n >= 0 && execsql("INSERT OR IGNORE INTO album_build SELECT * FROM browse");
        if (!ok || n < PAGE) break;
        ds_status_text("Loading album index: %d", off + n);
      }
      if (ok) {
        ok = execsql("BEGIN; DROP TABLE IF EXISTS album_catalogue; "
                     "ALTER TABLE album_build RENAME TO album_catalogue; "
                     "CREATE INDEX album_catalogue_name ON album_catalogue(name COLLATE NOCASE,id); COMMIT");
        if (!ok) execsql("ROLLBACK");
        catalogue_ready = ok;
      }
    }
    return ok && publish("album_catalogue", req);
  }
  if (req.view == ARTISTS) {
    table = "artists";
    sqlite3_stmt *s = NULL;
    int exists = sqlite3_prepare_v2(db, "SELECT 1 FROM artists LIMIT 1", -1, &s,
                                    NULL) == SQLITE_OK;
    sqlite3_finalize(s);
    if (!exists)
      ok = fetch("getArtists", "", table);
  } else if (req.view == ARTIST_ALBUMS) {
    snprintf(args, sizeof args, "id=%s", id);
    if (resident_view != req.view || strcmp(resident_id, req.id))
      ok = fetch("getArtist", args, table);
  } else if (req.view == TRACKS) {
    snprintf(args, sizeof args, "id=%s", id);
    if (resident_view != req.view || strcmp(resident_id, req.id))
      ok = fetch("getAlbum", args, table);
  }
  if (ok) {
    if (req.view != ARTISTS) {
      resident_view = req.view;
      snprintf(resident_id, sizeof resident_id, "%s", req.id);
    }
    ok = publish(table, req);
  }
  return ok;
}
static void safe_name(const char *s, char *out, size_t cap) {
  size_t n = 0;
  const unsigned char *p = (const unsigned char *)s;
  while (*p) {
    unsigned char c = *p;
    size_t width = c < 128                  ? 1
                   : c >= 0xc2 && c <= 0xdf ? 2
                   : c >= 0xe0 && c <= 0xef ? 3
                   : c >= 0xf0 && c <= 0xf4 ? 4
                                            : 1;
    if (n + width >= cap)
      break;
    int valid = 1;
    for (size_t i = 1; i < width; i++)
      if (!p[i] || (p[i] & 0xc0) != 0x80) {
        valid = 0;
        break;
      }
    if (!valid) {
      out[n++] = '_';
      p++;
      continue;
    }
    if (width == 1)
      out[n++] = (c < 32 || strchr("/\\:*?\"<>|", c)) ? '_' : c;
    else {
      memcpy(out + n, p, width);
      n += width;
    }
    p += width;
  }
  out[n] = 0;
  while (n && (out[n - 1] == ' ' || out[n - 1] == '.'))
    out[--n] = 0;
  if (!n)
    strcpy(out, "Unknown");
  if (out[0] == '.')
    out[0] = '_';
}
static int allowed_suffix(const char *s) {
  const char *allowed[] = {"mp3", "flac", "ogg", "opus", "wav", "aif", "aiff",
                           "m4a", "aac",  "wma", "ape",  "dsf", "dff", "wv"};
  for (unsigned i = 0; i < sizeof allowed / sizeof *allowed; i++)
    if (!strcmp(s, allowed[i]))
      return 1;
  return 0;
}
static int save_manifest(const item_t *r, const char *path) {
  sqlite3_stmt *s = NULL;
  int ok = sqlite3_prepare_v2(
               db, "INSERT OR REPLACE INTO downloads VALUES(?,?,?,?,?)", -1, &s,
               NULL) == SQLITE_OK;
  if (ok) {
    sqlite3_bind_text(s, 1, ds_server, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, r->id, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 4, r->size);
    sqlite3_bind_text(s, 5, r->name, -1, SQLITE_STATIC);
    ok = sqlite3_step(s) == SQLITE_DONE;
  }
  sqlite3_finalize(s);
  if(ok){
    char title[STR],artist[STR],album[STR];normalize(r->name,title,sizeof title);normalize(r->artist,artist,sizeof artist);normalize(r->album,album,sizeof album);
    if(sqlite3_prepare_v2(db,"INSERT INTO local SELECT ?,?,?,? WHERE NOT EXISTS(SELECT 1 FROM local WHERE path=?)",-1,&s,NULL)==SQLITE_OK){
      sqlite3_bind_text(s,1,title,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,artist,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,album,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,4,path,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,5,path,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_DONE;
    }else ok=0;sqlite3_finalize(s);
  }
  return ok;
}
static void track_directory(const item_t *r,char *dir,size_t cap){
  char artist[100],album[100];safe_name(r->artist,artist,sizeof artist);safe_name(r->album,album,sizeof album);
  int year=r->year>0 && r->year<=9999?r->year:0;
  snprintf(dir,cap,"%s/%s/%04d - %s",ds_download_root,artist,year,album);
}
/* Clean first choice; keep any unrelated occupant and use a short suffix only
 * for real collisions. Ownership is established solely by the manifest. */
static int track_free_path(const item_t *r,const char *dir,const char *title,char *path,size_t cap){
  for(int attempt=0;attempt<10000;attempt++){
    int n;
    if(!attempt)n=snprintf(path,cap,"%s/%02d. %s.%s",dir,r->track,title,r->suffix);
    else if(attempt==1 && r->disc>1)n=snprintf(path,cap,"%s/%02d. %s (Disc %d).%s",dir,r->track,title,r->disc,r->suffix);
    else n=snprintf(path,cap,"%s/%02d. %s (%d).%s",dir,r->track,title,attempt+1,r->suffix);
    if(n<0 || (size_t)n>=cap)return 0;
    struct stat st;if(lstat(path,&st)){
      if(errno!=ENOENT)return 0;
      char part[1630];snprintf(part,sizeof part,"%s.part",path);
      if(lstat(part,&st)){if(errno==ENOENT)return 1;return 0;}
    }
  }
  return 0;
}
static int download_track(item_t r, int *saved, int *skipped) {
  int dup = duplicate(&r);
  if (dup < 0) {
    ds_status_text("Duplicate check failed; stopped.");
    return 0;
  }
  if (dup && !ds_shared.replace) {
    (*skipped)++;
    return 1;
  }
  if (!shelf_sd_ready() || !allowed_suffix(r.suffix) || r.size <= 0 ||
      r.size > 8LL * 1024 * 1024 * 1024) {
    ds_status_text("Unsupported file or unknown size; stopped.");
    return 0;
  }
  char title[120], dir[1300], path[1600], temp[1630],encoded[500],args[600];
  safe_name(r.name,title,sizeof title);track_directory(&r,dir,sizeof dir);
  path[0]=0;
  /* Replace only this server's exact ID, never a tag-only match in the user's library. */
  int owned=0; sqlite3_stmt *own=NULL;
  if(sqlite3_prepare_v2(db,"SELECT path FROM downloads WHERE server=? AND id=?",-1,&own,NULL)!=SQLITE_OK)return 0;
  sqlite3_bind_text(own,1,ds_server,-1,SQLITE_STATIC);sqlite3_bind_text(own,2,r.id,-1,SQLITE_STATIC);
  int orc=sqlite3_step(own);
  if(orc==SQLITE_ROW){
    const char*old=(const char*)sqlite3_column_text(own,0);size_t len=strlen(ds_download_root);
    owned=old && !strncmp(old,ds_download_root,len) && old[len]=='/' && !strstr(old,"/../") && strlen(old)<sizeof path;
    if(owned){
      const char*slash=strrchr(old,'/');size_t plen=slash?(size_t)(slash-old):0;
      if(!plen||plen>=sizeof dir)owned=0;
      else {char parent[sizeof dir];memcpy(parent,old,plen);parent[plen]=0;
        if(!dirs(parent))owned=0;
        else {strcpy(dir,parent);strcpy(path,old);}
      }
    }
  }
  sqlite3_finalize(own);if(orc!=SQLITE_ROW && orc!=SQLITE_DONE)return 0;
  if(!owned && (!dirs(dir) || !track_free_path(&r,dir,title,path,sizeof path))){ds_status_text("Cannot choose a free filename; kept existing files.");return 0;}
  snprintf(temp,sizeof temp,"%s.part",path);
  if(dup && ds_shared.replace && !owned){(*skipped)++;return 1;}
  struct stat previous, st;int existed=!lstat(path,&previous);
  if(existed && (!S_ISREG(previous.st_mode) || (!owned && !file_exists(path,r.size)))){
    ds_status_text("Existing destination differs; kept original.");return 0;
  }
  if(!existed && errno!=ENOENT)return 0;
  if(existed && !ds_shared.replace){
    if(!save_manifest(&r,path))return 0;(*skipped)++;return 1;
  }
  ds_status_text("Downloading %s\n%d saved / %d already local", r.name, *saved,
              *skipped);
  ds_url_encode(r.id, encoded, sizeof encoded);
  snprintf(args, sizeof args, "id=%s", encoded);
  if (!ds_api_fetch("download", args, temp, r.size, r.size, 1)) {
    if (!atomic_load(&ds_abort_job) && !atomic_load(&ds_stopping))
      ds_status_text("Download failed. Retry; partial file removed.");
    return 0;
  }
  int unchanged=existed ? (!lstat(path,&st) && S_ISREG(st.st_mode) && st.st_dev==previous.st_dev && st.st_ino==previous.st_ino && st.st_size==previous.st_size && st.st_mtime==previous.st_mtime)
                        : (lstat(path,&st)!=0 && errno==ENOENT);
  if(atomic_load(&ds_abort_job)||atomic_load(&ds_stopping)||!shelf_sd_ready()||!unchanged||rename(temp,path)){
    unlink(temp);ds_status_text("Not replaced: cancelled, card unavailable or file changed.");return 0;
  }
  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    fsync(dfd);
    close(dfd);
  }
  if (!save_manifest(&r, path)) {
    ds_status_text("File saved, manifest failed; retry safely.");
    return 0;
  }
  (*saved)++;
  return 1;
}
static int next_item(const char *table, int kind, int64_t *cursor, item_t *r) {
  char sql[256];
  snprintf(
      sql, sizeof sql,
      "SELECT rowid,* FROM %s WHERE kind=? AND rowid>? ORDER BY rowid LIMIT 1",
      table);
  sqlite3_stmt *st = NULL;
  if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_int(st, 1, kind);
  sqlite3_bind_int64(st, 2, *cursor);
  int rc = sqlite3_step(st);
  if (rc == SQLITE_ROW) {
    *cursor = sqlite3_column_int64(st, 0);
    r->kind = sqlite3_column_int(st, 1);
#define FIELD(i, member)                                                       \
  snprintf(r->member, sizeof r->member, "%s",                                  \
           sqlite3_column_text(st, i)                                          \
               ? (const char *)sqlite3_column_text(st, i)                      \
               : "")
    FIELD(2, id);
    FIELD(3, name);
    FIELD(4, artist);
    FIELD(5, album);
    FIELD(6, suffix);
    r->track = sqlite3_column_int(st, 7);
    r->disc = sqlite3_column_int(st, 8);
    r->count = sqlite3_column_int(st, 9);
    r->size = sqlite3_column_int64(st, 10);
    FIELD(11, cover);
    r->year=sqlite3_column_int(st,12);
  }
  sqlite3_finalize(st);
  return rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
}
static void album_cover(const char *id) {
  if (atomic_load(&ds_abort_job) || atomic_load(&ds_stopping))
    return;
  sqlite3_stmt *st = NULL;
  item_t r = {0};
  if (sqlite3_prepare_v2(db, "SELECT * FROM tracks WHERE kind=3 LIMIT 1", -1,
                         &st, NULL) == SQLITE_OK &&
      sqlite3_step(st) == SQLITE_ROW)
    row_item(st, &r);
  sqlite3_finalize(st);
  if (!r.id[0])
    return;
  char dir[1300],path[1400],tmp[1400],encoded[500],args[600];
  track_directory(&r,dir,sizeof dir);
  struct stat fs;
  if (stat(dir, &fs) || !S_ISDIR(fs.st_mode))
    return;
  snprintf(path, sizeof path, "%s/cover.jpg", dir);
  if (!access(path, F_OK))
    return;
  snprintf(path, sizeof path, "%s/cover.png", dir);
  if (!access(path, F_OK))
    return;
  ds_url_encode(id, encoded, sizeof encoded);
  snprintf(args, sizeof args, "id=%s&size=268", encoded);
  snprintf(tmp, sizeof tmp, "%s/cover.part", dir);
  if (!ds_api_fetch("getCoverArt", args, tmp, 1024 * 1024, 0, 2))
    return;
  FILE *f = fopen(tmp, "rb");
  int c = f ? fgetc(f) : 0;
  if (f)
    fclose(f);
  snprintf(path, sizeof path, "%s/cover.%s", dir, c == 255 ? "jpg" : "png");
  if (access(path, F_OK) && errno == ENOENT)
    rename(tmp, path);
  unlink(tmp);
}
static int member_save(const char*album,const item_t*r){
 sqlite3_stmt*s=NULL;char path[1600]="";
 if(sqlite3_prepare_v2(db,"SELECT path FROM downloads WHERE server=? AND id=?",-1,&s,NULL)!=SQLITE_OK)return 0;
 sqlite3_bind_text(s,1,ds_server,-1,SQLITE_STATIC);sqlite3_bind_text(s,2,r->id,-1,SQLITE_STATIC);
 if(sqlite3_step(s)==SQLITE_ROW)snprintf(path,sizeof path,"%s",sqlite3_column_text(s,0));sqlite3_finalize(s);
 int64_t size=r->size;
 if(!path[0]){
   char title[STR],artist[STR],name[STR];normalize(r->name,title,sizeof title);normalize(r->artist,artist,sizeof artist);normalize(r->album,name,sizeof name);
   if(sqlite3_prepare_v2(db,"SELECT path FROM local WHERE title=? AND album=? AND artist=?",-1,&s,NULL)!=SQLITE_OK)return 0;
   sqlite3_bind_text(s,1,title,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,name,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,artist,-1,SQLITE_TRANSIENT);
   while(sqlite3_step(s)==SQLITE_ROW){const char*p=(const char*)sqlite3_column_text(s,0);struct stat st;if(p&&strlen(p)<sizeof path&&!lstat(p,&st)&&S_ISREG(st.st_mode)&&st.st_size>0){strcpy(path,p);size=st.st_size;break;}}
   sqlite3_finalize(s);if(!path[0])return 0;
   /* Presence only: this does not add a download manifest or grant overwrite ownership. */
 }
 if(sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO album_members VALUES(?,?,?,?,?)",-1,&s,NULL)!=SQLITE_OK)return 0;
 sqlite3_bind_text(s,1,ds_server,-1,SQLITE_STATIC);sqlite3_bind_text(s,2,album,-1,SQLITE_STATIC);sqlite3_bind_text(s,3,r->id,-1,SQLITE_STATIC);sqlite3_bind_text(s,4,path,-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,5,size);
 int ok=sqlite3_step(s)==SQLITE_DONE;sqlite3_finalize(s);return ok;
}
static int download_album(const char *id, int *saved, int *skipped) {
  char encoded[500], args[600];
  ds_url_encode(id, encoded, sizeof encoded);
  snprintf(args, sizeof args, "id=%s", encoded);
  if (!fetch("getAlbum", args, "tracks"))
    return 0;
  sqlite3_stmt*clear=NULL;
  if(sqlite3_prepare_v2(db,"DELETE FROM album_members WHERE server=? AND album=?",-1,&clear,NULL)!=SQLITE_OK)return 0;
  sqlite3_bind_text(clear,1,ds_server,-1,SQLITE_STATIC);sqlite3_bind_text(clear,2,id,-1,SQLITE_STATIC);int cleared=sqlite3_step(clear)==SQLITE_DONE;sqlite3_finalize(clear);if(!cleared)return 0;
  int before = *saved;
  int64_t cursor = 0;
  item_t r = {0};
  int rc;
  while ((rc = next_item("tracks", TRACKS, &cursor, &r)) == 1) {
    if (atomic_load(&ds_abort_job) || atomic_load(&ds_stopping) ||
        !download_track(r, saved, skipped) || !member_save(id,&r))
      return 0;
  }
  if (rc == 0 && *saved > before)
    album_cover(id);
  return rc == 0;
}
static int download_picks(item_t *picks, int n) {
  int saved = 0, skipped = 0, ok = 1;
  for (int i = 0; i < n && ok; i++) {
    if (atomic_load(&ds_abort_job) || atomic_load(&ds_stopping)) {
      ok = 0;
      break;
    }
    item_t *r = &picks[i];
    if (r->kind == TRACKS)
      ok = download_track(*r, &saved, &skipped);
    else if (r->kind == ALBUMS)
      ok = download_album(r->id, &saved, &skipped);
    else if (r->kind == ARTISTS) {
      char id[500], args[600];
      ds_url_encode(r->id, id, sizeof id);
      snprintf(args, sizeof args, "id=%s", id);
      ok = fetch("getArtist", args, "artist_jobs");
      int64_t cursor = 0;
      item_t album = {0};
      int rc = 0;
      while (ok &&
             (rc = next_item("artist_jobs", ALBUMS, &cursor, &album)) == 1)
        ok = download_album(album.id, &saved, &skipped);
      if (rc < 0)
        ok = 0;
    }
  }
  if (ok)
    ds_status_text("Done: %d downloaded, %d already local.\nUse Disco indexing to "
                "add new files.",
                saved, skipped);
  else if (atomic_load(&ds_abort_job))
    ds_status_text("Cancelled. Completed files kept.");
  return ok;
}
void *shelf_worker(void *arg) {
  (void)arg;
  char path[1200];
  snprintf(path, sizeof path, "%s/catalog.sqlite", ds_state_dir);
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                      NULL) != SQLITE_OK)
    goto fail;
  sqlite3_busy_timeout(db, 1000);
  sqlite3_progress_handler(db, 1000, sql_cancel, NULL);
  sqlite3_exec(
      db,
      "PRAGMA cache_size=-128; PRAGMA mmap_size=0; PRAGMA temp_store=FILE; "
      "PRAGMA journal_mode=DELETE; PRAGMA max_page_count=32768",
      NULL, NULL, NULL);
  if (!execsql("CREATE TABLE IF NOT EXISTS album_members(server TEXT,album TEXT,id TEXT,path TEXT,size INTEGER,PRIMARY KEY(server,album,id));CREATE TABLE IF NOT EXISTS downloads(server TEXT,id TEXT,path "
               "TEXT,size INTEGER,title TEXT,PRIMARY KEY(server,id)); DROP "
               "TABLE IF EXISTS album_build; DROP TABLE IF EXISTS album_catalogue; DROP TABLE IF EXISTS artists; DROP TABLE IF EXISTS browse; DROP "
               "TABLE IF EXISTS tracks; DROP TABLE IF EXISTS artist_jobs"))
    goto fail;
  ds_status_text("Checking existing Disc library...");
  if (!build_local())
    goto fail;
  ds_status_text("Loading artists...");
  int warmed=fetch("getArtists","","artists");
  pthread_mutex_lock(&ds_shared.mu);
  ds_shared.busy = 0;
  snprintf(ds_shared.message, sizeof ds_shared.message, warmed?"Ready to browse":"Cannot load artists. Tap Browse to retry.");
  ds_shared.revision++;
  pthread_mutex_unlock(&ds_shared.mu);
  while (!atomic_load(&ds_stopping)) {
    int command;
    request_t req;
    int n = 0;
    pthread_mutex_lock(&ds_shared.mu);
    while (!ds_shared.command && !atomic_load(&ds_stopping))
      pthread_cond_wait(&ds_shared.wake, &ds_shared.mu);
    if (atomic_load(&ds_stopping)) {
      pthread_mutex_unlock(&ds_shared.mu);
      break;
    }
    command = ds_shared.command;
    ds_shared.command = 0;
    req = ds_shared.req;
    if (command == 2) {
      n = ds_shared.npicks;
    }
    pthread_mutex_unlock(&ds_shared.mu);
    int previous_pm=command==2?ds_wifi_copy_begin():-1;
    int ok = command == 1 || command == 3 ? browse(req) : download_picks(ds_shared.picks, n);
    ds_wifi_copy_end(previous_pm);
    pthread_mutex_lock(&ds_shared.mu);
    if(!ok && command!=2){ds_shared.count=0;ds_shared.total_count=0;ds_shared.view=req.view;ds_shared.letter=req.letter;}
    ds_shared.busy = 0;
    ds_shared.result_generation=req.generation;
    ds_shared.done = command == 2 ? (ok ? 1 : 2) : 0;
    ds_shared.revision++;
    if (!ok && !ds_shared.message[0])
      snprintf(ds_shared.message, sizeof ds_shared.message,
               "Request failed. Check server or Wi-Fi.");
    pthread_mutex_unlock(&ds_shared.mu);
  }
  shelf_close();
  return NULL;
fail:
  ds_status_text("Cannot check local library or open SD catalogue.\nDownloads "
              "blocked; check SD/config.");
  pthread_mutex_lock(&ds_shared.mu);
  ds_shared.busy = 0;
  ds_shared.fatal = 1;
  ds_shared.revision++;
  pthread_mutex_unlock(&ds_shared.mu);
  shelf_close();
  return NULL;
}
void shelf_close(void) {
  if (localdb) {
    sqlite3_close(localdb);
    localdb = NULL;
  }
  if (db) {
    /* Scratch tables are replaced on next launch. Avoid lengthy SD writes
     * while the user is waiting for Exit. */
    sqlite3_progress_handler(db, 0, NULL, NULL);
    execsql("ROLLBACK");
    sqlite3_close(db);
    db = NULL;
  }
  if(response[0])unlink(response);response[0]=0;
  extern char ds_password[256];ds_secret_clear(ds_password,256);
  if (lockfd >= 0) {
    close(lockfd);
    lockfd = -1;
  }
}
