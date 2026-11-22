/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* Bandcamp, the pure part (no LVGL, no threads, no device paths): the search reply, the album/track page, an artist's
 * /music page and the stream list the own player plays. Bandcamp has no public API for listeners; these are the same
 * requests its own web pages make (docs/BANDCAMP.md):
 *   search:  POST https://bandcamp.com/api/bcsearch_public_api/1/autocomplete_elastic (JSON body bc_search_body)
 *   release: GET  <album or track page>: <script data-tralbum="{JSON}">, trackinfo[].file["mp3-128"] = the stream
 *   artist:  GET  <artist>/music: the release grid (li data-item-id) plus ol data-client-items="[JSON]"
 * Checked against pages fetched on the Disc 2026-11-22 (ui/tests/fixtures/bandcamp). Host-tested in
 * ui/tests/bandcamp_test.c. */
#ifndef BANDCAMP_CORE_H
#define BANDCAMP_CORE_H
#include <stddef.h>

#define BC_NAME_LEN   128
#define BC_URL_LEN    512
#define BC_MAX_HITS   40          /* search results kept */
#define BC_MAX_REL    300         /* releases of an artist / label */
#define BC_MAX_TRACKS 200         /* tracks of a release */

typedef struct {
    char type;                    /* 'b' artist or label, 'a' album, 't' track */
    char name[BC_NAME_LEN];       /* the artist, album or track */
    char artist[BC_NAME_LEN];     /* albums and tracks: by whom ("" artists) */
    char album[BC_NAME_LEN];      /* tracks: on which album ("" none known) */
    char url[BC_URL_LEN];         /* the artist's site (https://x.bandcamp.com) or the release page */
    char art[BC_URL_LEN];         /* a small image (https://f4.bcbits.com/img/...), "" none */
} bc_item_t;

typedef struct {
    char title[BC_NAME_LEN];
    char artist[BC_NAME_LEN];     /* the track's own artist (compilations), else the release's */
    char stream[BC_URL_LEN];      /* the 128 kbit/s MP3 (expires after about a day), "" = not streamable */
    char page[BC_URL_LEN];        /* the track's page, "" none */
    long ms;                      /* duration, 0 unknown */
    int  num;                     /* track number, 0 unknown */
} bc_track_t;

typedef struct {
    char type;                    /* 'a' album, 't' a single track's page */
    char title[BC_NAME_LEN];      /* the album (or the track) */
    char artist[BC_NAME_LEN];
    char album[BC_NAME_LEN];      /* a track page: its album ("" none / unknown) */
    char url[BC_URL_LEN];         /* the page itself */
    char art[BC_URL_LEN];         /* the cover, 350 px */
    int  ntracks;                 /* all tracks, streamable or not */
} bc_release_t;

/* An address we fetch: https://, a host, printable ASCII without spaces, quotes or angle brackets, < BC_URL_LEN. */
int  bc_url_ok(const char *url);
/* "https://host" of url (no path). 0, -1 not an https URL. */
int  bc_url_base(const char *url, char *out, size_t n);
/* path made absolute against base ("https://host"); a full https URL stays; the query and fragment go. 0 / -1. */
int  bc_url_join(const char *base, const char *path, char *out, size_t n);
/* The JSON body of a search for q. Returns its length, -1 when it did not fit. */
int  bc_search_body(const char *q, char *out, size_t n);
/* The search reply: artists/labels, albums and tracks in Bandcamp's order (fans are left out). Returns the count, -1
 * when the text is not the reply. */
int  bc_parse_search(const char *json, size_t len, bc_item_t *out, int max);
/* HTML entities (&quot; &amp; &lt; &gt; &#39; &#NNN; &#xHH; &nbsp;) decoded in place. */
void bc_entities(char *s);
/* The value of the first attribute name="..." in html, entity-decoded, malloc'd (free it). NULL when there is none. */
char *bc_html_attr(const char *html, const char *name);
/* An album or track page. Fills *r and up to max tracks; returns the track count, -1 when the page has no release. */
int  bc_parse_release(const char *html, bc_release_t *r, bc_track_t *t, int max);
/* An artist's or label's /music page (base: its site, "https://x.bandcamp.com"): the releases, newest first as the page
 * has them. Returns the count, -1 when the page has none. An artist with one release gets that release's own page
 * instead: then 0 and *one is filled (if not NULL). */
int  bc_parse_music(const char *html, const char *base, bc_item_t *out, int max, bc_release_t *one);
/* The own player's stream list (own_player.h OPL_BC_LIST) for the streamable tracks of r. map[k] = the track index of
 * line k (may be NULL). Returns the number of lines, -1 when it did not fit; *len = the text's length. */
int  bc_list_format(const bc_release_t *r, const bc_track_t *t, int n, char *out, size_t cap, int *map, size_t *len);
#endif
