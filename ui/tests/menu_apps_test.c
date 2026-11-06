/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "menu_apps.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef MENU_APPS_DIR
#error Test must use a private temporary app directory
#endif
static const char codes[]="mlsoceFaudbwrgqMRB";
#define NB 18
/* An app folder counts only with an executable "app" inside. */
static void mkapp(const char *name){char p[512];snprintf(p,sizeof p,MENU_APPS_DIR "/%s",name);assert(mkdir(p,0700)==0);
    snprintf(p,sizeof p,MENU_APPS_DIR "/%s/app",name);FILE *f=fopen(p,"w");assert(f);fclose(f);assert(chmod(p,0700)==0);}
static void rmapp(const char *name){char p[512];snprintf(p,sizeof p,MENU_APPS_DIR "/%s/app",name);unlink(p);
    snprintf(p,sizeof p,MENU_APPS_DIR "/%s/app.conf",name);unlink(p);snprintf(p,sizeof p,MENU_APPS_DIR "/%s",name);assert(rmdir(p)==0);}
static int find_name(const char *name){for(int i=0;i<menu_apps_count();i++)if(!strcmp(menu_apps_get(i)->name,name))return i;return -1;}
int main(void){
    assert(mkdir(MENU_APPS_DIR,0700)==0);
    for(int i=0;i<300;i++){char name[16];snprintf(name,sizeof name,"app%03d",i);mkapp(name);}
    mkapp("boomshine");mkapp("album-roulette");mkapp("disc-shelf");   /* built in: never listed twice */
    {char p[512];snprintf(p,sizeof p,MENU_APPS_DIR "/no-exec");assert(mkdir(p,0700)==0);}   /* nothing to run: hidden */
    char conf[512];snprintf(conf,sizeof conf,MENU_APPS_DIR "/app299/app.conf");FILE *f=fopen(conf,"w");assert(f);fputs("name=My Game\r\nexec=" MENU_APPS_DIR "/app299/app\r\n",f);fclose(f);
    cfg_load();cfg_set_str("disco_mnu","mlsoce");menu_apps_scan();assert(menu_apps_count()==302);
    int shelf=find_name("Disc Shelf");assert(shelf>=0&&!strcmp(menu_apps_get(shelf)->exec,"builtin:disc-shelf"));
    int radio=find_name("Radio");assert(radio>=0&&!strcmp(menu_apps_get(radio)->exec,"builtin:radio"));
    int app=find_name("My Game"),other=find_name("app298");assert(app>=0&&other>=0);
    int items[]={0,2,NB+app,NB+other};assert(menu_apps_save(items,4,codes)==0);
    char saved[48];snprintf(saved,sizeof saved,"%s",cfg_get_str("disco_mnu",""));
    assert(strlen(saved)==4&&saved[0]=='m'&&saved[1]=='s');
    char id[33];snprintf(id,sizeof id,"%s",menu_apps_get(app)->id);
    assert(!strcmp(menu_apps_get(app)->exec,MENU_APPS_DIR "/app299/app"));
    /* A newly installed app changes catalog ordering but must not retarget saved entries. */
    mkapp("000-new-app");
    menu_apps_scan();assert(menu_apps_count()==303);
    int resolved=menu_apps_decode(saved[2],codes)-NB;
    assert(resolved>=0&&!strcmp(menu_apps_get(resolved)->id,id));
    int reorder[]={0,NB+find_name("app298"),2,NB+resolved};assert(menu_apps_save(reorder,4,codes)==0);
    assert(menu_apps_decode(cfg_get_str("disco_mnu","")[3],codes)==NB+resolved);
    /* Removed apps cannot silently launch a different app; reinstall restores identity. */
    rmapp("app299");
    menu_apps_scan();assert(menu_apps_decode(saved[2],codes)==-1);
    mkapp("app299");menu_apps_scan();resolved=menu_apps_decode(saved[2],codes)-NB;
    assert(resolved>=0&&!strcmp(menu_apps_get(resolved)->id,id));
    /* Removing and adding another app reuses a free mapping, not an active mapping. */
    int small[]={0,2};assert(menu_apps_save(small,2,codes)==0);
    for(int i=0;i<25;i++){int next[]={0,2,NB+i};assert(menu_apps_save(next,3,codes)==0);assert(menu_apps_decode(cfg_get_str("disco_mnu","")[2],codes)==NB+i);assert(menu_apps_save(small,2,codes)==0);}
    assert(menu_apps_decode('?',codes)==-1&&menu_apps_decode('m',codes)==0);
    assert(menu_apps_get(-1)==NULL&&menu_apps_get(99999)==NULL);
    puts("PASS: 300+ arbitrary apps, CRLF metadata, stable identity, reorder, future installs, removal/reinstall and mapping reuse");
}
