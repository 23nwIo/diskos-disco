#!/usr/bin/env python3
"""Production downloader: default skips, owned replacement, cancellation, unrelated files."""
from pathlib import Path
import subprocess,tempfile,os
ui=Path(__file__).resolve().parents[1]
c=r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "discshelf/engine.c"
shared_t *ds_context;
char ds_password[256];
void ds_secret_clear(void*p,size_t n){memset(p,0,n);}
char ds_server[1024]="http://server",ds_download_root[1024],ds_state_dir[1100];
static int fail, cancel, serial;
int sd_io_allowed(void){return 1;}
void ds_status_text(const char *fmt,...){(void)fmt;}
int ds_api_fetch(const char *e,const char *a,const char *p,int64_t max,int64_t expected,int media){
 if(fail)return 0;FILE *f=fopen(p,"w");assert(f);fprintf(f,"fLaC%04d",++serial);fclose(f);
 if(cancel)atomic_store(&ds_abort_job,1);return 1;
}
void ds_url_encode(const char *s,char *o,size_t n){snprintf(o,n,"%s",s);}
static void check(const char *p,const char *v){char b[20]={0};FILE*f=fopen(p,"r");assert(f);fread(b,1,sizeof b-1,f);fclose(f);assert(!strcmp(b,v));}
int main(int argc,char**argv){
 ds_context=calloc(1,sizeof *ds_context);assert(ds_context);testmode=1;
 snprintf(ds_download_root,sizeof ds_download_root,"%s",argv[1]);assert(sqlite3_open(":memory:",&db)==SQLITE_OK);
 assert(execsql("CREATE TABLE downloads(server TEXT,id TEXT,path TEXT,size INTEGER,title TEXT,PRIMARY KEY(server,id));CREATE TABLE local(title TEXT,artist TEXT,album TEXT,path TEXT)"));
 item_t r={.kind=TRACKS,.size=8,.disc=1,.track=1};strcpy(r.id,"one");strcpy(r.name,"Song");strcpy(r.artist,"Artist");strcpy(r.album,"Album");strcpy(r.suffix,"flac");
 int saved=0,skipped=0;assert(download_track(r,&saved,&skipped));assert(saved==1);
 sqlite3_stmt*q;assert(sqlite3_prepare_v2(db,"SELECT path FROM downloads",-1,&q,NULL)==SQLITE_OK);assert(sqlite3_step(q)==SQLITE_ROW);
 char path[1600];snprintf(path,sizeof path,"%s",sqlite3_column_text(q,0));sqlite3_finalize(q);check(path,"fLaC0001");
 assert(download_track(r,&saved,&skipped)&&skipped==1&&saved==1);check(path,"fLaC0001");
 ds_shared.replace=1;fail=1;assert(!download_track(r,&saved,&skipped));check(path,"fLaC0001");fail=0;
 cancel=1;assert(!download_track(r,&saved,&skipped));check(path,"fLaC0001");cancel=0;atomic_store(&ds_abort_job,0);
 assert(download_track(r,&saved,&skipped));check(path,"fLaC0003");assert(saved==2);
 /* A local tag match does not grant overwrite ownership: the user's own file with the same tags is skipped, not
  * replaced. (A local row for a file this server downloaded under another ID is no match: duplicate() excludes it.) */
 char user[1200];snprintf(user,sizeof user,"%s/user-Other.flac",argv[1]);{FILE*f=fopen(user,"w");assert(f);fputs("USERFILE",f);fclose(f);}
 assert(sqlite3_prepare_v2(db,"INSERT INTO local VALUES('other','artist','album',?)",-1,&q,NULL)==SQLITE_OK);sqlite3_bind_text(q,1,user,-1,SQLITE_TRANSIENT);assert(sqlite3_step(q)==SQLITE_DONE);sqlite3_finalize(q);
 strcpy(r.id,"two");strcpy(r.name,"Other");int oldserial=serial;assert(download_track(r,&saved,&skipped));assert(serial==oldserial);check(user,"USERFILE");check(path,"fLaC0003");
 /* Missing member of a partial album is still downloaded. */
 ds_shared.replace=0;strcpy(r.id,"three");strcpy(r.name,"Missing");assert(download_track(r,&saved,&skipped));assert(saved==3);
 shelf_close();free(ds_context);puts("PASS partial album missing track, default skip, exact-ID atomic replacement, failure/cancel retain original, tag-only match preserved");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'t.c').write_text(c)
 subprocess.run(['cc','-std=gnu11','-g','-fsanitize=address,undefined','-ffunction-sections','-fdata-sections','-pthread','-I'+str(ui),str(p/'t.c'),str(ui/'sqlite3.c'),'-DSQLITE_THREADSAFE=1','-DSQLITE_OMIT_LOAD_EXTENSION','-lm','-Wl,--gc-sections','-o',str(p/'t')],check=True)
 subprocess.run([str(p/'t'),str(p)],check=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
