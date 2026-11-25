/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test of the own player skeleton (ui/player): frame parsing and formatting, the command handling in core.c, and
 * an end-to-end run of the real binary over POSIX message queues (watchdog off, own queue names).
 * Run: make -C ui/tests player_test && ./ui/tests/player_test ./ui/tests/diskos-player-host */
#include "../player/btout.h"
#include "../player/core.h"
#include "../player/proto.h"
#include "../player/sdcard.h"
#include "../player/dac.h"
#include "../player/hw.h"
#include "../player/mpdcli.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static char sent[16][512]; static int nsent;
static void cap_send(void *ctx, const char *f, int len){
    (void)ctx; if(nsent < 16){ snprintf(sent[nsent], sizeof sent[0], "%.*s", len, f); nsent++; }
}
static int send1(pl_core_t *c, const char *f){ return pl_core_handle(c, f, (int)strlen(f)); }

static void test_proto(void){
    pp_frame_t f;
    CHECK(pp_parse("0715000C0050", 12, &f) == 0 && f.tag == 0x0715 && pp_class1(&f) == 0x50);
    CHECK(pp_parse("02020008", 8, &f) == 0 && f.tag == 0x0202 && f.dlen == 0 && pp_class1(&f) == -1);
    CHECK(pp_parse("0715000c00FF", 12, &f) == 0 && pp_class1(&f) == 255);   /* either hex case */
    CHECK(pp_parse("0715000C005", 11, &f) == -1);                          /* LEN larger than received */
    CHECK(pp_parse("07150007", 8, &f) == -1);                              /* LEN below the header */
    CHECK(pp_parse("07x5000C0050", 12, &f) == -1);
    CHECK(pp_parse("0715", 4, &f) == -1);
    CHECK(pp_parse("0715000C00zz", 12, &f) == 0 && pp_class1(&f) == -1);

    char o[64];
    CHECK(pp_fmt_a1(o, sizeof o, 0x1234) == 16 && !strcmp(o, "a103001000001234"));
    CHECK(pp_fmt_a714(o, sizeof o, 120) == 12 && !strcmp(o, "a714000C0078") && pp_fmt_a1(o, sizeof o, 0xabc) == 16 && !strcmp(o, "a103001000000ABC"));
    CHECK(pp_fmt_a607(o, sizeof o, 8) == 12 && !strcmp(o, "a607000C0008"));
    CHECK(pp_fmt_a2(o, sizeof o, "{}") == 10 && !strcmp(o, "a202000A{}"));
    CHECK(pp_fmt_a2(o, 8, "{}") == -1);
    CHECK(pp_fmt_a1(o, 4, 1) == -1);

    char e[32] = "x";
    CHECK(pp_json_escape(e, sizeof e, 1, "a\"b\\c\n") == 14 && !strcmp(e, "xa\\\"b\\\\c\\u000a"));
    char s[6] = "";
    CHECK(pp_json_escape(s, sizeof s, 0, "abcdefgh") == -1 && s[0] == 0);
}
static int npause;
static int count_pause(void *ctx, int op, long arg){ (void)ctx; (void)arg; if(op == PL_T_PAUSE) npause++; return 0; }

