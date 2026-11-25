/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* dacpower.c - power the CS43131 DAC down or up by hand, as the stock mq_player does.
 *
 * The stock player's dac_power_down (0x4e83b8) sends _IO('M',47) = 0x20004d2f to /dev/cs43131 only; power up is
 * _IO('M',48) = 0x20004d30. On a Disc with firmware V2.57, /dev/cs43131c answers M47 with EPERM. Written for the
 * S/PDIF investigation of 2026-11-25 (docs/MPD_CORE.md); the own player does the same in ui/player/dac.c.
 *
 * BUILD: mipsel-linux-musl-gcc -O2 -static -o dacpower dacpower.c
 * USAGE (on the device): dacpower down|up [device...]   (default device /dev/cs43131)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(int argc, char **argv){
    if(argc < 2 || (strcmp(argv[1], "down") && strcmp(argv[1], "up"))){ fprintf(stderr, "usage: dacpower down|up [device...]\n"); return 2; }
    unsigned long req = strcmp(argv[1], "down") ? 0x20004d30UL : 0x20004d2fUL;
    static char *def[] = { "/dev/cs43131" };
    char **devs = argc > 2 ? argv + 2 : def; int n = argc > 2 ? argc - 2 : 1, rc = 0;
    for(int i = 0; i < n; i++){
        int fd = open(devs[i], O_RDWR);
        if(fd < 0){ fprintf(stderr, "open %s: %s\n", devs[i], strerror(errno)); rc = 1; continue; }
        if(ioctl(fd, req, 0) != 0){ fprintf(stderr, "%s: ioctl 0x%lx: %s\n", devs[i], req, strerror(errno)); rc = 1; }
        else printf("%s: power %s ok\n", devs[i], argv[1]);
        close(fd);
    }
    return rc;
}
