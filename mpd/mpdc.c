/* SPDX-License-Identifier: GPL-3.0-or-later */
/* mpdc: send each argument as one command to MPD over a unix socket, print the replies. Test tool. */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int line(int fd, char *b, int n){ int i = 0; while(i < n - 1){ char c; if(read(fd, &c, 1) != 1) return -1; if(c == '\n') break; b[i++] = c; } b[i] = 0; return i; }
int main(int argc, char **argv){
    if(argc < 3){ fprintf(stderr, "usage: mpdc <socket> <command>...\n"); return 2; }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, argv[1], sizeof a.sun_path - 1);
    if(connect(fd, (struct sockaddr *)&a, sizeof a) != 0){ perror("connect"); return 1; }
    char b[4096]; if(line(fd, b, sizeof b) < 0) return 1; printf("%s\n", b);
    for(int i = 2; i < argc; i++){
        dprintf(fd, "%s\n", argv[i]); printf("> %s\n", argv[i]);
        while(line(fd, b, sizeof b) >= 0){ printf("%s\n", b); if(!strcmp(b, "OK") || !strncmp(b, "ACK", 3)) break; }
    }
    return 0;
}
