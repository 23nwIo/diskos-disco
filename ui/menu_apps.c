#include "simple_theme.h"
/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "menu_apps.h"
#include "config.h"
#include "md5.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef MENU_APPS_DIR
#define MENU_APPS_DIR "/usr/data/apps"
#endif
#define MAPS 10
static menu_app_t *apps;
static int count;
static int compare(const void *a,const void *b){
    const menu_app_t *x=a,*y=b;int n=strcmp(x->name,y->name);
    return n?n:strcmp(x->id,y->id);
}
void menu_apps_scan(void){
    menu_app_t *next=calloc(16,sizeof *next);if(!next)return;int n=2,capacity=16;
    md5_hex("disc-shelf",10,next[0].id);strcpy(next[0].name,"Disc Shelf");strcpy(next[0].exec,"builtin:disc-shelf");strcpy(next[0].tagline,"Your music, to go.");strcpy(next[0].button,"Browse collection");
    md5_hex("builtin:radio",13,next[1].id);strcpy(next[1].name,"Radio");strcpy(next[1].exec,"builtin:radio");strcpy(next[1].tagline,"Stations from the internet.");   /* Web radio (radio.c) */
    strcpy(next[1].about,"Search thousands of stations or\nplay your favourites over Wi-Fi.");strcpy(next[1].button,"Open radio");
    DIR *d=opendir(MENU_APPS_DIR);if(!d){free(apps);apps=next;count=n;return;}
    struct dirent *de;
    while((de=readdir(d))){
        if(de->d_name[0]=='.')continue;
        /* Built into the menu already. */
        if(!strcmp(de->d_name,"album-roulette")||!strcmp(de->d_name,"boomshine")||!strcmp(de->d_name,"songsphere")||!strcmp(de->d_name,"disc-shelf"))continue;   /* SongSphere: built in since 2.2.0 */
        char dir[512],conf[544];struct stat st;
        int len=snprintf(dir,sizeof dir,MENU_APPS_DIR "/%s",de->d_name);
        if(len<0 || (size_t)len>=sizeof dir || stat(dir,&st)<0 || !S_ISDIR(st.st_mode))continue;
        if(n==capacity){
            int cap=capacity?capacity*2:16;
            /* Callback IDs use 16 bits; the player's memory is the practical limit. */
            if(cap>65500)break;
            menu_app_t *p=realloc(next,(size_t)cap*sizeof *p);
            if(!p){closedir(d);free(next);return;}
            next=p;capacity=cap;
        }
        menu_app_t *a=&next[n];memset(a,0,sizeof *a);
        md5_hex(de->d_name,strlen(de->d_name),a->id);
        snprintf(a->name,sizeof a->name,"%.63s",de->d_name);
        len=snprintf(a->exec,sizeof a->exec,"%s/app",dir);
        if(len<0 || (size_t)len>=sizeof a->exec)a->exec[0]=0;
        snprintf(conf,sizeof conf,"%s/app.conf",dir);
        FILE *f=fopen(conf,"r");
        if(f){char line[512];while(fgets(line,sizeof line,f)){
            char *nl=strpbrk(line,"\r\n");if(nl)*nl=0;
            if(!strncmp(line,"name=",5)&&line[5])snprintf(a->name,sizeof a->name,"%.63s",line+5);
            else if(!strncmp(line,"tagline=",8))snprintf(a->tagline,sizeof a->tagline,"%.63s",line+8);
            else if(!strncmp(line,"about=",6)){                   /* "\n" in the file starts a new line */
                snprintf(a->about,sizeof a->about,"%.159s",line+6);
                for(char *q=a->about;(q=strstr(q,"\\n"));){*q='\n';memmove(q+1,q+2,strlen(q+2)+1);}
            }
            else if(!strncmp(line,"button=",7))snprintf(a->button,sizeof a->button,"%.31s",line+7);
            else if(!strncmp(line,"icon=",5)&&line[5]){                 /* relative: inside the app's folder */
                if(line[5]=='/')snprintf(a->icon,sizeof a->icon,"%.255s",line+5);
                else{int k=snprintf(a->icon,sizeof a->icon,"%s/%s",dir,line+5);if(k<0||(size_t)k>=sizeof a->icon)a->icon[0]=0;}
            }
            else if(!strncmp(line,"exec=",5)){
                if(strlen(line+5)<sizeof a->exec)memcpy(a->exec,line+5,strlen(line+5)+1);
                else a->exec[0]=0;
            }
        }fclose(f);}
        if(!a->exec[0]||access(a->exec,X_OK))continue;
        if(!a->icon[0]){char ic[544];snprintf(ic,sizeof ic,"%s/icon.bmp",dir);
            if(strlen(ic)<sizeof a->icon&&!access(ic,R_OK))memcpy(a->icon,ic,strlen(ic)+1);}
        n++;
    }
    closedir(d);
    if(n>1)qsort(next,(size_t)n,sizeof *next,compare);
    free(apps);apps=next;count=n;
}
int menu_apps_count(void){return count;}
const menu_app_t *menu_apps_get(int index){return index>=0&&index<count?&apps[index]:NULL;}
static void map_key(char key[16],int i,const char *prefix){snprintf(key,16,"%s%d",prefix,i);}
int menu_apps_decode_named(char code,const char *builtins,const char *prefix){
    const char *p=strchr(builtins,code);
    if(p&&code)return (int)(p-builtins);
    if(code<'0'||code>'9')return -1;
    char key[16];map_key(key,code-'0',prefix);const char *id=simple_cfg_get_str(key,"");
    for(int i=0;i<count;i++)if(!strcmp(apps[i].id,id))return (int)strlen(builtins)+i;
    return -1;
}
int menu_apps_save_named(const int *items,int n,const char *builtins,const char *config_key,const char *prefix){
    if(n<0||n>MAPS)return -1;
    int nb=(int)strlen(builtins);char codes[MAPS+1]={0},old[48];
    char maps[MAPS][48],prior[MAPS][48];
    snprintf(old,sizeof old,"%s",simple_cfg_get_str(config_key,""));
    for(int i=0;i<MAPS;i++){char key[16];map_key(key,i,prefix);snprintf(maps[i],sizeof maps[i],"%s",simple_cfg_get_str(key,""));}
    memcpy(prior,maps,sizeof prior);
    for(int i=0;i<n;i++){
        if(items[i]<0)return -1;
        if(items[i]<nb){codes[i]=builtins[items[i]];continue;}
        const menu_app_t *app=menu_apps_get(items[i]-nb);if(!app)return -1;
        int slot=-1;
        for(int j=0;j<MAPS;j++)if(!strcmp(maps[j],app->id)){slot=j;break;}
        if(slot<0)for(int j=0;j<MAPS;j++){
            char code=(char)('0'+j);
            if(!strchr(old,code)&&!strchr(codes,code)){slot=j;snprintf(maps[j],sizeof maps[j],"%s",app->id);break;}
        }
        if(slot<0)return -1;
        codes[i]=(char)('0'+slot);
    }
    /* One atomic settings rewrite keeps app identities and ordering together. */
    cfg_begin();
    for(int i=0;i<MAPS;i++)if(strcmp(maps[i],prior[i])){
        char key[16];map_key(key,i,prefix);simple_cfg_set_str_deferred(key,maps[i]);
    }
    simple_cfg_set_str_deferred(config_key,codes);
    if(cfg_commit()==0)return 0;
    cfg_begin();
    for(int i=0;i<MAPS;i++)if(strcmp(maps[i],prior[i])){
        char key[16];map_key(key,i,prefix);simple_cfg_set_str_deferred(key,prior[i]);
    }
    simple_cfg_set_str_deferred(config_key,old);cfg_commit();return -1;
}

int menu_apps_decode(char code,const char*b){return menu_apps_decode_named(code,b,"dm_app");}
int menu_apps_save(const int*i,int n,const char*b){return menu_apps_save_named(i,n,b,"disco_mnu","dm_app");}