static void test_core(void){
    pl_core_t c; pl_core_init(&c, cap_send, NULL, NULL);
    nsent = 0;
    CHECK(c.volume == 20 && c.max_volume == 120);

    CHECK(send1(&c, "02020008") == 0 && nsent == 1);
    CHECK(!strcmp(sent[0], "a2020013{\"state\":1}"));
    { pp_frame_t f; CHECK(pp_parse(sent[0], strlen(sent[0]), &f) == 0 && f.dlen == (int)strlen(sent[0]) - 8); }

    nsent = 0;
    CHECK(send1(&c, "0715000C0050") == 0 && c.volume == 80 && nsent == 1 && !strcmp(sent[0], "a714000C0050"));
    CHECK(send1(&c, "0715000C0079") == -1 && c.volume == 80);                  /* 121: out of range */
    nsent = 0;
    CHECK(send1(&c, "0711000C0040") == 0 && c.max_volume == 64 && c.volume == 64 && nsent == 1);
    CHECK(send1(&c, "0715000C0050") == 0 && c.volume == 64);                   /* capped by the maximum */

    CHECK(send1(&c, "0713000C0105") == 0 && c.balance == 0x105);
    CHECK(send1(&c, "0713000C000b") == -1 && c.balance == 0x105);
    CHECK(send1(&c, "0713000C0205") == -1);
    CHECK(send1(&c, "0649000C0001") == 0 && c.gain == 1);
    CHECK(send1(&c, "0645000C0000") == 0 && c.gain == 0);
    CHECK(send1(&c, "0653000C0004") == 0 && c.dac_filter == 4);
    CHECK(send1(&c, "0653000C0006") == -1 && c.dac_filter == 4);
    CHECK(send1(&c, "0812000C0001") == 0 && c.dre == 1);
    CHECK(send1(&c, "0689000C000c") == 0 && c.eq_preset == 12);
    CHECK(send1(&c, "0102000C0001") == 0 && c.play_mode == 1);
    CHECK(send1(&c, "0664000C012c") == 0 && c.idle_off_s == 300);
    CHECK(send1(&c, "0821000C0003") == 0 && c.key_action[1] == 3);
    unsigned sq = c.sleep_seq;
    CHECK(send1(&c, "0806000C0001") == 0 && c.sleep_s == 1 && c.sleep_seq == sq + 1);
    CHECK(send1(&c, "0806000C0000") == 0 && c.sleep_s == 0 && c.sleep_seq == sq + 2);
    CHECK(c.jack == -1);                                          /* jack: first level recorded, then changes */
    c.transport = count_pause; npause = 0;
    pl_core_jack(&c, 1); CHECK(c.jack == 1);
    pl_core_jack(&c, 0); CHECK(c.jack == 0 && npause == 1);       /* unplugged: pause */
    pl_core_jack(&c, 1); CHECK(c.jack == 1 && npause == 1);       /* plugged in again: stays paused */
    c.transport = NULL;
    unsigned uq = c.usb_seq;                                      /* 0642: storage on/off; the USB audio gadgets not yet */
    CHECK(send1(&c, "0642000C0001") == 0 && c.usb_gadget == 1 && c.usb_seq == uq + 1);
    CHECK(send1(&c, "0642000C0005") == 0 && c.usb_gadget == 1 && c.usb_seq == uq + 1);
    CHECK(send1(&c, "0642000C0000") == 0 && c.usb_gadget == 0 && c.usb_seq == uq + 2);

    /* work mode: 8 is announced once (as the stock player announces a change), other modes are not claimed */
    nsent = 0;
    CHECK(send1(&c, "0657000C0008") == 0 && c.work_mode == 8 && nsent == 1 && !strcmp(sent[0], "a607000C0008"));
    CHECK(send1(&c, "0657000C0008") == 0 && nsent == 1);
    CHECK(send1(&c, "0657000C0006") == 0 && c.work_mode == 8 && nsent == 1);
    unsigned rs0 = c.route_seq;
    CHECK(send1(&c, "0666000C0004") == 0 && c.route == -1 && c.out_dig == 1 && c.out_aux == 0 && c.route_seq == rs0 + 1);   /* stock form: AUX digital instead of analog */
    CHECK(send1(&c, "0666000C0006") == 0 && c.route == 6 && c.out_dig == 1);                            /* 0666 6 leaves AUX digital alone */
    c.route = -1; c.out_aux = 1; c.out_dig = 0; c.mem_aux = 1; c.mem_dig = 0;
    unsigned rs = c.route_seq;
    CHECK(send1(&c, "0666000C0002") == 0 && c.route == 2 && c.route_seq == rs + 1);
    /* 06b3: codec + the headphones' MAC (ui/bt.c bt_codec_frame_value) */
    CHECK(send1(&c, "06b3001D0003AA:bb:CC:01:02:03") == 0 && c.bt_codec == 3 && !strcmp(c.bt_mac, "AA:bb:CC:01:02:03"));
    CHECK(c.route_seq > rs + 1 && c.bt_ready && c.out_bt == 1 && c.out_aux == 0);   /* headphones: Bluetooth on, AUX off */
    CHECK(send1(&c, "06b3001D0003AA:bb:CC:01:02") == -1 && send1(&c, "06b3001D0009AA:bb:CC:01:02:03") == -1);
    CHECK(send1(&c, "06b3001D0000AA-bb-CC-01-02-03") == -1 && c.bt_codec == 3);
    char bp[96];
    CHECK(bto_pcm_path("aa:bb:CC:01:02:03", bp, sizeof bp) == 0 && !strcmp(bp, "/org/bluealsa/hci0/dev_AA_BB_CC_01_02_03/a2dpsrc/sink"));
    CHECK(bto_pcm_path("aa:bb:CC:01:02", bp, sizeof bp) == -1 && bto_pcm_path("aa-bb-CC-01-02-03", bp, sizeof bp) == -1);
    CHECK(bto_parse_rate("Device: /org/bluez/hci0/dev_X\nFormat: S16_LE\nChannels: 2\nSampling: 48000 Hz\n") == 48000);
    CHECK(bto_parse_rate("Transport: A2DP-source\n  Rate: 44100\n") == 44100 && bto_parse_rate("Rate: 7\nVolume: 127\n") == 0);
    CHECK(!strcmp(bto_codec_name(6), "aptX-HD") && !strcmp(bto_codec_name(3), "LDAC") && !strcmp(bto_codec_name(0), "SBC"));
    {   /* bluealsa-cli info as on the Disc (docs/MPD_CORE.md) plus the codec lines of BlueALSA 4.0 */
        const char *info = "Device: /org/bluez/hci0/dev_53_4A\nSequence: 1\nTransport: A2DP-source\nMode: sink\n"
                           "Format: S32_LE\nChannels: 2\nSampling: 96000 Hz\nAvailable codecs: SBC AAC LDAC\n"
                           "Selected codec: LDAC\nDelay: 150.0 ms\nSoftVolume: Y\n";
        char sel[16], av[64];
        bto_parse_selected(info, sel, sizeof sel); bto_parse_avail(info, av, sizeof av);
        CHECK(!strcmp(sel, "LDAC") && !strcmp(av, "SBC AAC LDAC"));
        bto_parse_selected("Codec: aac\n", sel, sizeof sel); bto_parse_avail("Volume: 4\n", av, sizeof av);
        CHECK(!strcmp(sel, "AAC") && av[0] == 0);
        bto_parse_avail("Available codecs:\n  SBC AAC\n  aptX aptX-HD\nSelected codec: SBC\n", av, sizeof av);
        CHECK(!strcmp(av, "SBC AAC APTX APTX-HD"));
        /* newer BlueALSA: each codec with its configuration, and the selected one too */
        info = "Rate: 44100\nAvailable codecs: SBC:ffff0235 AAC:c0ff8c8000 aptX:4f0000000100ff LDAC:2d0100000aa00a\n"
               "Selected codec: SBC:21150235\nDelay: 150.0 ms\n";
        bto_parse_selected(info, sel, sizeof sel); bto_parse_avail(info, av, sizeof av);
        CHECK(!strcmp(sel, "SBC") && !strcmp(av, "SBC AAC APTX LDAC"));
        bto_parse_avail("Available codecs:\n  SBC:ffff0235\n  aptX-HD:d70000\nSelected codec: SBC:2115\n", av, sizeof av);
        CHECK(!strcmp(av, "SBC APTX-HD"));
    }
    /* outputs: plugs, routes, 0d01..0d04 and the two volumes */
    c.transport = count_pause; npause = 0;
    c.max_volume = 120; c.jack = 0; c.bal = -1;
    c.vol_aux = 30; c.vol_bt = 50; c.volume = 50;                    /* Bluetooth is the lead now */
    nsent = 0; CHECK(send1(&c, "0715000C0034") == 0);                /* 52: only Bluetooth is on, AUX moves along */
    CHECK(c.volume == 52 && c.vol_bt == 52 && c.vol_aux == 32 && nsent == 1 && !strcmp(sent[0], "a714000C0034"));
    pl_core_bal(&c, 0); CHECK(c.bal == 0 && c.out_bt == 1);          /* the first level is only recorded */
    pl_core_jack(&c, 1);                                             /* a plug in: AUX on, Bluetooth off, AUX volume shown */
    CHECK(c.out_aux == 1 && c.out_bt == 0 && c.volume == 32 && npause == 0);
    pl_core_jack(&c, 0); CHECK(npause == 1);                         /* the last plug out, AUX only: pause */
    CHECK(send1(&c, "0d01000C0003") == 0 && c.out_aux == 1 && c.out_bt == 1 && c.volume == 52);   /* both by hand */
    pl_core_volume_step(&c, -1); CHECK(c.vol_bt == 51 && c.vol_aux == 31 && c.volume == 51);
    CHECK(send1(&c, "0d02000C0014") == 0 && c.vol_aux == 20 && c.volume == 51);
    CHECK(send1(&c, "0d03000C0010") == 0 && c.vol_bt == 16 && c.volume == 16);
    pl_core_set_volume(&c, 0); CHECK(c.vol_bt == 0 && c.vol_aux == 4);  /* each within 0..max: the gap stays until a limit */
    pl_core_set_volume(&c, 10); CHECK(c.vol_bt == 10 && c.vol_aux == 14);
    pl_core_bal(&c, 1); CHECK(c.out_aux == 1 && c.out_bt == 0 && pl_core_plugged(&c));   /* the 4.4 mm plug counts too */
    CHECK(send1(&c, "0d01000C0002") == 0 && c.out_aux == 0 && c.out_bt == 1);
    npause = 0; pl_core_bal(&c, 0); CHECK(npause == 0 && c.out_bt == 1 && c.out_aux == 0);   /* Bluetooth plays on */
    CHECK(send1(&c, "0d01000C0000") == 0 && !c.out_bt && !c.out_aux && !c.out_dig);   /* none at all: playback waits */
    CHECK(send1(&c, "0d01000C0008") == -1);                           /* bit 8: no such output */
    CHECK(send1(&c, "0d01000C0002") == 0 && c.out_bt == 1);
    CHECK(send1(&c, "0d04000C0001") == 0 && c.codec_want == 1);
    CHECK(send1(&c, "0d04000C0003") == -1 && send1(&c, "0d04000C0007") == -1);   /* only SBC/AAC/LDAC 2/aptX/aptX HD */
    CHECK(send1(&c, "0711000C0005") == 0 && c.vol_aux == 5 && c.vol_bt == 5);    /* max volume: both */
    c.max_volume = 120;
    c.transport = NULL;
    /* the headphones gone while they were the only output and no plug is in: Bluetooth waits for them, the DAC plays */
    CHECK(send1(&c, "0666000C0006") == 0 && c.route == 6 && !c.bt_ready && c.out_aux == 0 && c.out_bt == 1);
    CHECK(c.volume == c.vol_aux);                                    /* the DAC leads now */
    CHECK(send1(&c, "0d01000C0002") == 0 && c.out_bt == 1);          /* Bluetooth stays chosen, waiting */
    CHECK(send1(&c, "0d01000C0001") == 0 && c.out_bt == 0);
    CHECK(send1(&c, "0d01000C0002") == -1 && c.out_bt == 0 && c.out_aux == 1);   /* turning it on without headphones: refused */
    CHECK(send1(&c, "0d04000C0001") == -1);
    c.jack = -1; c.bal = -1;

    /* Headphone Volume (0d06): the headphones' own volume moves Bluetooth only (the default), or AUX with it */
    {
        pl_core_t h; pl_core_init(&h, cap_send, NULL, NULL);
        CHECK(h.hp_vol_all == 0);
        CHECK(send1(&h, "0d06000C0002") == -1 && h.hp_vol_all == 0);
        CHECK(send1(&h, "0666000C0002") == 0 && send1(&h, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(send1(&h, "0d01000C0003") == 0 && h.out_aux == 1 && h.out_bt == 1);
        pl_core_load_volumes(&h, 40, 50, 45);
        pl_core_set_bt_volume(&h, 60);
        CHECK(h.vol_bt == 60 && h.vol_aux == 40 && h.volume == 60);
        CHECK(send1(&h, "0d06000C0001") == 0 && h.hp_vol_all == 1);
        pl_core_set_bt_volume(&h, 55);
        CHECK(h.vol_bt == 55 && h.vol_aux == 35 && h.volume == 55);
        pl_core_set_bt_volume(&h, 0);
        CHECK(h.vol_bt == 0 && h.vol_aux == 0);                       /* AUX stops at 0 */
        CHECK(send1(&h, "0d01000C0002") == 0 && h.out_aux == 0);
        pl_core_set_bt_volume(&h, 20);
        CHECK(h.vol_bt == 20 && h.vol_aux == 20);                     /* AUX off: moves too */
    }

    /* AUX digital (0d01 bit 4, 0d07): its own volume, the volume keys move it too; a plug brings back the last AUX
     * choice (Jo 2026-11-25); none at all is fine; a notice when a play was held back */
    {
        pl_core_t d; pl_core_init(&d, cap_send, NULL, NULL);
        d.transport = count_pause; npause = 0; d.jack = 0;
        CHECK(d.out_aux == 1 && d.out_dig == 0 && d.mem_aux == 1 && d.mem_dig == 0);
        CHECK(send1(&d, "0d01000C0004") == 0 && !d.out_aux && d.out_dig && !d.mem_aux && d.mem_dig);   /* digital only */
        nsent = 0; CHECK(send1(&d, "0d07000C0030") == 0 && d.vol_dig == 48 && d.volume == 48 && nsent == 1);   /* it leads */
        CHECK(send1(&d, "0d07000C0079") == -1 && d.vol_dig == 48);
        pl_core_volume_step(&d, 1); CHECK(d.vol_dig == 49 && d.vol_aux == 21 && d.vol_bt == 21);
        pl_core_jack(&d, 1); CHECK(!d.out_aux && d.out_dig && !d.out_bt);   /* digital only comes back */
        pl_core_jack(&d, 0); CHECK(npause == 1 && d.out_dig);               /* the jack's output: pause, kept */
        CHECK(send1(&d, "0d01000C0000") == 0 && !d.out_dig && !d.mem_aux && d.mem_dig);   /* none: the memory stays */
        pl_core_jack(&d, 1); CHECK(!d.out_aux && d.out_dig);
        CHECK(send1(&d, "0d01000C0005") == 0 && d.out_aux && d.out_dig && d.volume == d.vol_aux);   /* both, analog leads */
        npause = 0; pl_core_jack(&d, 0); CHECK(npause == 1);
        CHECK(send1(&d, "0666000C0002") == 0 && send1(&d, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(d.out_bt && !d.out_aux && !d.out_dig && d.mem_aux && d.mem_dig);   /* newest: Bluetooth alone */
        pl_core_jack(&d, 1); CHECK(d.out_aux && d.out_dig && !d.out_bt);    /* both come back */
        unsigned ns = d.notice_seq;
        pl_core_notice(&d, PL_NOTICE_NONE); CHECK(d.notice_seq == ns + 1 && d.notice == PL_NOTICE_NONE);
    }

    /* Auto Outputs (0d05): newest (the default), all connected, off */
    {
        pl_core_t a; pl_core_init(&a, cap_send, NULL, NULL);
        a.transport = count_pause; npause = 0;
        CHECK(a.auto_out == PL_AUTO_NEWEST);
        CHECK(send1(&a, "0d05000C0003") == -1 && a.auto_out == PL_AUTO_NEWEST);
        pl_core_jack(&a, 0); pl_core_bal(&a, 0);                      /* levels known: nothing plugged */
        /* newest: headphones connect -> Bluetooth alone; a plug -> AUX alone; the plug out -> Bluetooth again */
        CHECK(send1(&a, "0666000C0002") == 0 && send1(&a, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(a.out_bt == 1 && a.out_aux == 0 && a.bt_ready);
        pl_core_jack(&a, 1); CHECK(a.out_aux == 1 && a.out_bt == 0);
        pl_core_jack(&a, 0); CHECK(a.out_aux == 0 && a.out_bt == 1 && npause == 1);   /* paused, on Bluetooth */
        npause = 0;
        /* both by hand, then the headphones go: Bluetooth off, AUX plays on, no pause */
        CHECK(send1(&a, "0d01000C0003") == 0);
        CHECK(send1(&a, "0666000C0006") == 0 && a.out_aux == 1 && a.out_bt == 0 && npause == 0);
        /* Bluetooth alone and a plug in when the headphones go: pause, AUX takes over */
        CHECK(send1(&a, "0666000C0002") == 0 && send1(&a, "06b3001D0000AA:BB:CC:01:02:03") == 0 && a.out_bt == 1);
        pl_core_bal(&a, 1); CHECK(a.out_aux == 1 && a.out_bt == 0);
        CHECK(send1(&a, "0d01000C0002") == 0 && a.out_aux == 0 && a.out_bt == 1);
        CHECK(send1(&a, "0666000C0006") == 0 && npause == 1 && a.out_aux == 1 && a.out_bt == 0);
        pl_core_bal(&a, 0); CHECK(npause == 2);                       /* the last plug out, AUX only: pause */

        /* all connected: switching to it turns on what is connected; a plug adds AUX, headphones add Bluetooth */
        npause = 0;
        CHECK(send1(&a, "0d05000C0002") == 0 && a.auto_out == PL_AUTO_ALL);
        CHECK(a.out_aux == 1 && a.out_bt == 0);                       /* nothing connected: the choice stays */
        CHECK(send1(&a, "0666000C0002") == 0 && send1(&a, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(a.out_bt == 1 && a.out_aux == 0);                       /* no plug: AUX off */
        pl_core_jack(&a, 1); CHECK(a.out_aux == 1 && a.out_bt == 1);  /* both */
        CHECK(send1(&a, "0666000C0006") == 0 && a.out_aux == 1 && a.out_bt == 0 && npause == 0);   /* AUX plays on */
        CHECK(send1(&a, "0666000C0002") == 0 && send1(&a, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(a.out_aux == 1 && a.out_bt == 1);
        pl_core_jack(&a, 0); CHECK(a.out_aux == 0 && a.out_bt == 1 && npause == 0);   /* Bluetooth plays on */

        /* off: nothing changes; a device that is not on gets a notice */
        CHECK(send1(&a, "0d05000C0000") == 0 && a.auto_out == PL_AUTO_OFF);
        unsigned ns = a.notice_seq;
        pl_core_jack(&a, 1); CHECK(a.out_aux == 0 && a.out_bt == 1 && a.notice_seq == ns + 1 && a.notice == PL_NOTICE_AUX);
        pl_core_jack(&a, 0); CHECK(npause == 0);                      /* AUX was off: Bluetooth plays on */
        CHECK(send1(&a, "0666000C0006") == 0 && a.out_bt == 1 && !a.bt_ready && npause == 1);   /* the only output: pause, kept */
        CHECK(send1(&a, "0666000C0002") == 0 && send1(&a, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(a.out_bt == 1 && a.bt_ready && a.notice_seq == ns + 1);  /* back on Bluetooth, no notice */
        pl_core_jack(&a, 1); CHECK(a.notice_seq == ns + 2);
        CHECK(send1(&a, "0d01000C0001") == 0 && a.out_aux == 1 && a.out_bt == 0);
        CHECK(send1(&a, "0666000C0006") == 0 && send1(&a, "0666000C0002") == 0 && send1(&a, "06b3001D0000AA:BB:CC:01:02:03") == 0);
        CHECK(a.out_aux == 1 && a.out_bt == 0 && a.notice_seq == ns + 3 && a.notice == PL_NOTICE_BT);
        npause = 0; pl_core_jack(&a, 0); CHECK(npause == 1);          /* AUX alone: the plug out pauses */
    }

    /* play list: answered with the state, name may be long */
    nsent = 0;
    char play[600]; const char *name = "Some Artist / Some Album";
    snprintf(play, sizeof play, "0100%04x00020003%s", (unsigned)(16 + strlen(name)), name);
    CHECK(send1(&c, play) == 0 && nsent == 1);
    CHECK(send1(&c, "0100000C0002") == -1);
    CHECK(send1(&c, "01030010000003e8") == 0);
    CHECK(send1(&c, "0201000C0000") == 0);
    CHECK(send1(&c, "06c1000C0000") == 0);

    unsigned bad = c.bad_frames, unk = c.unknown_tags;
    CHECK(send1(&c, "garbage") == -1 && c.bad_frames == bad + 1);
    CHECK(send1(&c, "0999000C0000") == -1 && c.unknown_tags == unk + 1);
    CHECK(pl_core_handle(&c, "0715000C0010", -1) == -1);
}

/* the real binary over real queues */
static void test_e2e(const char *bin){
    char pfx[64], qp[80], qu[80];
    snprintf(pfx, sizeof pfx, "/dptest%d_", (int)getpid());
    snprintf(qp, sizeof qp, "%splayer", pfx); snprintf(qu, sizeof qu, "%sui", pfx);
    struct mq_attr a; memset(&a, 0, sizeof a); a.mq_maxmsg = 10; a.mq_msgsize = 8192;
    mq_unlink(qu);
    mqd_t ui = mq_open(qu, O_RDONLY | O_CREAT, 0600, &a);        /* the UI creates /ui if the player has not yet */
    CHECK(ui != (mqd_t)-1);
    if(ui == (mqd_t)-1){ fprintf(stderr, "mq_open: %s (no mqueue support here?)\n", strerror(errno)); return; }

    char fifo[80]; snprintf(fifo, sizeof fifo, "/tmp/dptest%d_keys", (int)getpid());
    unlink(fifo); CHECK(mkfifo(fifo, 0600) == 0);
    int kw = open(fifo, O_RDWR);                                  /* a writer before the player reads: no EOF */
    CHECK(kw >= 0);
    pid_t pid = fork();
    if(pid == 0){ execl(bin, "mq_player", "-n", "-l", "-", "-q", pfx, "-k", fifo, (char*)NULL); _exit(127); }
    mqd_t pl = (mqd_t)-1;
    for(int i = 0; i < 100 && pl == (mqd_t)-1; i++){ pl = mq_open(qp, O_WRONLY | O_NONBLOCK); if(pl == (mqd_t)-1) usleep(20000); }
    CHECK(pl != (mqd_t)-1);
    struct mq_attr ga; CHECK(pl != (mqd_t)-1 && mq_getattr(pl, &ga) == 0 && ga.mq_msgsize == 8192);

    char buf[8193]; struct timespec dl;
    #define RECV() (clock_gettime(CLOCK_REALTIME, &dl), dl.tv_sec += 3, mq_timedreceive(ui, buf, sizeof buf - 1, NULL, &dl))
    CHECK(mq_send(pl, "02020008", 8, 0) == 0);
    ssize_t n = RECV();
    CHECK(n > 8 && !strncmp(buf, "a202", 4));
    CHECK(mq_send(pl, "0715000C0030", 12, 0) == 0);
    n = RECV();
    CHECK(n == 12 && !strncmp(buf, "a714000C0030", 12));
    CHECK(mq_send(pl, "0657000C0008", 12, 0) == 0);
    n = RECV();
    CHECK(n == 12 && !strncmp(buf, "a607000C0008", 12));

    /* keys: Volume up is one step and reaches the UI as a714; a release alone does nothing */
    struct input_event ev[3]; memset(ev, 0, sizeof ev);
    ev[0].type = EV_KEY; ev[0].code = 251; ev[0].value = 1;
    ev[1].type = EV_KEY; ev[1].code = 251; ev[1].value = 0;
    ev[2].type = EV_SYN;
    CHECK(write(kw, ev, sizeof ev) == (ssize_t)sizeof ev);
    n = RECV();
    CHECK(n == 12 && !strncmp(buf, "a714000C0031", 12));
    ev[0].code = ev[1].code = 252;
    CHECK(write(kw, ev, sizeof ev) == (ssize_t)sizeof ev);
    n = RECV();
    CHECK(n == 12 && !strncmp(buf, "a714000C0030", 12));
    ev[0].code = ev[1].code = 265;                                /* Power: screen toggle (dry run on the host) */
    CHECK(write(kw, ev, sizeof ev) == (ssize_t)sizeof ev);
    ev[0].code = ev[1].code = 259;                                /* the next short press comes as 259 */
    CHECK(write(kw, ev, sizeof ev) == (ssize_t)sizeof ev);

    CHECK(mq_send(pl, "0642000C0001", 12, 0) == 0);           /* USB storage on and off: logged only (-n) */
    CHECK(mq_send(pl, "0642000C0000", 12, 0) == 0);
    CHECK(mq_send(pl, "0806000C0001", 12, 0) == 0);           /* sleep timer 1 s: a dry-run power off (-n) */
    usleep(1600000);

    kill(pid, SIGTERM);
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    mq_close(pl); mq_close(ui); mq_unlink(qp); mq_unlink(qu);
    close(kw); unlink(fifo);
}

static void test_uevent(void){
    static const char add[] = "add@/devices/platform/md_ingenic,sdhci.2/mmc_host/mmc1/mmc1:aaaa/block/mmcblk0\0ACTION=add\0"   /* as on a Disc */
        "DEVPATH=/devices/platform/md_ingenic,sdhci.2/mmc_host/mmc1/mmc1:aaaa/block/mmcblk0\0SUBSYSTEM=block\0MAJOR=179\0"
        "MINOR=16\0DEVNAME=mmcblk0\0DEVTYPE=disk\0SEQNUM=774";
    static const char rem[] = "remove@/devices/x/block/mmcblk0\0ACTION=remove\0SUBSYSTEM=block\0DEVNAME=mmcblk0\0DEVTYPE=disk";
    static const char part[] = "add@/devices/x/block/mmcblk0/mmcblk0p1\0ACTION=add\0SUBSYSTEM=block\0DEVNAME=mmcblk0p1\0DEVTYPE=partition";
    static const char chg[] = "change@/devices/x/block/mmcblk0\0ACTION=change\0SUBSYSTEM=block\0DEVNAME=mmcblk0\0DEVTYPE=disk";
    static const char other[] = "add@/devices/x/block/sda\0ACTION=add\0SUBSYSTEM=block\0DEVNAME=sda\0DEVTYPE=disk";
    static const char libudev[] = "libudev\0\xfe\xed\xca\xfe";
    CHECK(sd_uevent_kind(add, sizeof add) == SDEV_ADD);
    CHECK(sd_uevent_kind(rem, sizeof rem) == SDEV_REMOVE);
    CHECK(sd_uevent_kind(part, sizeof part) == SDEV_NONE);
    CHECK(sd_uevent_kind(chg, sizeof chg) == SDEV_NONE);
    CHECK(sd_uevent_kind(other, sizeof other) == SDEV_NONE);
    CHECK(sd_uevent_kind(libudev, sizeof libudev) == SDEV_NONE);
    CHECK(sd_uevent_kind(add, 10) == SDEV_NONE);                  /* cut short: no keys */
}

static void test_bal(void){
    CHECK(hw_bal_level(1501) == 1 && hw_bal_level(3300) == 1 && hw_bal_level(801) == 1 && hw_bal_level(949) == 1);
    CHECK(hw_bal_level(1151) == 0 && hw_bal_level(1349) == 0);
    CHECK(hw_bal_level(800) == -1 && hw_bal_level(950) == -1 && hw_bal_level(1000) == -1 && hw_bal_level(1500) == -1 && hw_bal_level(0) == -1);
    int st = -1, n = 0;
    CHECK(hw_debounce(&st, &n, -1) == 0 && st == -1);                /* nothing known yet */
    CHECK(hw_debounce(&st, &n, 0) == 1 && st == 0);                  /* the first known level at once */
    CHECK(hw_debounce(&st, &n, 1) == 0 && hw_debounce(&st, &n, 1) == 0 && hw_debounce(&st, &n, 1) == 0 && st == 0);
    CHECK(hw_debounce(&st, &n, 1) == 1 && st == 1);                  /* the 4th read in a row */
    CHECK(hw_debounce(&st, &n, 0) == 0 && hw_debounce(&st, &n, 1) == 0 && n == 0);   /* one odd read is forgotten */
    CHECK(hw_debounce(&st, &n, -1) == 0 && st == 1);                 /* an in-between value keeps the state */
}

/* Charging Limit (hw.h): off at 80 % and more, on below, only with power in; "off" re-sent every 30 s */
static void test_charge(void){
    CHECK(hw_cc_power_in("Rdef\n") == 1 && hw_cc_power_in("R1p5") == 1 && hw_cc_power_in("R3p0\n") == 1);
    CHECK(hw_cc_power_in("Undefined\n") == 0 && hw_cc_power_in("Rd") == -1 && hw_cc_power_in("") == -1 && hw_cc_power_in(NULL) == -1);
    hw_charge_t s; hw_charge_init(&s);
    CHECK(hw_charge_step(&s, 1, 79, 1, 0) == 1);                     /* below the limit: charging on */
    CHECK(hw_charge_step(&s, 1, 79, 1, 5) == -1);                    /* ... once */
    CHECK(hw_charge_step(&s, 1, 80, 1, 10) == 0);                    /* at the limit: off */
    CHECK(hw_charge_step(&s, 1, 81, 1, 15) == -1);                   /* not again at once */
    CHECK(hw_charge_step(&s, 1, 81, 1, 40) == 0);                    /* but every 30 s */
    CHECK(hw_charge_step(&s, 1, 79, 1, 45) == 1);                    /* fell below: on again (stock: < 80) */
    CHECK(hw_charge_step(&s, 1, 85, 0, 50) == -1 && s.sent == -1);   /* unplugged: nothing, state forgotten */
    CHECK(hw_charge_step(&s, 1, 85, -1, 55) == -1);                  /* the Disc as USB host: nothing */
    CHECK(hw_charge_step(&s, 1, 85, 1, 60) == 0);                    /* plugged in again over the limit: off at once */
    CHECK(hw_charge_step(&s, 0, 85, 1, 65) == 1);                    /* limit switched off: charging on */
    CHECK(hw_charge_step(&s, 0, 85, 1, 200) == -1);                  /* ... and left alone */
    CHECK(hw_charge_step(&s, 1, -1, 1, 205) == -1);                  /* no gauge: leave it */
    hw_charge_init(&s);
    CHECK(hw_charge_step(&s, 1, 90, 2, 0) == 0);                     /* no cc_term node: limit like the stock player */
    CHECK(hw_charge_step(&s, 0, 90, 2, 5) == 1);                     /* switched off: on again */
    hw_charge_init(&s);
    CHECK(hw_charge_step(&s, 0, 90, 2, 0) == -1);                    /* limit off, no node: never touched */
}

static void test_dac_mpd(void){
    CHECK(dac_rate_code(44100) == 1 && dac_rate_code(48000) == 2 && dac_rate_code(96000) == 4 && dac_rate_code(192000) == 6);
    CHECK(dac_rate_code(22050) == 0);
    unsigned char a, c;
    dac_attenuation(27, 1, 0, 0x0102, &a, &c); CHECK(a == 0x68 && c == 0x66);  /* trace 02:41: volume 27, balance 0x0102 */
    dac_attenuation(27, 0, 0, 0, &a, &c);  CHECK(a == 0x72 && c == 0x72);     /* gain low: 12 more */
    dac_attenuation(119, 1, 0, 0x0003, &a, &c); CHECK(a == 1 && c == 4);    /* 0x00NN: C down */
    dac_attenuation(0, 1, 0, 0, &a, &c);   CHECK(a == 0xff && c == 0xff);     /* 0 = mute */
    dac_attenuation(130, 1, 0, 0, &a, &c); CHECK(a == 0 && c == 0);           /* above 120: clamped */
    /* the four curves read from the stock player's binary (mq_player 0x4e7800/0x4e78c0/0x4e7980/0x4e7a40) */
    static const unsigned char BAL_HIGH[120] = {
        222, 214, 206, 200, 194, 188, 182, 176, 170, 164, 160, 156, 152, 148, 144, 140, 136, 132, 128, 124,
        120, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88, 86, 84, 82, 80,
        79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60,
        59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40,
        39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20,
        19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    };
    static const unsigned char BAL_LOW[120] = {
        234, 226, 218, 212, 206, 200, 194, 188, 182, 176, 172, 168, 164, 160, 156, 152, 148, 144, 140, 136,
        132, 128, 126, 124, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98, 96, 94, 92,
        91, 90, 89, 88, 87, 86, 85, 84, 83, 82, 81, 80, 79, 78, 77, 76, 75, 74, 73, 72,
        71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52,
        51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
        31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
    };
    static const unsigned char SE_HIGH[120] = {
        218, 210, 202, 196, 190, 184, 178, 172, 166, 160, 156, 152, 148, 144, 140, 136, 132, 128, 124, 120,
        116, 112, 110, 108, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88, 86, 84, 83, 82, 81, 80,
        79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60,
        59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40,
        39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20,
        19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    };
    static const unsigned char SE_LOW[120] = {
        230, 222, 214, 208, 202, 196, 190, 184, 178, 172, 168, 164, 160, 156, 152, 148, 144, 140, 136, 132,
        128, 124, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98, 96, 95, 94, 93, 92,
        91, 90, 89, 88, 87, 86, 85, 84, 83, 82, 81, 80, 79, 78, 77, 76, 75, 74, 73, 72,
        71, 70, 69, 68, 67, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52,
        51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
        31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12,
    };
    for(int v = 1; v <= 120; v++){
        unsigned char x, y;
        dac_attenuation(v, 1, 0, 0, &x, &y); CHECK(x == SE_HIGH[v - 1] && y == x);
        dac_attenuation(v, 0, 0, 0, &x, &y); CHECK(x == SE_LOW[v - 1] && y == x);
        dac_attenuation(v, 1, 1, 0, &x, &y); CHECK(x == BAL_HIGH[v - 1] && y == x);
        dac_attenuation(v, 0, 1, 0, &x, &y); CHECK(x == BAL_LOW[v - 1] && y == x);
    }
    char q[64];
    CHECK(mc_quote(q, sizeof q, "file:///a \"b\"\\c") > 0 && !strcmp(q, "\"file:///a \\\"b\\\"\\\\c\""));
    CHECK(mc_quote(q, 4, "abcdef") == -1);
    const char *r = "volume: -1\nstate: play\nsong: 2\nelapsed: 12.345\naudio: 44100:16:2\n";
    char v[32];
    CHECK(mc_get(r, "state", v, sizeof v) && !strcmp(v, "play"));
    CHECK(mc_get(r, "audio", v, sizeof v) && !strcmp(v, "44100:16:2"));
    CHECK(mc_get(r, "song", v, sizeof v) && !strcmp(v, "2"));
    CHECK(!mc_get(r, "songid", v, sizeof v));
    CHECK(!mc_get(r, "stat", v, sizeof v));
}

int main(int argc, char **argv){
    test_proto();
    test_dac_mpd();
    test_bal();
    test_charge();
    test_uevent();
    test_core();
    if(argc > 1) test_e2e(argv[1]);
    else fprintf(stderr, "(end-to-end part skipped: no binary given)\n");
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("player_test: all checks passed\n");
    return 0;
}
