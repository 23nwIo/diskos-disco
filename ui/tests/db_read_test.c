/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <time.h>
static char test_database[256];
#define DB_PATH test_database
#include "../musicdb.c"
/* Stand-ins for the SD gates (main.c, sdio.c) and the firmware EQ check (fwcaps.c), which need the device; this test
 * only reads and writes song.db. The Makefile links the real bookmarks.c + sdstate.c and jsmn (jsmn_impl.c). */
int sd_io_begin(void){return 1;}
void sd_io_end(void){}
int sd_write_begin(void){return 1;}
void sd_write_end(void){}
int fw_custom_peq_curve_writecheck(const int *hz,int count){(void)hz;(void)count;return 0;}
static long long milliseconds(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000LL+t.tv_nsec/1000000;}
static void *reader_thread(void *arg){
    sqlite3*d=db_read();assert(d&&d!=arg);
    sqlite3_stmt*s;assert(sqlite3_prepare_v2(d,"SELECT TITLE FROM SONG",-1,&s,NULL)==SQLITE_OK);assert(sqlite3_step(s)==SQLITE_ROW);sqlite3_finalize(s);return NULL;
}
int main(void){
    char directory[]="/tmp/disco-db-read-XXXXXX";assert(mkdtemp(directory));snprintf(test_database,sizeof test_database,"%s/song.db",directory);
    sqlite3 *external;assert(sqlite3_open(DB_PATH,&external)==SQLITE_OK);
    assert(sqlite3_exec(external,"CREATE TABLE SONG(ID INTEGER,TITLE TEXT,PATH TEXT); INSERT INTO SONG VALUES(1,'Original','/track.flac');",NULL,NULL,NULL)==SQLITE_OK);
    sqlite3*w=db();assert(w);sqlite3*r=db_read();assert(r&&r!=w);
    assert(sqlite3_exec(external,"BEGIN EXCLUSIVE;",NULL,NULL,NULL)==SQLITE_OK);
    long long start=milliseconds();char path[64];assert(!mdb_song_path(1,path,sizeof path));assert(milliseconds()-start<50);
    assert(sqlite3_exec(external,"COMMIT;",NULL,NULL,NULL)==SQLITE_OK);
    assert(mdb_song_path(1,path,sizeof path)&&!strcmp(path,"/track.flac"));
    assert(sqlite3_exec(w,"BEGIN IMMEDIATE; UPDATE SONG SET TITLE='Changed';",NULL,NULL,NULL)==SQLITE_OK);
    assert(db_read()==w);sqlite3_stmt*s;assert(sqlite3_prepare_v2(db_read(),"SELECT TITLE FROM SONG",-1,&s,NULL)==SQLITE_OK);assert(sqlite3_step(s)==SQLITE_ROW&&!strcmp((const char*)sqlite3_column_text(s,0),"Changed"));sqlite3_finalize(s);
    assert(sqlite3_exec(w,"COMMIT;",NULL,NULL,NULL)==SQLITE_OK);assert(db_read()==r);
    pthread_t t;assert(!pthread_create(&t,NULL,reader_thread,r));pthread_join(t,NULL);
    sqlite3_close(external);unlink(DB_PATH);rmdir(directory);
    puts("PASS: UI read returns promptly under exclusive lock, recovers, sees own uncommitted writes, and isolates worker readers");
}
