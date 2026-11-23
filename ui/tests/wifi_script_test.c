/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* wifi_script.h: the DHCP hostname (name file, else from the MAC, taken over from an old user script) and the embedded
 * wifi_up.sh, run for real against stand-in rfkill/ifconfig/wpa_supplicant/udhcpc that log their arguments. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "wifi_script.h"

static int fails;
#define EXPECT(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static char dir[64];
static const char *path(const char *n){ static char b[4][160]; static int i; i = (i + 1) % 4; snprintf(b[i], sizeof b[i], "%s/%s", dir, n); return b[i]; }
static void put(const char *p, const char *body, mode_t mode){
    FILE *f = fopen(p, "w"); if(!f){ perror(p); exit(2); }
    fputs(body, f); fclose(f); chmod(p, mode);
}

int main(void){
    char o[64];
    /* names */
    EXPECT(wsc_name_clean("IoMusic\n", o, sizeof o) && !strcmp(o, "IoMusic"));
    EXPECT(wsc_name_clean("  disc-2 \r\nignored", o, sizeof o) && !strcmp(o, "disc-2"));
    EXPECT(!wsc_name_clean("", o, sizeof o)); EXPECT(!wsc_name_clean("\n", o, sizeof o));
    EXPECT(!wsc_name_clean("-x", o, sizeof o)); EXPECT(!wsc_name_clean("x-", o, sizeof o));
    EXPECT(!wsc_name_clean("a b", o, sizeof o)); EXPECT(!wsc_name_clean("a;reboot", o, sizeof o));
    EXPECT(!wsc_name_clean("FIIO_$x", o, sizeof o)); EXPECT(!wsc_name_clean("a_b", o, sizeof o));
    { char l[80]; memset(l, 'a', 64); l[64] = 0; EXPECT(!wsc_name_clean(l, o, sizeof o)); l[63] = 0; EXPECT(wsc_name_clean(l, o, sizeof o)); }
    wsc_name_from_mac("40:fd:f3:eb:1a:2b\n", o, sizeof o); EXPECT(!strcmp(o, "Disc-1A2B"));
    wsc_name_from_mac("40:FD:F3:EB:1A:2B", o, sizeof o);   EXPECT(!strcmp(o, "Disc-1A2B"));
    wsc_name_from_mac("", o, sizeof o);                    EXPECT(!strcmp(o, "Disc"));
    wsc_name_from_mac("40:fd:f3:eb:1a", o, sizeof o);      EXPECT(!strcmp(o, "Disc"));
    wsc_name_from_mac("40:fd:f3:eb:1a:2g", o, sizeof o);   EXPECT(!strcmp(o, "Disc"));
    wsc_name_from_mac(NULL, o, sizeof o);                  EXPECT(!strcmp(o, "Disc"));
    /* taken over from a user copy of the stock script */
    const char *jo = "#!/bin/sh\n\t#\tudhcpc -i wlan0 -x hostname:FIIO_K17&\n\twpa_supplicant -B -i wlan0 -c $wpa_conf &\n"
                     "\tudhcpc -i wlan0 -x hostname:IoMusic &\nfi\n";
    EXPECT(wsc_name_from_script(jo, o, sizeof o) && !strcmp(o, "IoMusic"));          /* not the commented-out line */
    EXPECT(!wsc_name_from_script("\tudhcpc -i wlan0 -x hostname:FIIO_$product_model &\n", o, sizeof o));   /* stock */
    EXPECT(wsc_name_from_script("udhcpc -x hostname:\"my-disc\" -i wlan0\n", o, sizeof o) && !strcmp(o, "my-disc"));
    EXPECT(!wsc_name_from_script("echo hostname:foo\n", o, sizeof o));
    EXPECT(!wsc_name_from_script("", o, sizeof o));
    puts("PASS hostname: valid names only, Disc-XXXX from the MAC, taken over from an old script");

    snprintf(dir, sizeof dir, "/tmp/wsctestXXXXXX");
    if(!mkdtemp(dir)){ perror("mkdtemp"); return 2; }
    put(path("name"), "Kitchen\n", 0644); put(path("mac"), "40:fd:f3:eb:1a:2b\n", 0644);
    wsc_resolve(path("name"), path("mac"), o, sizeof o); EXPECT(!strcmp(o, "Kitchen"));
    put(path("name"), "bad name\n", 0644);
    wsc_resolve(path("name"), path("mac"), o, sizeof o); EXPECT(!strcmp(o, "Disc-1A2B"));   /* invalid file: default */
    wsc_resolve(path("none"), path("mac"), o, sizeof o); EXPECT(!strcmp(o, "Disc-1A2B"));
    wsc_resolve(path("none"), path("nomac"), o, sizeof o); EXPECT(!strcmp(o, "Disc"));
    puts("PASS hostname: name file first, else the MAC");

    /* the script itself, with the name file moved into the test dir */
    char script[2 * sizeof WSC_SCRIPT + 512]; const char *at = strstr(WSC_SCRIPT, "/tmp/diskos-hostname ]");
    EXPECT(at != NULL);
    if(at){
        size_t pre = (size_t)(at - WSC_SCRIPT);
        char tail[sizeof WSC_SCRIPT]; snprintf(tail, sizeof tail, "%s", at + strlen("/tmp/diskos-hostname ]"));
        char *r = strstr(tail, "< /tmp/diskos-hostname");
        EXPECT(r != NULL);
        if(r){
            *r = 0;
            snprintf(script, sizeof script, "%.*s%s ]%s< %s%s", (int)pre, WSC_SCRIPT, path("hn"), tail, path("hn"),
                     r + strlen("< /tmp/diskos-hostname"));
        }
    }
    mkdir(path("bin"), 0755);
    char stub[256];
    const char *tools[] = { "rfkill", "ifconfig", "wpa_supplicant", "udhcpc" };
    for(int i = 0; i < 4; i++){
        snprintf(stub, sizeof stub, "#!/bin/sh\necho \"%s $*\" >> %s\n", tools[i], path("log"));
        char p[200]; snprintf(p, sizeof p, "%s/bin/%s", dir, tools[i]); put(p, stub, 0755);
    }
    put(path("up.sh"), script, 0755);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "PATH=%s/bin:$PATH sh %s --help", dir, path("up.sh"));
    EXPECT(system(cmd) == 0); EXPECT(access(path("log"), F_OK) != 0);               /* self-test: does nothing */
    put(path("hn"), "Kitchen", 0644);
    snprintf(cmd, sizeof cmd, "PATH=%s/bin:$PATH sh %s; sleep 0.3", dir, path("up.sh"));
    EXPECT(system(cmd) == 0);
    char log[1024] = ""; wsc_read(path("log"), log, sizeof log);
    EXPECT(strstr(log, "rfkill unblock wifi\nifconfig wlan0 up\nwpa_supplicant -B -i wlan0 -c /usr/data/wpa_supplicant.conf\n"));
    EXPECT(strstr(log, "udhcpc -i wlan0 -x hostname:Kitchen -t 10 -T 2 -A 3\n"));
    unlink(path("log")); unlink(path("hn"));
    EXPECT(system(cmd) == 0); wsc_read(path("log"), log, sizeof log);
    EXPECT(strstr(log, "udhcpc -i wlan0 -x hostname:Disc -t 10 -T 2 -A 3\n"));     /* no name file yet: "Disc" */
    if(fails) fprintf(stderr, "log:\n%s\nscript:\n%s\n", log, script);

    snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if(system(cmd)) {}
    if(fails) return 1;
    puts("PASS wifi_up.sh: supplicant first, then DHCP with the hostname and short retries; --help does nothing");
    return 0;
}
