/* SPDX-License-Identifier: GPL-3.0-or-later */
/* dacstart: run the stock player's DAC start sequence (trace run 1, 578 s; docs/MQ_PLAYER_TRACE.md) by hand, so MPD can
 * be tested on the DAC before the own player does it. Test tool.
 *   dacstart [-r] <rate code> <volume byte>
 *     rate code: 1 = 44.1 kHz, 2 = 48 kHz, 4 = 96 kHz, 6 = 192 kHz (M52/M54, run 3)
 *     volume byte: attenuation, 0x72 = the stock start value; larger = quieter
 *     -r: first _IO('M',48) on A, as the stock player does before its very first stream
 * Keeps both DAC devices open until it is killed: the stock player holds them open all the time, and closing them may
 * switch the DAC off again (test 2026-11-15: the data ran through far faster than real time after dacstart had exited). */
#include <signal.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define IOW1(nr) (0x80014d00UL | (nr))
static void w1(int fd, const char *n, int nr, unsigned char v){
    int r = ioctl(fd, IOW1(nr), &v);
    printf("%s M%d = 0x%02x -> %d\n", n, nr, v, r);
}
int main(int argc, char **argv){
    int i = 1, reset = 0;
    if(argc > 1 && !strcmp(argv[1], "-r")){ reset = 1; i++; }
    if(argc - i != 2){ fprintf(stderr, "usage: dacstart [-r] <rate code> <volume byte>\n"); return 2; }
    unsigned char rate = (unsigned char)strtoul(argv[i], 0, 0), vol = (unsigned char)strtoul(argv[i + 1], 0, 0);
    int a = open("/dev/cs43131", O_RDWR), c = open("/dev/cs43131c", O_RDWR);
    if(a < 0 || c < 0){ perror("open dac"); return 1; }
    if(reset) printf("A M48 -> %d\n", ioctl(a, 0x20004d30UL, 0));
    w1(a, "A", 56, 3); w1(c, "C", 60, 3);
    usleep(400000);
    w1(a, "A", 56, 0); w1(c, "C", 60, 0);
    w1(a, "A", 52, rate); w1(c, "C", 54, rate);
    w1(a, "A", 41, 0); w1(c, "C", 50, 0);
    w1(a, "A", 45, vol); w1(a, "A", 66, vol); w1(c, "C", 47, vol); w1(c, "C", 68, vol);
    printf("A M46 -> %d\n", ioctl(a, 0x20004d2eUL, 0));
    printf("holding /dev/cs43131 and /dev/cs43131c open; kill to release\n");
    fflush(stdout);
    for(;;) pause();
}
