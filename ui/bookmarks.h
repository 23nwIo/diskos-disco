/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
#ifndef DISKOS_BOOKMARKS_H
#define DISKOS_BOOKMARKS_H

/* Audiobook bookmarks on the SD card, next to the books (<card>/.diskos/state/books.tsv; Jo, 2026-11-16: no recurring
 * flash writes, the bookmarks belong with the books). Before, they were the BOOK_PROGRESS table in song.db on the NAND;
 * musicdb.c moves those rows over once (mdb_books_import). A different card brings its own file.
 * One line per book: book path, member path, position ms, updated (unix s), completed; tab separated, \t \n \\
 * escaped. Paths are as the player sees them (/tmp/sdcard/...). Thread-safe. */
#define BM_FILE "books.tsv"
#define BM_PATH_MAX 512

/* the card's file: -1 no card (or SD I/O not allowed now), 0 no file yet, 1 there (the count in *n) */
int bm_state(int *n);
/* 1 bookmark found, 0 none, -1 no card / read error (a caller must not treat that as "start from zero") */
int bm_get(const char *book, char *member, int member_cap, long *position_ms, int *completed, long *updated);
/* store one and write the file; 1 written, 0 failed (retry later) */
int bm_put(const char *book, const char *member, long position_ms, int completed, long updated);
/* store several, written once: bm_put_many(n, books, members, positions, completed, updated); 1 written, 0 failed */
int bm_put_many(int n, const char *const *book, const char *const *member, const long *pos, const int *completed,
                const long *updated);
/* a file or folder moved on the card: rename book and member paths at or under oldp; 1 ok (or nothing to do), 0 fail */
int bm_moved(const char *oldp, const char *newp);
/* forget the cached copy (the next call reads the card again) */
void bm_reset(void);

#endif
