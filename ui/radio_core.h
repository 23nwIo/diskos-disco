/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Web radio, the pure part (no LVGL, no threads, no device paths): station records, the favourites file, the
 * radio-browser.info reply, and the UPnP/DLNA messages that hand a stream to the stock player's own renderer.
 * Host-tested in ui/tests/radio_test.c. */
#ifndef RADIO_CORE_H
#define RADIO_CORE_H
#include <stddef.h>

#define RC_NAME_LEN 96
#define RC_URL_LEN  512
#define RC_MAX_FAV  60          /* favourites kept */
#define RC_MAX_HITS 40          /* stations per directory reply */

typedef struct {
    char name[RC_NAME_LEN];
    char url[RC_URL_LEN];
    char codec[16];             /* "MP3", "AAC", "OGG"... as the directory reports it ("" unknown) */
    char country[48];
    int  bitrate;               /* kbit/s, 0 unknown */
} rc_station_t;

/* A stream URL we will hand to the player: http:// or https://, a host, printable ASCII, no spaces/quotes/angle
 * brackets, shorter than RC_URL_LEN. */
int  rc_url_ok(const char *url);
/* "MP3 · 128 kbps · Germany" (only the parts that are known); "" when nothing is */
void rc_detail(const rc_station_t *s, char *out, size_t n);
/* Percent-encodes everything but unreserved characters (RFC 3986) - safe in a query string. */
void rc_urlencode(const char *in, char *out, size_t n);
/* XML text/attribute escaping (& < > " '). Returns 0, or -1 if it did not fit. */
int  rc_xml_escape(const char *in, char *out, size_t n);
/* MIME type for the DIDL protocolInfo, from the directory's codec name or else the URL ("audio/mpeg" default). */
const char *rc_mime(const char *codec, const char *url);

/* ---- favourites file: one station per line, "name<TAB>url<TAB>codec<TAB>bitrate<TAB>country" */
int  rc_fav_parse(const char *text, rc_station_t *out, int max);          /* stations read (bad lines skipped) */
int  rc_fav_format(const rc_station_t *s, int n, char *out, size_t cap);  /* bytes written, -1 if it did not fit */
int  rc_fav_find(const rc_station_t *s, int n, const char *url);          /* index, -1 */

/* ---- radio-browser.info: the JSON array of /json/stations/... Uses url_resolved (falls back to url), skips
 * stations whose URL fails rc_url_ok. Returns stations read, -1 if the reply is not a JSON array. */
int  rc_parse_stations(const char *json, size_t len, rc_station_t *out, int max);

/* ---- UPnP AVTransport (the renderer the player runs in DLNA work mode) */
/* From a device description: the AVTransport control URL made absolute against the description's URL.
 * Returns 0, -1 if the description has no AVTransport service. */
int  rc_desc_control_url(const char *desc, const char *desc_url, char *out, size_t n);
/* The SOAP envelope for an AVTransport action; args are already-escaped XML elements ("" for none). */
int  rc_soap_body(const char *action, const char *args, char *out, size_t n);
/* SetAVTransportURI arguments for a station: the URL plus DIDL-Lite metadata (title = station name, artist =
 * "Web radio", album = the detail line), escaped twice as the action requires. */
int  rc_set_uri_args(const rc_station_t *s, char *out, size_t n);
/* The value of <tag>...</tag> in a reply (first occurrence), unescaped for the five XML entities. */
int  rc_xml_value(const char *xml, const char *tag, char *out, size_t n);
/* Split http://host[:port]/path. Returns 0, -1 for anything else (https is not used by UPnP control). */
int  rc_split_http(const char *url, char *host, size_t hn, int *port, char *path, size_t pn);

/* ---- ICY (SHOUTcast/Icecast) in-stream metadata: asked for with "Icy-MetaData: 1", the reply carries
 * "icy-metaint: N"; after every N audio bytes comes one length byte L and L*16 bytes of "StreamTitle='...';..." */
#define RC_ICY_METAINT_MAX 262144
int  rc_icy_head_end(const char *buf, size_t n);              /* offset just past the header block, -1 not complete */
long rc_icy_metaint(const char *head, size_t n);              /* icy-metaint (1..RC_ICY_METAINT_MAX), -1 none */
/* StreamTitle from one metadata block: UTF-8 (Latin-1 is converted), control characters as spaces, trimmed.
 * Returns 0 (out may be "": the station sends an empty title), -1 if the block has no StreamTitle. */
int  rc_icy_title(const char *meta, size_t n, char *out, size_t on);
/* "Artist - Title" at the first " - ": 1 split; 0 not (title = the whole text, artist = ""). */
int  rc_icy_split(const char *t, char *artist, size_t an, char *title, size_t tn);
/* ---- Ogg streams (Vorbis, Opus) carry no ICY blocks: each song starts a new chained stream whose comment header
 * (TITLE=, ARTIST=) the server sends again to every new listener. */
size_t rc_ogg_payload(const unsigned char *in, size_t n, unsigned char *out, size_t on);  /* the first stream's page bodies, joined */
int  rc_ogg_tags(const unsigned char *payload, size_t n, char *artist, size_t an, char *title, size_t tn);   /* 0 found, -1 none */
#endif
