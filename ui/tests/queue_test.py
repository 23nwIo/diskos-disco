#!/usr/bin/env python3
"""Real queue backend and SQLite plans; UI, IPC and SD admission are boundaries."""
import os
from pathlib import Path
import subprocess
import tempfile
app=Path(__file__).resolve().parents[1]
queue=(app/'queue.c').read_text().split('/* ================================================================ the Queue screen */')[0]
stubs=r'''
#include <assert.h>
#include "modes.h"
#include "sdio.h"
static int local=1,slot_fail,rest_fail,sends,restores,last_pos,leases;
static track_state_t state;
int sd_io_begin(void){ if(!local) return 0; leases++;return 1; }
void sd_io_end(void){ assert(leases>0); leases--; }
int modes_output_busy(void){return 0;}
int ui_local_playback_allowed(void){return local;}
void ipc_get_state(track_state_t *out){*out=state;}
int cfg_get_int(const char *key,int fallback){(void)key;return fallback;}
static int work_mode,party;
int simple_cfg_get_int(const char *key,int fallback){return strcmp(key,"work_mode")?cfg_get_int(key,fallback):work_mode;}
int party_mode(void){return party;}
#include <time.h>
static int real_ticks;                                   /* 0: the clock stands at 10000 (most tests); 1: real time */
static uint32_t mono_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint32_t)(t.tv_sec*1000+t.tv_nsec/1000000);}
uint32_t lv_tick_get(void){static uint32_t base;if(!real_ticks)return 10000;if(!base)base=mono_ms();return 10000+(mono_ms()-base);}
uint32_t lv_tick_elaps(uint32_t t){return lv_tick_get()-t;}
void ui_queue_changed(void){}
void ui_toast(const char *s){(void)s;}
int screen_current(void){return SCR_NOWPLAYING;}
int ui_play_context(int *type,char *name,int cap,long *pid){ *type=3;snprintf(name,(size_t)cap,"Album");*pid=0;return 3;}
static int playing_now;
int ui_is_playing(void){return playing_now;}
int ui_play_slot(int pos){sends++;last_pos=pos;return !slot_fail;}
int ui_play_restore(int type,const char *name,long pid,int pos){(void)name;(void)pid;assert(type==3);restores++;last_pos=pos;return !rest_fail;}
static void view_reload(void){}
'''
test=r'''
#include <pthread.h>
#include <unistd.h>
static void *release_later(void *p){usleep(300000);char *e=NULL;sqlite3_exec((sqlite3*)p,"COMMIT;",0,0,&e);sqlite3_free(e);return NULL;}
static void sql(sqlite3 *db,const char *query){char *err=NULL;int rc=sqlite3_exec(db,query,0,0,&err);if(rc) fprintf(stderr,"%s: %s\n",query,err);sqlite3_free(err);assert(rc==SQLITE_OK);}
int main(int argc,char **argv){
    assert(argc==2);sqlite3 *db=NULL;assert(sqlite3_open(argv[1],&db)==SQLITE_OK);
    sql(db,"CREATE TABLE SONG(ID INTEGER PRIMARY KEY,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST,IS_M3U,M3U_PATH);");
    sql(db,"CREATE TABLE CUSTOM_PLAYLIST_INDEX(LIST_ID INTEGER PRIMARY KEY,LIST_NAME,M3U_PATH);");
    sql(db,"CREATE TABLE CUSTOM_PLAYLIST(ID INTEGER PRIMARY KEY,PLAYLIST_ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION,ALBUM_ARTIST,IS_M3U,M3U_PATH,UNIQUE(PLAYLIST_ID,PATH,TRACK,IS_CUE,IS_ISO));");
    sql(db,"CREATE TABLE LIST_SONG_0(ID INTEGER PRIMARY KEY,PATH,TRACK,IS_CUE,IS_ISO,TITLE);");
    sql(db,"INSERT INTO SONG VALUES(10,'/album.flac','album.flac','First','Album','Artist','',2,1,1,0,0,0,10000,'Artist',1,'/list.m3u'),(11,'/album.flac','album.flac','Second','Album','Artist','',2,2,1,0,0,10000,12000,'Artist',1,'/list.m3u'),(20,'/next.flac','next.flac','Next','Album','Artist','',2,3,0,0,0,0,14000,'Artist',0,'');");
    sql(db,"INSERT INTO LIST_SONG_0 VALUES(1,'/album.flac',1,1,0,'First'),(2,'/album.flac',2,1,0,'Second'),(3,'/next.flac',3,0,0,'Next');");
    assert(mdb_queue_current_id("/album.flac",0,"")==0);
    assert(mdb_queue_current_id("/album.flac",2,"Second")==11);
    assert(mdb_queue_current_id("/album.flac",2,"First")==0);
    assert(mdb_queue_current_id("/next.flac",0,"")==20);
    int ids[8];assert(mdb_queue_live_ids(ids,8)==3 && ids[0]==10 && ids[1]==11 && ids[2]==20);
    assert(mdb_queue_live_ids(ids,2)==-1);
    sql(db,"CREATE TABLE SONG_BAK AS SELECT * FROM SONG WHERE ID=20;");
    sql(db,"UPDATE SONG_BAK SET ID=99;INSERT INTO SONG SELECT * FROM SONG_BAK;UPDATE SONG_BAK SET ID=20;");   /* same identity twice */
    assert(mdb_queue_live_ids(ids,8)==-1);
    sql(db,"DELETE FROM SONG WHERE ID IN (20,99);");                          /* and none at all: missing */
    assert(mdb_queue_live_ids(ids,8)==-1);
    sql(db,"INSERT INTO SONG SELECT * FROM SONG_BAK;DROP TABLE SONG_BAK;");
    assert(mdb_queue_live_ids(ids,8)==3 && ids[2]==20);
    puts("PASS the player's list maps in one query; an ambiguous or missing row is an error, never guessed or dropped");
    puts("PASS ambiguous subtracks require verified row/title; live queues retain exact track identity");
    state.have_track=1;state.pos_id=1;strcpy(state.path,"/album.flac");strcpy(state.title,"First");
    assert(queue_add_path("/album.flac")==2 && g_q[0].id==10 && g_q[1].id==11);
    local=0;assert(queue_next()==-1 && sends==0 && queue_count()==2);local=1;
    slot_fail=1;assert(queue_next()==-1 && queue_count()==2 && !g_active);slot_fail=0;
    assert(queue_next()==1 && queue_count()==1 && g_q[0].id==11 && g_active && last_pos==1);
    sqlite3_stmt *st;assert(sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM CUSTOM_PLAYLIST WHERE IS_M3U=1 AND M3U_PATH='/list.m3u';",-1,&st,0)==SQLITE_OK);assert(sqlite3_step(st)==SQLITE_ROW && sqlite3_column_int(st,0)==2);sqlite3_finalize(st);
    state.pos_id=2;strcpy(state.title,"Second");queue_tick(&state,1);assert(queue_count()==0);
    state.pos_id=3;strcpy(state.path,"/next.flac");strcpy(state.title,"Next");queue_tick(&state,1);
    assert(!g_active && restores==1 && last_pos==3 && !leases);
    puts("PASS same-file CUE transitions, V2.57 M3U fields, IPC failure retention, SD hold and exact context restoration");
    assert(queue_add_path("/next.flac")==1);g_last_id=0;g_prev2=0;
    assert(queue_next()==1);rest_fail=1;hand_back(20);assert(g_active);rest_fail=0;hand_back(20);assert(!g_active);
    assert(queue_add_path("/next.flac")==1);int old=sends;sql(db,"UPDATE SONG SET PATH='/replacement.flac' WHERE ID=20;");
    assert(!play_copy(0) && sends==old && queue_count()==1);
    puts("PASS failed restoration and stale IDs do not consume queue state");
    /* 2.5.1 regression: the player writes the database at every song change and locks out readers for a moment.
     * Queue lookups must wait briefly instead of giving up (they gave up at once, so the queue was skipped). */
    queue_clear();sql(db,"UPDATE SONG SET PATH='/next.flac' WHERE ID=20;");
    g_active=0;g_last_id=0;g_prev2=0;g_skip_change=0;
    state.have_track=1;state.pos_id=1;strcpy(state.path,"/album.flac");strcpy(state.title,"First");
    assert(queue_add_path("/next.flac")==1);
    sqlite3 *player=NULL;assert(sqlite3_open(argv[1],&player)==SQLITE_OK);
    sql(player,"BEGIN EXCLUSIVE;");
    pthread_t th;assert(pthread_create(&th,NULL,release_later,player)==0);
    old=sends;int q=queue_next();pthread_join(th,NULL);sqlite3_close(player);
    assert(q==1 && sends==old+1 && queue_count()==0 && g_active);
    puts("PASS queue waits briefly for the player's database write instead of skipping the queued song");
    /* Nothing playing: Play/Next start the queue itself (it failed with "Couldn't start queued song") */
    queue_clear();g_active=0;g_last_id=0;g_prev2=0;restores=0;
    memset(&state,0,sizeof state);playing_now=0;
    assert(queue_add_path("/next.flac")==1 && queue_add_path("/album.flac")==2 && queue_count()==3);
    old=sends;assert(queue_next()==1 && sends==old+1 && g_active && queue_count()==2 && g_q[0].id==10 && g_nrest==0);
    state.have_track=1;strcpy(state.path,"/next.flac");strcpy(state.title,"Next");state.pos_id=last_pos;state.duration_ms=14000;
    queue_tick(&state,1);assert(queue_count()==2);                 /* the started song itself: nothing moves */
    state.position_ms=13500;queue_tick(&state,1);
    strcpy(state.path,"/album.flac");strcpy(state.title,"First");state.pos_id=idx_of(g_copy,g_ncopy,10)+1;state.position_ms=0;state.duration_ms=10000;
    queue_tick(&state,1);assert(queue_count()==1 && g_q[0].id==11);  /* natural end: the next queued song, as planned */
    puts("PASS with nothing playing, the queue starts with its first song and plays through in order");
    queue_clear();g_active=0;g_last_id=0;memset(&state,0,sizeof state);
    assert(queue_add_path("/album.flac")==2);old=sends;queue_jump(1);
    assert(sends==old+1 && queue_count()==0 && g_active);
    puts("PASS Queue screen Play/tap with nothing playing starts the chosen song");
    /* Review #2: a failed fresh start must not drop or reorder queued songs */
    queue_clear();g_active=0;g_last_id=0;memset(&state,0,sizeof state);playing_now=0;
    assert(queue_add_path("/next.flac")==1 && queue_add_path("/album.flac")==2 && queue_count()==3);
    slot_fail=1;queue_jump(1);slot_fail=0;
    assert(queue_count()==3 && g_q[0].id==20 && g_q[1].id==10 && g_q[2].id==11 && !g_active);
    old=sends;assert(queue_next()==1 && queue_count()==2);    /* and it still starts normally afterwards */
    puts("PASS a failed start leaves the queue exactly as it was");
    /* Review #3: queuing one CUE track adds that track only, with its identity; several fit or none */
    queue_clear();g_active=0;
    {int one[]={11};assert(queue_add_ids(one,1)==1 && queue_count()==1 && g_q[0].id==11 && g_q[0].track==2 && g_q[0].cue==1);}
    {int two[]={10,20};assert(queue_add_ids(two,2)==2 && queue_count()==3 && g_q[1].id==10 && g_q[2].id==20);}
    {static int many[QMAX+1];for(int k=0;k<QMAX+1;k++)many[k]=20;assert(queue_add_ids(many,QMAX+1)==-1 && queue_count()==3);}
    puts("PASS multi-select queues exact songs (one CUE track stays one track); too many is refused, not cut off");
    /* Audit #2: a database locked the whole time costs one wait, not one per song; nothing is added */
    {sqlite3 *player=NULL;assert(sqlite3_open(argv[1],&player)==SQLITE_OK);sql(player,"BEGIN EXCLUSIVE;");
     int before=queue_count();int ids[]={10,11,20};real_ticks=1;
     struct timespec t0,t1;clock_gettime(CLOCK_MONOTONIC,&t0);
     int r=queue_add_ids(ids,3);
     clock_gettime(CLOCK_MONOTONIC,&t1);double sec=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
     real_ticks=0;sql(player,"COMMIT;");sqlite3_close(player);
     assert(r==-2 && queue_count()==before && sec<2.5);
     printf("PASS a locked database stops a bulk add after one wait (%.1f s), adding nothing\n",sec);}
    /* Jo 2026-11-15: songs queued from another card blocked Next ("Queue song unavailable") after a card swap.
     * A queued song whose file is gone from a mounted card is dropped; Next then goes on normally. */
    queue_clear();g_active=0;g_last_id=0;g_prev2=0;memset(&state,0,sizeof state);
    {char q[600];snprintf(q,sizeof q,"INSERT INTO SONG(ID,PATH,NAME,TITLE,ALBUM,ARTIST,GENRE,DISC,TRACK,IS_CUE,IS_ISO,IS_DSD,OFFSET,DURATION) VALUES(30,'%s','db','OnCard','Album','Artist','',1,1,0,0,0,0,1000);",argv[1]);sql(db,q);}
    assert(queue_add_path("/next.flac")==1 && queue_add_path(argv[1])==1 && queue_count()==2);
    g_card="/";g_card_parent="/proc";                         /* "mounted": / and /proc are different file systems */
    old=sends;queue_jump(1);
    assert(sends==old+1 && queue_count()==0 && g_active && g_last_id==30);   /* index followed the removal */
    queue_clear();g_active=0;assert(queue_add_path("/next.flac")==1);
    old=sends;assert(queue_next()==0 && sends==old && queue_count()==0);     /* nothing left: the player's own Next */
    g_card="/proc";g_card_parent="/proc";                     /* card not mounted: nothing is dropped */
    assert(queue_add_path("/next.flac")==1);assert(prune_missing(NULL)==0 && queue_count()==1);
    g_card="/tmp/sdcard";g_card_parent="/tmp";queue_clear();
    puts("PASS queued songs that are not on the mounted card are dropped and never block Next");
    /* Jo 2026-11-22: a library of more than RMAX songs can't be held as "the list after the queue" (here: a row of
     * the player's list with no song). The queue then waited for Next; now it starts at the next song change, on its
     * own, and that song comes back after it. In party mode there is no way back (After the List Ends follows). */
    queue_clear();g_active=0;g_last_id=0;g_prev2=0;g_skip_change=0;g_resume_type=-1;restores=0;memset(&state,0,sizeof state);
    sql(db,"INSERT INTO LIST_SONG_0 VALUES(4,'/gone.flac',0,0,0,'Gone');");
    for(party=0;party<2;party++){
        queue_clear();g_active=0;g_last_id=0;g_prev2=0;g_skip_change=0;g_resume_type=-1;restores=0;
        state.have_track=1;state.pos_id=1;strcpy(state.path,"/album.flac");strcpy(state.title,"First");state.duration_ms=10000;state.position_ms=0;
        queue_tick(&state,1);
        assert(queue_add_path("/next.flac")==1);
        state.position_ms=9800;queue_tick(&state,1);
        state.pos_id=2;strcpy(state.title,"Second");state.position_ms=0;old=sends;queue_tick(&state,1);
        assert(sends==old+1 && queue_count()==0 && g_active && g_nrest==0);
        assert(party ? g_resume_type==-1 : (g_resume_type==3 && g_resume_pos==2));
        strcpy(state.path,"/next.flac");strcpy(state.title,"Next");state.pos_id=last_pos;state.duration_ms=14000;state.position_ms=13800;
        queue_tick(&state,1);queue_tick(&state,0);                 /* the queued song ended and the player stopped */
        assert(party ? restores==0 : (restores==1 && last_pos==2 && !g_active));
    }
    sql(db,"DELETE FROM LIST_SONG_0 WHERE ID=4;");
    puts("PASS an unreadable (too long) list: the queue starts at the next song change; party mode never goes back");
    queue_clear();g_active=0;g_last_id=0;memset(&state,0,sizeof state);party=1;work_mode=1;
    assert(queue_add_path("/next.flac")==1 && queue_add_path("/album.flac")==2);
    for(int k=0;k<20;k++){assert(queue_next()==1 && queue_count()==2 && g_q[0].id==10 && g_q[1].id==11);
        queue_clear();g_active=0;assert(queue_add_path("/next.flac")==1 && queue_add_path("/album.flac")==2);}
    queue_clear();party=0;work_mode=0;
    puts("PASS party mode keeps the guests' order in the Shuffle play mode");
    {FILE *f=fopen(QUEUE_LOG,"r");assert(f);char l[400];int way=0,party_line=0;
     while(fgets(l,sizeof l,f)){if(strstr(l,"way back: list type 3"))way=1;if(strstr(l,"way back: none (party mode"))party_line=1;}
     fclose(f);assert(way&&party_line);}
    puts("PASS the decision log says why the queue went back or not");
    assert(!leases);sqlite3_close(db);
    puts("HARNESS COMPLETE");
}
'''
with tempfile.TemporaryDirectory(prefix='diskos-queue-test-') as tmp:
    root=Path(tmp);db=root/'song.db';qfile=root/'queue.tsv';qlog=root/'queue.log'
    (root/'db.c').write_text((app/'musicdb.c').read_text().replace('"/usr/data/fiio/db/song.db"','"'+str(db)+'"'))
    (root/'test.c').write_text(queue+stubs+'\n#include "sqlite3.h"\n'+test)
    exe=root/'test'
    subprocess.run([os.environ.get('CC','cc'),'-O1','-g','-std=gnu11','-D_GNU_SOURCE','-DLV_CONF_INCLUDE_SIMPLE','-DQUEUE_FILE="'+str(qfile)+'"','-DQUEUE_LOG="'+str(qlog)+'"','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-I'+str(app),str(root/'test.c'),str(root/'db.c'),str(app/'sqlite3.c'),'-pthread','-lm','-ldl','-Wl,--gc-sections','-o',str(exe)],check=True)
    r=subprocess.run([str(exe),str(db)],text=True,capture_output=True,timeout=20)
    print(r.stdout,end='');print(r.stderr,end='')
    assert r.returncode==0 and r.stdout.splitlines()[-1]=='HARNESS COMPLETE'
print('COMPLETE queue_test.py')
