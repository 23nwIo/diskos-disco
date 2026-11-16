/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 diskOS contributors */
/* A minimal UPnP AV MediaRenderer (AVTransport only) for the Radio app (docs/RADIO.md): the stock player has a DLNA
 * renderer on port 49494 in work mode 0C, and ui/radio.c hands it the station with SetAVTransportURI + Play. The own
 * player answers the same requests while it is in that work mode, so the Radio app works with either player and the
 * UI does not need to know which one runs.
 *
 * Only on 127.0.0.1 (the Radio app finds it there, ui/radio.c find_renderer): nothing on the network can reach it, and
 * there is no SSDP. One request per connection (Connection: close), answered at once. */
#ifndef DISKOS_PLAYER_UPNP_H
#define DISKOS_PLAYER_UPNP_H
#include <stddef.h>

#define UP_PORT      49494
#define UP_DESC_PATH "/description.xml"
#define UP_CTL_PATH  "/AVTransport/control"
#define UP_REQ_MAX   65536

typedef void (*up_log_fn)(const char *fmt, ...);
/* One AVTransport action: name ("Play"), the SOAP body. Writes the action's response arguments (XML elements, already
 * escaped; "" none) to out. Returns 0, or a UPnP error code (e.g. 701 transition not available, 716 not found). */
typedef int (*up_action_fn)(void *ctx, const char *action, const char *body, char *out, size_t cap);

/* The listening socket on 127.0.0.1:port (non-blocking), -1 with the reason logged. */
int  up_listen(int port, up_log_fn log);
/* The listening socket is readable: accept one connection, read the request (2 s at most), answer it, close. */
void up_serve(int lfd, up_action_fn fn, void *ctx, up_log_fn log);

/* ---- pure parts (host-tested in ui/tests/player_test.c) */
/* A complete request in buf[0..n)? 1 yes (method, path, the SOAPACTION's action name, the body), 0 not yet, -1 bad. */
int  up_parse(const char *buf, size_t n, char *method, size_t mcap, char *path, size_t pcap, char *action, size_t acap,
              const char **body, size_t *blen);
/* The text of the first <tag>...</tag> (any namespace prefix, attributes allowed) in xml, with the five XML entities
 * decoded once. 0, or -1 when the tag is missing or the text did not fit. */
int  up_xml_value(const char *xml, const char *tag, char *out, size_t cap);
/* XML escape (& < > " '). 0, or -1 when it did not fit. */
int  up_xml_escape(const char *in, char *out, size_t cap);
/* The whole HTTP reply for a request: the device description, a SOAP response or fault, 404. Returns its length. */
int  up_reply(const char *method, const char *path, const char *action, const char *body, up_action_fn fn, void *ctx,
              char *out, size_t cap);
#endif
