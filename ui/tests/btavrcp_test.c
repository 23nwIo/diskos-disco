/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* ui/dbusmini.c + ui/player/btavrcp.c against a real dbus-daemon with the Disc's system bus policy (the default
 * system.conf rules plus BlueZ 5.54's bluetooth.conf and a BlueALSA rule): a fake BlueZ (written with dbusmini) takes
 * RegisterPlayer, presses the buttons, reports the transport volume and restarts; gdbus (GLib's own marshalling)
 * checks our replies and calls from outside. Needs dbus-daemon and gdbus on the host; skipped (exit 0) without them.
 *   make -C ui/tests btavrcp_test && ./btavrcp_test */
#include "../dbusmini.h"
#include "../player/btavrcp.h"
#include "../btbatt.h"
#include "../player/core.h"
#include "../player/engine.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static char g_dir[64], g_sock[128];
static pid_t g_daemon;
static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

/* ---- the module's callbacks ---- */
static int ops[16], nops, vols[16][2], nvols; static char volmac[18];
static void tlog(const char *fmt, ...){ va_list ap; va_start(ap, fmt); fprintf(stderr, "  [btav] "); vfprintf(stderr, fmt, ap); fputc('\n', stderr); va_end(ap); }
static void tr(int op){ if(nops < 16) ops[nops++] = op; }
static void vol(const char *mac, int v, int first){ snprintf(volmac, sizeof volmac, "%s", mac); if(nvols < 16){ vols[nvols][0] = v; vols[nvols][1] = first; nvols++; } }

/* ---- the fake BlueZ / BlueALSA ---- */
static dm_conn fz = { .fd = -1 }, fa = { .fd = -1 };   /* org.bluez, org.bluealsa; -1: not connected (0 would poll stdin) */
static char player_name[64], player_path[96];
static int registrations, last_caps, pc_signals, set_soft = -1, battery_gets;
static char last_status[32], last_title[64];
static uint32_t pending_call;                  /* our call to the player */
static int call_replied;

static int hfp_on, hfp_connects;                /* the second headphones (92:C8): their hands-free link */
static char hfp_uuid[48], hfp_path[64];
static void bluez_msg(void *ctx, const dm_msg *m){
    (void)ctx;
    if(m->type == DM_CALL && m->member && !strcmp(m->member, "ConnectProfile")){
        dm_rd b = m->body; dm_val u;
        if(dm_r_expect(&b, 's', &u)) snprintf(hfp_uuid, sizeof hfp_uuid, "%s", u.s);
        snprintf(hfp_path, sizeof hfp_path, "%s", m->path ? m->path : "");
        hfp_connects++; hfp_on = 1;
        dm_send(&fz, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, NULL, NULL);
        return;
    }
    if(m->type == DM_CALL && m->member && !strcmp(m->member, "RegisterPlayer")){
        dm_rd b = m->body; dm_val p, props, v;
        CHECK(m->path && !strcmp(m->path, "/org/bluez/hci0"));
        CHECK(m->iface && !strcmp(m->iface, "org.bluez.Media1"));
        CHECK(dm_r_expect(&b, 'o', &p) && dm_r_expect(&b, 'a', &props));
        snprintf(player_path, sizeof player_path, "%s", p.s);
        snprintf(player_name, sizeof player_name, "%s", m->sender);
        last_caps = 0;
        const char *caps[] = { "CanControl", "CanPlay", "CanPause", "CanGoNext", "CanGoPrevious" };
        for(int i = 0; i < 5; i++) if(dm_r_dict_find(props.sub, caps[i], &v) && v.type == 'b' && v.u == 1) last_caps++;
        if(dm_r_dict_find(props.sub, "PlaybackStatus", &v) && v.type == 's') snprintf(last_status, sizeof last_status, "%s", v.s);
        registrations++;
        dm_send(&fz, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, NULL, NULL);
        return;
    }
    if(m->type == DM_CALL && m->member && !strcmp(m->member, "GetManagedObjects")){
        /* a{oa{sa{sv}}}: one device object and one transport with a volume */
        dm_buf b; dm_buf_init(&b);
        size_t objs = dm_w_array_begin(&b, 8);
        dm_w_struct(&b); dm_w_str(&b, "/org/bluez/hci0/dev_70_DA_17_01_03_35");
        size_t ifs = dm_w_array_begin(&b, 8);
        dm_w_struct(&b); dm_w_str(&b, "org.bluez.Device1");
        size_t pr = dm_w_array_begin(&b, 8); dm_w_dict_sv_str(&b, "Name", "Hi-X25BT"); dm_w_dict_sv_bool(&b, "Connected", 1); dm_w_array_end(&b, pr);
        dm_w_array_end(&b, ifs);
        dm_w_struct(&b); dm_w_str(&b, "/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0");
        ifs = dm_w_array_begin(&b, 8);
        dm_w_struct(&b); dm_w_str(&b, "org.freedesktop.DBus.Introspectable");
        pr = dm_w_array_begin(&b, 8); dm_w_array_end(&b, pr);
        dm_w_struct(&b); dm_w_str(&b, "org.bluez.MediaTransport1");
        pr = dm_w_array_begin(&b, 8);
        dm_w_dict_sv_str(&b, "State", "active");
        dm_w_struct(&b); dm_w_str(&b, "Volume"); dm_w_sig(&b, "q"); dm_w_u16(&b, 33);
        dm_w_array_end(&b, pr);
        dm_w_array_end(&b, ifs);
        dm_w_array_end(&b, objs);
        CHECK(dm_send(&fz, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, "a{oa{sa{sv}}}", &b));
        dm_buf_free(&b);
        return;
    }
    if(m->type == DM_RETURN && m->reply_serial == pending_call){ call_replied++; return; }
    if(m->type == DM_SIGNAL && m->member && !strcmp(m->member, "PropertiesChanged") && m->path && !strcmp(m->path, BTAV_PLAYER_PATH)){
        dm_rd b = m->body; dm_val ifn, props, v;
        CHECK(dm_r_expect(&b, 's', &ifn) && !strcmp(ifn.s, "org.mpris.MediaPlayer2.Player") && dm_r_expect(&b, 'a', &props));
        if(dm_r_dict_find(props.sub, "PlaybackStatus", &v) && v.type == 's') snprintf(last_status, sizeof last_status, "%s", v.s);
        dm_val md;
        if(dm_r_dict_find(props.sub, "Metadata", &md) && md.type == 'a' && dm_r_dict_find(md.sub, "xesam:title", &v) && v.type == 's')
            snprintf(last_title, sizeof last_title, "%s", v.s);
        pc_signals++;
    }
}
static void bluealsa_msg(void *ctx, const dm_msg *m){
    (void)ctx;
    if(m->type == DM_CALL && m->member && !strcmp(m->member, "Get") && m->path && strstr(m->path, "92_C8")){
        if(!hfp_on){                                /* no hands-free link: no RFCOMM object */
            dm_send(&fa, DM_ERROR, DM_NO_REPLY, m->sender, NULL, NULL, NULL, "org.freedesktop.DBus.Error.UnknownObject",
                    m->serial, NULL, NULL);
            return;
        }
        dm_buf r; dm_buf_init(&r); dm_w_sig(&r, "y"); dm_w_u8(&r, 80);
        dm_send(&fa, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, "v", &r);
        dm_buf_free(&r);
        return;
    }
    if(m->type == DM_CALL && m->member && !strcmp(m->member, "Get")){   /* RFCOMM1.Battery of the Hi-X25BT: 90 */
        dm_rd b = m->body; dm_val i, p;
        CHECK(m->path && !strcmp(m->path, "/org/bluealsa/hci0/dev_70_DA_17_01_03_35/rfcomm"));
        CHECK(dm_r_expect(&b, 's', &i) && !strcmp(i.s, "org.bluealsa.RFCOMM1") && dm_r_expect(&b, 's', &p) && !strcmp(p.s, "Battery"));
        dm_buf r; dm_buf_init(&r); dm_w_sig(&r, "y"); dm_w_u8(&r, 90);
        dm_send(&fa, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, "v", &r);
        dm_buf_free(&r);
        battery_gets++;
        return;
    }
    if(m->type != DM_CALL || !m->member || strcmp(m->member, "Set")) return;
    dm_rd b = m->body; dm_val i, p, v;
    CHECK(m->path && !strcmp(m->path, "/org/bluealsa/hci0/dev_70_DA_17_01_03_35/a2dpsrc/sink"));
    if(dm_r_expect(&b, 's', &i) && !strcmp(i.s, "org.bluealsa.PCM1") && dm_r_expect(&b, 's', &p) && !strcmp(p.s, "SoftVolume")
       && dm_r_expect(&b, 'v', &v)){ dm_rd vr = v.sub; dm_val bv; if(dm_r_expect(&vr, 'b', &bv)) set_soft = (int)bv.u; }
    dm_send(&fa, DM_RETURN, DM_NO_REPLY, m->sender, NULL, NULL, NULL, NULL, m->serial, NULL, NULL);
}

static void pump(double secs){
    double until = now_s() + secs;
    while(now_s() < until){
        btav_tick(now_s());
        struct pollfd pf[3] = { { btav_fd(), POLLIN, 0 }, { fz.fd, POLLIN, 0 }, { fa.fd, POLLIN, 0 } };
        poll(pf, 3, 20);
        if(pf[0].fd >= 0 && pf[0].revents) btav_io();
        if(fz.fd >= 0 && pf[1].revents && dm_read_msgs(&fz, bluez_msg, NULL) < 0){ fprintf(stderr, "fake bluez dropped by the bus\n"); fails++; dm_close(&fz); }
        if(fa.fd >= 0 && pf[2].revents && dm_read_msgs(&fa, bluealsa_msg, NULL) < 0){ fprintf(stderr, "fake bluealsa dropped\n"); fails++; dm_close(&fa); }
    }
}
/* run argv (gdbus) while the bus keeps being served; its stdout in out */
static int run_pumping(char *const argv[], char *out, size_t cap){
    int pfd[2]; if(pipe(pfd)) return -1;
    pid_t p = fork();
    if(p == 0){ dup2(pfd[1], 1); close(pfd[0]); execvp(argv[0], argv); _exit(127); }
    close(pfd[1]);
    size_t n = 0; int st = -1;
    for(;;){
        pump(0.05);
        struct pollfd pf = { pfd[0], POLLIN, 0 };
        if(poll(&pf, 1, 0) > 0){ ssize_t r = read(pfd[0], out + n, cap - 1 - n); if(r > 0) n += (size_t)r; }
        if(waitpid(p, &st, WNOHANG) == p) break;
    }
    ssize_t r; while(n + 1 < cap && (r = read(pfd[0], out + n, cap - 1 - n)) > 0) n += (size_t)r;
    out[n] = 0; close(pfd[0]);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}
static int request_name(dm_conn *c, const char *name){
    dm_buf b; dm_buf_init(&b); dm_w_str(&b, name); dm_w_u32(&b, 0);
    uint32_t s = dm_call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName", "su", &b);
    dm_buf_free(&b);
    return s ? 0 : -1;
}
static void release_name(dm_conn *c, const char *name){
    dm_buf b; dm_buf_init(&b); dm_w_str(&b, name);
    dm_call(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName", "s", &b);
    dm_buf_free(&b);
}
static void signal_volume(dm_conn *c, const char *path, int v){
    dm_buf b; dm_buf_init(&b);
    dm_w_str(&b, "org.bluez.MediaTransport1");
    size_t a = dm_w_array_begin(&b, 8);
    dm_w_struct(&b); dm_w_str(&b, "Volume"); dm_w_sig(&b, "q"); dm_w_u16(&b, (uint16_t)v);
    dm_w_array_end(&b, a);
    a = dm_w_array_begin(&b, 4); dm_w_array_end(&b, a);
    CHECK(dm_send(c, DM_SIGNAL, DM_NO_REPLY, NULL, path, "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, 0, "sa{sv}as", &b));
    dm_buf_free(&b);
}

static const char *CONF =
    "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
    " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
    "<busconfig><type>system</type><listen>unix:path=%s</listen><auth>EXTERNAL</auth>\n"
    /* the default system.conf */
    "<policy context=\"default\"><allow user=\"*\"/><deny own=\"*\"/><deny send_type=\"method_call\"/>"
    "<allow send_type=\"signal\"/><allow send_requested_reply=\"true\" send_type=\"method_return\"/>"
    "<allow send_requested_reply=\"true\" send_type=\"error\"/><allow receive_type=\"method_call\"/>"
    "<allow receive_type=\"method_return\"/><allow receive_type=\"error\"/><allow receive_type=\"signal\"/>"
    "<allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus\"/>"
    "<allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Introspectable\"/>"
    "<allow send_destination=\"org.freedesktop.DBus\" send_interface=\"org.freedesktop.DBus.Properties\"/></policy>\n"
    /* BlueZ 5.54 src/bluetooth.conf */
    "<policy user=\"root\"><allow own=\"org.bluez\"/><allow send_destination=\"org.bluez\"/>"
    "<allow send_interface=\"org.bluez.Agent1\"/><allow send_interface=\"org.bluez.MediaEndpoint1\"/>"
    "<allow send_interface=\"org.bluez.MediaPlayer1\"/><allow send_interface=\"org.bluez.Profile1\"/>"
    "<allow send_interface=\"org.freedesktop.DBus.ObjectManager\"/><allow send_interface=\"org.freedesktop.DBus.Properties\"/>"
    "<allow send_interface=\"org.mpris.MediaPlayer2.Player\"/></policy>\n"
    "<policy context=\"default\"><allow send_destination=\"org.bluez\"/></policy>\n"
    /* BlueALSA */
    "<policy user=\"root\"><allow own_prefix=\"org.bluealsa\"/><allow send_destination=\"org.bluealsa\"/></policy>\n"
    "</busconfig>\n";

static void cleanup(void){
    if(g_daemon > 0){ kill(g_daemon, SIGTERM); waitpid(g_daemon, NULL, 0); g_daemon = 0; }
    char cmd[128]; snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir); if(g_dir[0] && system(cmd)){}
}

int main(void){
    /* pure helpers first */
    char mac[18];
    CHECK(btav_mac_of_path("/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0", mac, sizeof mac) == 0 && !strcmp(mac, "70:DA:17:01:03:35"));
    CHECK(btav_mac_of_path("/org/bluealsa/hci0/dev_70_da_17_01_03_35", mac, sizeof mac) == 0 && !strcmp(mac, "70:DA:17:01:03:35"));
    CHECK(btav_mac_of_path("/org/bluez/hci0", mac, sizeof mac) == -1);
    CHECK(btav_mac_of_path("/org/bluez/hci0/dev_70_DA_17_01_03_3", mac, sizeof mac) == -1);
    for(int v = 0; v <= PL_VOL_MAX; v++) CHECK(btav_from127(btav_to127(v, PL_VOL_MAX), PL_VOL_MAX) == v);   /* no drift */
    CHECK(btav_to127(PL_VOL_MAX, PL_VOL_MAX) == 127 && btav_to127(0, PL_VOL_MAX) == 0);
    CHECK(btav_from127(127, PL_VOL_MAX) == PL_VOL_MAX && btav_from127(200, PL_VOL_MAX) == PL_VOL_MAX);

    if(system("command -v dbus-daemon >/dev/null && command -v gdbus >/dev/null") != 0){
        fprintf(stderr, "btavrcp_test: no dbus-daemon/gdbus, bus part skipped\n");
        return fails ? 1 : 0;
    }
    snprintf(g_dir, sizeof g_dir, "/tmp/btavrcp_test.%d", (int)getpid());
    mkdir(g_dir, 0700);
    snprintf(g_sock, sizeof g_sock, "%s/bus", g_dir);
    char conf[160]; snprintf(conf, sizeof conf, "%s/system.conf", g_dir);
    FILE *f = fopen(conf, "w"); fprintf(f, CONF, g_sock); fclose(f);
    char cfgarg[200]; snprintf(cfgarg, sizeof cfgarg, "--config-file=%s", conf);
    g_daemon = fork();
    if(g_daemon == 0){ execlp("dbus-daemon", "dbus-daemon", cfgarg, "--nofork", "--nopidfile", (char *)NULL); _exit(127); }
    atexit(cleanup);
    for(int i = 0; i < 100 && access(g_sock, F_OK); i++) usleep(20000);

    /* the module first: bluetoothd is not there yet */
    btav_cb cb = { tlog, tr, vol };
    btav_init(&cb, g_sock);
    btav_status(ENG_PAUSE, "First", "An Artist", 200000);
    pump(0.3);
    CHECK(btav_fd() >= 0 && !btav_registered());

    /* bluetoothd comes: the player registers (after its 1 s grace) */
    CHECK(dm_connect(&fz, g_sock, 2000) == 0);
    CHECK(request_name(&fz, "org.bluez") == 0);
    pump(1.6);
    CHECK(registrations == 1 && btav_registered());
    CHECK(last_caps == 5 && !strcmp(last_status, "Paused"));
    /* GetManagedObjects after the registration: the transport's volume, a new link */
    CHECK(nvols == 1 && vols[0][0] == 33 && vols[0][1] == 1 && !strcmp(volmac, "70:DA:17:01:03:35"));
    CHECK(btav_volume_of("70:DA:17:01:03:35") == 33 && btav_volume_of("00:11:22:33:44:55") == -1);

    /* the buttons, as BlueZ calls them */
    const char *btn[] = { "Play", "Pause", "Next", "Previous", "Stop", "PlayPause" };
    int want[] = { PL_T_TOGGLE, PL_T_PAUSE, PL_T_NEXT, PL_T_PREV, PL_T_PAUSE, PL_T_TOGGLE };
    for(int i = 0; i < 6; i++){
        pending_call = dm_send(&fz, DM_CALL, 0, player_name, player_path, "org.mpris.MediaPlayer2.Player", btn[i], NULL, 0, NULL, NULL);
        pump(0.15);
    }
    CHECK(nops == 6 && call_replied == 6);
    for(int i = 0; i < 6 && i < nops; i++) CHECK(ops[i] == want[i]);

    /* the headphones' volume: changes on this link; a link that goes and comes is new again */
    signal_volume(&fz, "/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0", 40);
    signal_volume(&fz, "/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0", 40);   /* no change: no call */
    pump(0.2);
    CHECK(nvols == 2 && vols[1][0] == 40 && vols[1][1] == 0);
    {   /* InterfacesRemoved(o, as) */
        dm_buf b; dm_buf_init(&b); dm_w_str(&b, "/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0");
        size_t a = dm_w_array_begin(&b, 4); dm_w_str(&b, "org.bluez.MediaTransport1"); dm_w_array_end(&b, a);
        CHECK(dm_send(&fz, DM_SIGNAL, DM_NO_REPLY, NULL, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", NULL, 0, "oas", &b));
        dm_buf_free(&b);
    }
    signal_volume(&fz, "/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0", 50);
    pump(0.2);
    CHECK(nvols == 3 && vols[2][0] == 50 && vols[2][1] == 1);

    /* a volume from someone who is not BlueZ is not taken (the match rule names org.bluez) */
    dm_conn other; CHECK(dm_connect(&other, g_sock, 2000) == 0);
    signal_volume(&other, "/org/bluez/hci0/dev_70_DA_17_01_03_35/sep1/fd0", 99);
    pump(0.2);
    CHECK(nvols == 3);
    dm_close(&other);

    /* status and track go to BlueZ as PropertiesChanged */
    char rule[200]; snprintf(rule, sizeof rule, "type='signal',sender='%s',path='%s'", player_name, BTAV_PLAYER_PATH);
    dm_add_match(&fz, rule);
    pump(0.1);
    btav_status(ENG_PLAY, "Second Song", "Somebody", 180000);
    btav_status(ENG_PLAY, "Second Song", "Somebody", 180000);   /* unchanged: nothing sent */
    pump(0.2);
    CHECK(pc_signals == 1 && !strcmp(last_status, "Playing") && !strcmp(last_title, "Second Song"));

    /* gdbus (GLib's marshalling) reads our properties and presses a button */
    char out[4096];
    char *getall[] = { "gdbus", "call", "--address", NULL, "--dest", player_name, "--object-path", BTAV_PLAYER_PATH,
                       "--method", "org.freedesktop.DBus.Properties.GetAll", "org.mpris.MediaPlayer2.Player", NULL };
    char addr[200]; snprintf(addr, sizeof addr, "unix:path=%s", g_sock); getall[3] = addr;
    int x = run_pumping(getall, out, sizeof out);
    CHECK(x == 0 && strstr(out, "'PlaybackStatus': <'Playing'>") && strstr(out, "'CanGoNext': <true>")
          && strstr(out, "'xesam:title': <'Second Song'>") && strstr(out, "'xesam:artist': <['Somebody']>")
          && strstr(out, "'mpris:length': <int64 180000000>"));
    if(x != 0 || !strstr(out, "Playing")) fprintf(stderr, "gdbus GetAll (%d): %s\n", x, out);
    char *get[] = { "gdbus", "call", "--address", addr, "--dest", player_name, "--object-path", BTAV_PLAYER_PATH,
                    "--method", "org.freedesktop.DBus.Properties.Get", "org.mpris.MediaPlayer2.Player", "PlaybackStatus", NULL };
    x = run_pumping(get, out, sizeof out);
    CHECK(x == 0 && strstr(out, "<'Playing'>"));
    char *next[] = { "gdbus", "call", "--address", addr, "--dest", player_name, "--object-path", BTAV_PLAYER_PATH,
                     "--method", "org.mpris.MediaPlayer2.Player.Next", NULL };
    int before = nops;
    x = run_pumping(next, out, sizeof out);
    CHECK(x == 0 && nops == before + 1 && ops[before] == PL_T_NEXT);
    char *bad[] = { "gdbus", "call", "--address", addr, "--dest", player_name, "--object-path", "/elsewhere",
                    "--method", "org.mpris.MediaPlayer2.Player.Next", NULL };
    x = run_pumping(bad, out, sizeof out);
    CHECK(x != 0 && nops == before + 1);          /* another object: an error, no button */

    /* SoftVolume to BlueALSA */
    CHECK(dm_connect(&fa, g_sock, 2000) == 0);
    CHECK(request_name(&fa, "org.bluealsa") == 0);
    pump(0.1);
    CHECK(btav_soft_volume("70:da:17:01:03:35", 0) == 0);
    pump(0.2);
    CHECK(set_soft == 0);

    /* the UI's battery (btbatt.c): asks BlueALSA once, then follows its signals */
    char envaddr[200]; snprintf(envaddr, sizeof envaddr, "unix:path=%s", g_sock);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", envaddr, 1);
    CHECK(btbatt_mac_of_path("/org/bluealsa/hci0/dev_70_DA_17_01_03_35/rfcomm", mac, sizeof mac) == 0 && !strcmp(mac, "70:DA:17:01:03:35"));
    CHECK(btbatt_level("70:DA:17:01:03:35") == -1);
    btbatt_poll("", tlog);                          /* nothing connected: no bus, no question */
    pump(0.1);
    CHECK(battery_gets == 0);
    for(int i = 0; i < 5; i++){ btbatt_poll("70:da:17:01:03:35", tlog); pump(0.1); }
    CHECK(battery_gets == 1 && btbatt_level("70:DA:17:01:03:35") == 90);
    {
        dm_buf b; dm_buf_init(&b);
        dm_w_str(&b, "org.bluealsa.RFCOMM1");
        size_t a = dm_w_array_begin(&b, 8);
        dm_w_struct(&b); dm_w_str(&b, "Battery"); dm_w_sig(&b, "y"); dm_w_u8(&b, 70);
        dm_w_array_end(&b, a);
        a = dm_w_array_begin(&b, 4); dm_w_array_end(&b, a);
        CHECK(dm_send(&fa, DM_SIGNAL, DM_NO_REPLY, NULL, "/org/bluealsa/hci0/dev_70_DA_17_01_03_35/rfcomm",
                      "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, 0, "sa{sv}as", &b));
        dm_buf_free(&b);
    }
    pump(0.1); btbatt_poll("70:DA:17:01:03:35", tlog);
    CHECK(btbatt_level("70:DA:17:01:03:35") == 70 && battery_gets == 1);
    /* headphones with only A2DP (no RFCOMM object): after the wait the hands-free link is asked for, then the level */
    for(int i = 0; i < 4; i++){ btbatt_poll("70:DA:17:01:92:C8", tlog); pump(0.05); }
    CHECK(btbatt_level("70:DA:17:01:92:C8") == -1 && hfp_connects == 0);    /* not before BB_HFP_WAIT_MS */
    for(int i = 0; i < 30 && btbatt_level("70:DA:17:01:92:C8") < 0; i++){ btbatt_poll("70:DA:17:01:92:C8", tlog); pump(0.05); }
    CHECK(hfp_connects == 1 && !strcmp(hfp_uuid, "0000111e-0000-1000-8000-00805f9b34fb")
          && !strcmp(hfp_path, "/org/bluez/hci0/dev_70_DA_17_01_92_C8"));
    CHECK(btbatt_level("70:DA:17:01:92:C8") == 80);
    for(int i = 0; i < 20; i++){ btbatt_poll("70:DA:17:01:92:C8", tlog); pump(0.05); }
    CHECK(hfp_connects == 1);                       /* known: not asked again */
    /* connected again: the old level is not shown; without the link, at most BB_HFP_TRIES requests */
    hfp_on = 0; hfp_connects = 0;
    btbatt_poll("", tlog); btbatt_poll("70:DA:17:01:92:C8", tlog);
    CHECK(btbatt_level("70:DA:17:01:92:C8") == -1);
    hfp_on = 0;
    for(int i = 0; i < 60; i++){ btbatt_poll("70:DA:17:01:92:C8", tlog); pump(0.05); if(hfp_on && hfp_connects < 2) hfp_on = 0; }
    CHECK(hfp_connects == 2);

    /* bluetoothd restarts: the player registers again; its links are forgotten */
    release_name(&fz, "org.bluez");
    pump(0.2);
    CHECK(!btav_registered() && btav_volume_of("70:DA:17:01:03:35") == -1);
    request_name(&fz, "org.bluez");
    pump(1.6);
    CHECK(registrations == 2 && btav_registered() && !strcmp(last_status, "Playing"));

    /* the bus goes away and comes back: reconnects within the retry time */
    btav_close();
    CHECK(btav_fd() < 0 && !btav_registered());

    dm_close(&fz); dm_close(&fa);
    if(fails) fprintf(stderr, "btavrcp_test: %d failures\n", fails);
    else printf("btavrcp_test: ok\n");
    return fails ? 1 : 0;
}
