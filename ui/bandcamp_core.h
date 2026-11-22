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
    char cover[BC_URL_LEN];       /* the same cover, 1200 px (downloads), "" none */
    char date[16];                /* the release date, "YYYY-MM-DD", "" unknown */
    char free_dl[BC_URL_LEN];     /* the free download page (name your price / free), "" none */
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

/* ---- downloads to the SD card (docs/BANDCAMP.md "Downloads") */
#define BC_DL_DIR "BandcampDownloads"
/* s as one file or folder name that FAT32 and ext4 both take: no / \ : * ? " < > | or control characters (they
 * become '_'), no leading or trailing dots and spaces, at most 100 bytes (cut on a UTF-8 boundary). "" becomes
 * fallback. Returns the length. */
int  bc_safe_name(const char *s, const char *fallback, char *out, size_t n);
/* "<root>/BandcampDownloads/<artist>/<album>" for r (a track page without an album: the track's title). 0, -1 too long. */
int  bc_dl_dir(const char *root, const bc_release_t *r, char *out, size_t n);
/* The file name of track t: "NN Title.mp3" (NN = its number, two digits; no number: "Title.mp3"). 0, -1 too long. */
int  bc_dl_file(const bc_track_t *t, char *out, size_t n);
/* The size of an ID3v2 tag at the start of buf (header, footer and all), 0 when there is none. */
size_t bc_id3_skip(const unsigned char *buf, size_t len);
/* An ID3v2.4 tag (UTF-8) for track t of r (total = the release's track count): TIT2, TPE1, TALB, TPE2, TRCK "n/N",
 * TDRC, and the cover as APIC when jpg is not NULL. malloc'd, *len its size; NULL out of memory. */
unsigned char *bc_id3_tag(const bc_release_t *r, const bc_track_t *t, int total, const unsigned char *jpg, size_t jlen, size_t *len);
/* ---- the best quality: Bandcamp's own download (free / name-your-price releases, r.free_dl)
 * 1. GET r.free_dl: its data-blob has digital_items[0].downloads{enc:{url}}; bc_parse_dlpage picks the first of
 *    prefs (e.g. "flac", "mp3-v0", ...) it offers: the format's url and "album" | "track".
 * 2. GET that url with /download/ -> /statdownload/ and "&.vrs=1&.rand=N" appended (bc_stat_url): a JavaScript line
 *    Downloads.statResult({...}); bc_parse_stat: 1 "ok" with download_url, 0 "err" with a retry_url (then step 2
 *    again with it), -1 neither.
 * 3. GET download_url: an album is a ZIP of the tracks and the cover, a track the file itself. */
int  bc_parse_dlpage(const char *html, const char *const *prefs, char *enc, size_t en, char *url, size_t un, char *type, size_t tn,
                     long long *bytes);   /* bytes: its size_mb ("118MB"), 0 unknown */
int  bc_stat_url(const char *url, unsigned rnd, char *out, size_t n);
int  bc_parse_stat(const char *text, char *url, size_t n);
#endif
