/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Audiobook bookmarks on the card (bookmarks.c, sdstate.c) against a temporary folder as the card. */
#include "../bookmarks.h"
#include "../sdstate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)
int main(void){
    char dir[] = "/tmp/bmtestXXXXXX";
    if(!mkdtemp(dir)) return 1;
    sds_test_root(dir);
    int n = -1;
    CHECK(bm_state(&n) == 0 && n == 0);                                  /* no file yet */
    long pos; int done; long upd; char m[BM_PATH_MAX];
    CHECK(bm_get("/tmp/sdcard/A.m4b", m, sizeof m, &pos, &done, &upd) == 0);
    CHECK(bm_put("/tmp/sdcard/A.m4b", "/tmp/sdcard/A.m4b", 1234, 0, 100) == 1);
    CHECK(bm_put("/tmp/sdcard/Tab\there/B.m4b", "/tmp/sdcard/Tab\there/B.m4b", 99, 1, 200) == 1);
    CHECK(bm_put("/tmp/sdcard/A.m4b", "/tmp/sdcard/A.m4b", 5678, 0, 300) == 1);   /* update, not a second line */
    bm_reset();                                                          /* read back from the file */
    CHECK(bm_state(&n) == 1 && n == 2);
    CHECK(bm_get("/tmp/sdcard/A.m4b", m, sizeof m, &pos, &done, &upd) == 1 && pos == 5678 && upd == 300 && !done);
    CHECK(bm_get("/tmp/sdcard/Tab\there/B.m4b", m, sizeof m, &pos, &done, &upd) == 1 && pos == 99 && done == 1
          && !strcmp(m, "/tmp/sdcard/Tab\there/B.m4b"));
    /* moved: a folder rename carries the bookmark along; a same-named prefix does not match */
    CHECK(bm_put("/tmp/sdcard/Tabby.m4b", NULL, 7, 0, 1) == 1);
    CHECK(bm_moved("/tmp/sdcard/Tab\there", "/tmp/sdcard/Books") == 1);
    bm_reset();
    CHECK(bm_get("/tmp/sdcard/Books/B.m4b", m, sizeof m, &pos, NULL, NULL) == 1 && pos == 99 && !strcmp(m, "/tmp/sdcard/Books/B.m4b"));
    CHECK(bm_get("/tmp/sdcard/Tab\there/B.m4b", NULL, 0, NULL, NULL, NULL) == 0);
    CHECK(bm_get("/tmp/sdcard/Tabby.m4b", m, sizeof m, &pos, NULL, NULL) == 1 && pos == 7 && !strcmp(m, "/tmp/sdcard/Tabby.m4b"));
    /* a move onto an existing bookmark: the moved one wins, no duplicate */
    CHECK(bm_moved("/tmp/sdcard/Tabby.m4b", "/tmp/sdcard/A.m4b") == 1);
    bm_reset();
    CHECK(bm_state(&n) == 1 && n == 2);
    CHECK(bm_get("/tmp/sdcard/A.m4b", NULL, 0, &pos, NULL, NULL) == 1 && pos == 7);
    /* the file changes on the card (another card, an edit): read again */
    char p[512]; snprintf(p, sizeof p, "%s/.diskos/state/books.tsv", dir);
    FILE *f = fopen(p, "w"); fprintf(f, "# x\n/tmp/sdcard/C.m4b\t/tmp/sdcard/C.m4b\t42\t5\t0\nbroken line\n"); fclose(f);
    sleep(1); f = fopen(p, "a"); fclose(f);   /* (mtime granularity) */
    CHECK(bm_get("/tmp/sdcard/C.m4b", NULL, 0, &pos, NULL, NULL) == 1 && pos == 42);
    CHECK(bm_state(&n) == 1 && n == 1);
    /* many at once (the import from song.db) */
    const char *bk[2] = { "/tmp/sdcard/D.m4b", "/tmp/sdcard/E.m4b" }; long ps[2] = { 1, 2 }, us[2] = { 3, 4 }; int cs[2] = { 0, 1 };
    CHECK(bm_put_many(2, bk, bk, ps, cs, us) == 1);
    bm_reset();
    CHECK(bm_state(&n) == 1 && n == 3);
    char cmd[600]; snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if(system(cmd) != 0) return 1;
    if(fails) return 1;
    printf("bookmarks_test: all checks passed\n");
    return 0;
}
