/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Host test of the own player skeleton (ui/player): frame parsing and formatting, the command handling in core.c, and
 * an end-to-end run of the real binary over POSIX message queues (watchdog off, own queue names).
 * Run: make -C ui/tests player_test && ./ui/tests/player_test ./ui/tests/diskos-player-host */
#include "../player/core.h"
#include "../player/proto.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

    /* work mode: 8 is announced once (as the stock player announces a change), other modes are not claimed */
    nsent = 0;
    CHECK(send1(&c, "0657000C0008") == 0 && c.work_mode == 8 && nsent == 1 && !strcmp(sent[0], "a607000C0008"));
    CHECK(send1(&c, "0657000C0008") == 0 && nsent == 1);
    CHECK(send1(&c, "0657000C0006") == 0 && c.work_mode == 8 && nsent == 1);
    CHECK(send1(&c, "0666000C0002") == 0 && c.route == -1);
    CHECK(send1(&c, "0666000C0006") == 0 && c.route == 6);

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

    pid_t pid = fork();
    if(pid == 0){ execl(bin, "mq_player", "-n", "-l", "-", "-q", pfx, (char*)NULL); _exit(127); }
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

    kill(pid, SIGTERM);
    int st = 0; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    mq_close(pl); mq_close(ui); mq_unlink(qp); mq_unlink(qu);
}

int main(int argc, char **argv){
    test_proto();
    test_core();
    if(argc > 1) test_e2e(argv[1]);
    else fprintf(stderr, "(end-to-end part skipped: no binary given)\n");
    if(fails){ fprintf(stderr, "%d check(s) failed\n", fails); return 1; }
    printf("player_test: all checks passed\n");
    return 0;
}
