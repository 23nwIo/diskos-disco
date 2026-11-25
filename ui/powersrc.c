/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Power source and battery, read in one place: see powersrc.h. */
#include "powersrc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_line(const char *path, char *buf, size_t n){
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return -1;
    ssize_t r = read(fd, buf, n - 1); int e = errno;
    close(fd);
    if(r <= 0){ errno = r < 0 ? e : EIO; return -1; }
    buf[r] = 0;
    return 0;
}

int powersrc_gauge(int *pct, int *charging){
    char b[32];
    int p = -1; long cur = 0;
    if(read_line(PS_GAUGE_CAPACITY, b, sizeof b) == 0){
        char *end; long v = strtol(b, &end, 10);
        if(end != b && v >= 0 && v <= 100) p = (int)v;
    }
    if(read_line(PS_GAUGE_CURRENT, b, sizeof b) == 0) cur = strtol(b, NULL, 10);
    if(pct) *pct = p;
    if(charging) *charging = cur > 0;
    return p >= 0 ? 0 : -1;
}

/* cc_term is the current advertisement on CC - Rdef, R1p5 or R3p0 whenever a wall charger, power bank or computer
 * supplies power, whether or not the battery draws any; "Undefined" with nothing plugged in. Device-probed 2026-11-14
 * (V2.57): wall charger, battery held at 80 % by Charging Limit, no charge current -> "Rdef"; unplugged ->
 * "Undefined"; the gauge and the USB controller showed no difference there. But cc_term alone is not "power in": with
 * an unpowered USB device (a sound card) the Disc is the source and cc_term shows its OWN advertisement, R3p0 (device
 * log 2026-11-25: R3p0 | Source | 6 AttachedSource; charger: Rdef | Sink | 4 AttachedSink). So typec_state decides
 * the direction. */
int powersrc_cc_term_parse(const char *v){
    if(!v) return -1;
    size_t n = strcspn(v, " \t\r\n");
    if(n == 4 && (!strncmp(v, "Rdef", 4) || !strncmp(v, "R1p5", 4) || !strncmp(v, "R3p0", 4))) return 1;
    if(n == 9 && !strncmp(v, "Undefined", 9)) return 0;
    return -1;
}

int powersrc_port_parse(const char *cc_term, const char *typec_state){
    if(!typec_state) return powersrc_cc_term_parse(cc_term);    /* no direction node: cc_term alone, as before */
    if(strstr(typec_state, "Source")) return 0;      /* AttachWaitSource / AttachedSource: the Disc powers the device */
    if(strstr(typec_state, "Unattached")) return 0;
    if(strstr(typec_state, "Sink")) return powersrc_cc_term_parse(cc_term) == 1 ? 1 : -1;   /* AttachWaitSink / AttachedSink */
    return -1;                                       /* toggling, error recovery, unknown text */
}

int powersrc_port(void){
    char cc[32], st[48];
    if(read_line(PS_CC_TERM, cc, sizeof cc) < 0) return errno == ENOENT ? PS_PORT_NONE : -1;
    return powersrc_port_parse(cc, read_line(PS_TYPEC_STATE, st, sizeof st) == 0 ? st : NULL);
}
