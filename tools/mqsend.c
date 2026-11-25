/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* mqsend.c - send raw frames to the /player message queue, as mq_ui does.
 *
 * Like ui/psend.c, but takes several frames in one call and does not block when the queue is full.
 * Example: "mqsend 0666000C0001" is the stock player's S/PDIF switch (value 1 = S/PDIF, other values = DAC;
 * docs/PLAYER_PROTOCOL.md). Written for the S/PDIF investigation of 2026-11-25.
 *
 * BUILD: mipsel-linux-musl-gcc -O2 -static -o mqsend mqsend.c
 * USAGE (on the device): mqsend FRAME...
 */
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv){
    if(argc < 2){ fprintf(stderr, "usage: mqsend FRAME...\n"); return 2; }
    mqd_t q = mq_open("/player", O_WRONLY | O_NONBLOCK);
    if(q == (mqd_t)-1){ fprintf(stderr, "mq_open /player: %s\n", strerror(errno)); return 1; }
    int rc = 0;
    for(int i = 1; i < argc; i++){
        if(mq_send(q, argv[i], strlen(argv[i]), 0) != 0){ fprintf(stderr, "%s: mq_send: %s\n", argv[i], strerror(errno)); rc = 1; }
        else printf("%s: sent\n", argv[i]);
    }
    mq_close(q);
    return rc;
}
