/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Host tests for the pure web radio core. Build/run: make -C ui CROSS= radio-test */
#include "../radio_core.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do{ if(!(c)){ fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } }while(0)

static void test_url(void){
    CHECK(rc_url_ok("http://ice1.somafm.com/groovesalad-128-mp3"));
    CHECK(rc_url_ok("https://icecast.walmradio.com:8443/classic_opus"));
    CHECK(!rc_url_ok("ftp://x/y"));
    CHECK(!rc_url_ok("http://"));
    CHECK(!rc_url_ok("http:///path"));
    CHECK(!rc_url_ok("http://a b/c"));
    CHECK(!rc_url_ok("http://x/'; rm -rf /"));
    CHECK(!rc_url_ok("http://x/\xC3\xA4"));
    CHECK(!rc_url_ok(NULL));
    char big[600]; memset(big, 'a', sizeof big); memcpy(big, "http://", 7); big[599] = 0;
    CHECK(!rc_url_ok(big));
}
static void test_encode(void){
    char o[128];
    rc_urlencode("Radio Paradise & ä", o, sizeof o);
    CHECK(!strcmp(o, "Radio%20Paradise%20%26%20%C3%A4"));
    rc_urlencode("abc", o, 3); CHECK(!strcmp(o, "ab"));
    CHECK(rc_xml_escape("a<b>&\"c'", o, sizeof o) == 0 && !strcmp(o, "a&lt;b&gt;&amp;&quot;c&apos;"));
    CHECK(rc_xml_escape("&&&&", o, 8) == -1);
    CHECK(!strcmp(rc_mime("MP3", ""), "audio/mpeg"));
    CHECK(!strcmp(rc_mime("AAC+", ""), "audio/aac"));
    CHECK(!strcmp(rc_mime("OGG", ""), "audio/ogg"));
    CHECK(!strcmp(rc_mime("", "https://x/classic_opus"), "audio/ogg"));
    CHECK(!strcmp(rc_mime("", "http://x/flac"), "audio/flac"));
    CHECK(!strcmp(rc_mime(NULL, "http://x/stream"), "audio/mpeg"));
}
static void test_detail(void){
    rc_station_t s = {0}; char d[128];
    rc_detail(&s, d, sizeof d); CHECK(!strcmp(d, ""));
    strcpy(s.codec, "MP3"); s.bitrate = 128; strcpy(s.country, "Germany");
    rc_detail(&s, d, sizeof d); CHECK(!strcmp(d, "MP3 \xC2\xB7 128 kbps \xC2\xB7 Germany"));
    s.codec[0] = 0; rc_detail(&s, d, sizeof d); CHECK(!strcmp(d, "128 kbps \xC2\xB7 Germany"));
}
static void test_fav(void){
    const char *txt = "# comment\nGroove Salad\thttp://ice1.somafm.com/groovesalad-128-mp3\tMP3\t128\tUSA\r\n"
                      "bad line without url\n"
                      "Dup\thttp://ice1.somafm.com/groovesalad-128-mp3\n"
                      "Evil\tjavascript:alert(1)\n"
                      "Opus\thttps://icecast.walmradio.com:8443/classic_opus\n";
    rc_station_t s[8];
    int n = rc_fav_parse(txt, s, 8);
    CHECK(n == 2);
    CHECK(!strcmp(s[0].name, "Groove Salad") && s[0].bitrate == 128 && !strcmp(s[0].codec, "MP3") && !strcmp(s[0].country, "USA"));
    CHECK(!strcmp(s[1].name, "Opus") && s[1].bitrate == 0 && s[1].codec[0] == 0);
    CHECK(rc_fav_find(s, n, "https://icecast.walmradio.com:8443/classic_opus") == 1);
    CHECK(rc_fav_find(s, n, "http://nope/") == -1);
    strcpy(s[1].name, "Tab\there");
    char out[1024]; int k = rc_fav_format(s, n, out, sizeof out);
    CHECK(k > 0);
    rc_station_t r[8]; int m = rc_fav_parse(out, r, 8);
    CHECK(m == 2 && !strcmp(r[1].name, "Tab here") && !strcmp(r[0].url, s[0].url));
    CHECK(rc_fav_format(s, n, out, 20) == -1);
    CHECK(rc_fav_parse(txt, s, 1) == 1);
}
static void test_json(void){
    const char *js =
        "[{\"changeuuid\":\"x\",\"name\":\"  Radio \\u00e4 One \",\"url\":\"http://a/pls\",\"url_resolved\":\"http://a/stream\","
        "\"tags\":\"pop,rock\",\"country\":\"Germany\",\"codec\":\"MP3\",\"bitrate\":128,\"lastcheckok\":1,\"geo\":{\"lat\":1,\"x\":[1,2,{\"y\":3}]}},"
        "{\"name\":\"Broken\",\"url\":\"http://b/\",\"url_resolved\":\"\",\"codec\":\"UNKNOWN\",\"bitrate\":0,\"lastcheckok\":0},"
        "{\"name\":\"NoResolved\",\"url\":\"https://c/s\",\"url_resolved\":\"\",\"codec\":\"AAC+\",\"bitrate\":\"64\",\"lastcheckok\":1},"
        "{\"name\":\"\",\"url\":\"http://d/\"},"
        "{\"name\":\"Bad url\",\"url\":\"file:///etc/passwd\"},"
        "{\"name\":\"Emoji \\ud83d\\ude00\",\"url_resolved\":\"http://e/\"},"
        "\"not an object\","
        "{\"name\":\"Dup\",\"url_resolved\":\"http://a/stream\"}]";
    rc_station_t s[RC_MAX_HITS];
    int n = rc_parse_stations(js, strlen(js), s, RC_MAX_HITS);
    CHECK(n == 3);
    CHECK(!strcmp(s[0].name, "Radio \xC3\xA4 One") && !strcmp(s[0].url, "http://a/stream") && s[0].bitrate == 128 && !strcmp(s[0].country, "Germany"));
    CHECK(!strcmp(s[1].name, "NoResolved") && !strcmp(s[1].url, "https://c/s") && s[1].bitrate == 64 && !strcmp(s[1].codec, "AAC+"));
    CHECK(!strcmp(s[2].name, "Emoji \xF0\x9F\x98\x80"));
    CHECK(rc_parse_stations("{\"a\":1}", 7, s, 4) == -1);
    CHECK(rc_parse_stations("[1,2", 4, s, 4) == -1);
    CHECK(rc_parse_stations("[]", 2, s, 4) == 0);
    CHECK(rc_parse_stations(js, strlen(js), s, 1) == 1);
}
static void test_upnp(void){
    char host[64], path[256], out[2048]; int port;
    CHECK(rc_split_http("http://192.168.1.5:49494/desc.xml", host, sizeof host, &port, path, sizeof path) == 0);
    CHECK(!strcmp(host, "192.168.1.5") && port == 49494 && !strcmp(path, "/desc.xml"));
    CHECK(rc_split_http("http://h", host, sizeof host, &port, path, sizeof path) == 0 && port == 80 && !strcmp(path, "/"));
    CHECK(rc_split_http("https://h/", host, sizeof host, &port, path, sizeof path) == -1);
    CHECK(rc_split_http("http://h:99999/", host, sizeof host, &port, path, sizeof path) == -1);
    const char *desc = "<root><device><friendlyName>SNOWSKY DISC</friendlyName><serviceList>"
        "<service><serviceType>urn:schemas-upnp-org:service:RenderingControl:1</serviceType><controlURL>/rc</controlURL></service>"
        "<service><serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL> AVTransport/ctl </controlURL></service>"
        "</serviceList></device></root>";
    CHECK(rc_desc_control_url(desc, "http://10.0.0.2:49494/description.xml", out, sizeof out) == 0);
    CHECK(!strcmp(out, "http://10.0.0.2:49494/AVTransport/ctl"));
    CHECK(rc_desc_control_url("<root>no</root>", "http://x/", out, sizeof out) == -1);
    const char *abs = "<serviceType>urn:schemas-upnp-org:service:AVTransport:1</serviceType><controlURL>http://1.2.3.4:5/x</controlURL>";
    CHECK(rc_desc_control_url(abs, "http://x/", out, sizeof out) == 0 && !strcmp(out, "http://1.2.3.4:5/x"));
    char v[64];
    CHECK(rc_xml_value("<a><CurrentTransportState>PLAYING</CurrentTransportState></a>", "CurrentTransportState", v, sizeof v) == 0 && !strcmp(v, "PLAYING"));
    CHECK(rc_xml_value("<x>a &amp; b</x>", "x", v, sizeof v) == 0 && !strcmp(v, "a & b"));
    CHECK(rc_xml_value("<x>a", "x", v, sizeof v) == -1);
    rc_station_t s = {0}; strcpy(s.name, "Rock & <Roll>"); strcpy(s.url, "http://x/s?a=1&b=2"); strcpy(s.codec, "MP3"); s.bitrate = 128;
    static char args[16384];
    CHECK(rc_set_uri_args(&s, args, sizeof args) == 0);
    CHECK(strstr(args, "<CurrentURI>http://x/s?a=1&amp;b=2</CurrentURI>") != NULL);
    CHECK(strstr(args, "&lt;dc:title&gt;Rock &amp;amp; &amp;lt;Roll&amp;gt;&lt;/dc:title&gt;") != NULL);   /* escaped twice */
    CHECK(strstr(args, "http-get:*:audio/mpeg:*") != NULL);
    CHECK(strstr(args, "<DIDL") == NULL);                                     /* metadata never raw */
    CHECK(rc_set_uri_args(&s, args, 64) == -1);
    static char body[20000];
    CHECK(rc_soap_body("Play", "<Speed>1</Speed>", body, sizeof body) == 0);
    CHECK(strstr(body, "<u:Play xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\"><InstanceID>0</InstanceID><Speed>1</Speed></u:Play>") != NULL);
    CHECK(rc_soap_body("Play", "", body, 50) == -1);
}
static void test_icy(void){
    const char *h = "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: Vintage\r\nICY-MetaInt: 16000\r\n\r\nAUDIO";
    CHECK(rc_icy_head_end(h, strlen(h)) == (int)(strstr(h, "AUDIO") - h));
    CHECK(rc_icy_head_end(h, 20) == -1);
    CHECK(rc_icy_metaint(h, strlen(h)) == 16000);
    const char *s1 = "ICY 200 OK\nicy-metaint:8192\n\n";
    CHECK(rc_icy_head_end(s1, strlen(s1)) == (int)strlen(s1) && rc_icy_metaint(s1, strlen(s1)) == 8192);
    CHECK(rc_icy_metaint("HTTP/1.0 200 OK\r\nX-icy-metaint: 5\r\n", 36) == -1);   /* only at a line start */
    CHECK(rc_icy_metaint("icy-metaint: 0\r\n", 17) == -1);
    CHECK(rc_icy_metaint("icy-metaint: 99999999\r\n", 24) == -1);
    char t[160], a[80], ti[80];
    char blk[96] = "StreamTitle='Rude Norton - Tits On The Beach [Canada ,(Joke)Punk] (1980)';StreamUrl='';";
    CHECK(rc_icy_title(blk, sizeof blk, t, sizeof t) == 0 && !strcmp(t, "Rude Norton - Tits On The Beach [Canada ,(Joke)Punk] (1980)"));
    CHECK(rc_icy_split(t, a, sizeof a, ti, sizeof ti) == 1 && !strcmp(a, "Rude Norton") && !strcmp(ti, "Tits On The Beach [Canada ,(Joke)Punk] (1980)"));
    CHECK(rc_icy_title("StreamTitle='Guns N' Roses - Don't Cry';", 41, t, sizeof t) == 0 && !strcmp(t, "Guns N' Roses - Don't Cry"));
    CHECK(rc_icy_title("StreamTitle='';", 15, t, sizeof t) == 0 && t[0] == 0);
    CHECK(rc_icy_title("StreamUrl='x';", 14, t, sizeof t) == -1);
    CHECK(rc_icy_title("StreamTitle='Caf\xE9  \x01 Tune';", 30, t, sizeof t) == 0 && !strcmp(t, "Caf\xC3\xA9   Tune"));   /* Latin-1 */
    CHECK(rc_icy_title("StreamTitle='Caf\xC3\xA9';", 20, t, sizeof t) == 0 && !strcmp(t, "Caf\xC3\xA9"));                /* UTF-8 kept */
    CHECK(rc_icy_title("StreamTitle='ab\xC3\xA9';", 19, t, 4) == 0 && !strcmp(t, "ab"));                                   /* no cut sequence */
    CHECK(rc_icy_title("StreamTitle='  No end", 21, t, sizeof t) == 0 && !strcmp(t, "No end"));
    CHECK(rc_icy_split("Just a show", a, sizeof a, ti, sizeof ti) == 0 && a[0] == 0 && !strcmp(ti, "Just a show"));
    CHECK(rc_icy_split(" - x", a, sizeof a, ti, sizeof ti) == 0);
}
static size_t ogg_page(unsigned char *o, unsigned serial, const unsigned char *d, size_t n){   /* one page, n <= 255*10 */
    size_t segs = n / 255 + 1, k = 0;
    memcpy(o, "OggS", 4); memset(o + 4, 0, 22);
    o[14] = serial & 255; o[15] = serial >> 8 & 255; o[16] = serial >> 16 & 255; o[17] = serial >> 24;
    o[26] = (unsigned char)segs;
    for(size_t i = 0; i < segs; i++) o[27 + i] = i + 1 < segs ? 255 : (unsigned char)(n % 255);
    k = 27 + segs; memcpy(o + k, d, n);
    return k + n;
}
static size_t put32(unsigned char *o, const char *s){ size_t l = strlen(s); o[0] = l & 255; o[1] = l >> 8 & 255; o[2] = 0; o[3] = 0; memcpy(o + 4, s, l); return 4 + l; }
static void test_ogg(void){
    unsigned char pk[600], st[4000], pay[4000]; size_t n = 0, k = 0;
    memcpy(pk, "\x03vorbis", 7); n = 7;
    n += put32(pk + n, "Xiph.Org libVorbis I 20200704");
    pk[n++] = 3; pk[n++] = 0; pk[n++] = 0; pk[n++] = 0;
    n += put32(pk + n, "artist=Kraftwerk");
    n += put32(pk + n, "GENRE=Electronic");
    n += put32(pk + n, "Title=Das Model");
    k += ogg_page(st + k, 7, (const unsigned char *)"\x01vorbis-ident", 13);
    k += ogg_page(st + k, 7, pk, 30);                          /* the comment packet spans two pages ... */
    k += ogg_page(st + k, 9, (const unsigned char *)"\x03vorbis-other-stream", 20);   /* ... with another stream between */
    k += ogg_page(st + k, 7, pk + 30, n - 30);
    size_t pl = rc_ogg_payload(st, k, pay, sizeof pay);
    CHECK(pl == 13 + n);
    char a[80], t[80];
    CHECK(rc_ogg_tags(pay, pl, a, sizeof a, t, sizeof t) == 0 && !strcmp(a, "Kraftwerk") && !strcmp(t, "Das Model"));
    CHECK(rc_ogg_payload(st + 3, k - 3, pay, sizeof pay) == 30 + n - 30);        /* cut first page: resync at the next */
    CHECK(rc_ogg_tags(pay, rc_ogg_payload(st, k - 10, pay, sizeof pay), a, sizeof a, t, sizeof t) == 0 && !strcmp(a, "Kraftwerk") && t[0] == 0);  /* cut title */
    n = 0; memcpy(pk, "OpusTags", 8); n = 8; n += put32(pk + n, "libopus 1.3"); pk[n++] = 1; pk[n++] = 0; pk[n++] = 0; pk[n++] = 0;
    n += put32(pk + n, "TITLE=Clair de lune");
    k = ogg_page(st, 3, (const unsigned char *)"OpusHead", 8); k += ogg_page(st + k, 3, pk, n);
    pl = rc_ogg_payload(st, k, pay, sizeof pay);
    CHECK(rc_ogg_tags(pay, pl, a, sizeof a, t, sizeof t) == 0 && a[0] == 0 && !strcmp(t, "Clair de lune"));
    CHECK(rc_ogg_tags((const unsigned char *)"ID3 junk", 8, a, sizeof a, t, sizeof t) == -1);
    unsigned char bad[] = "OpusTags\xff\xff\xff\x7f";
    CHECK(rc_ogg_tags(bad, sizeof bad - 1, a, sizeof a, t, sizeof t) == -1);
}
int main(void){
    test_url(); test_encode(); test_detail(); test_fav(); test_json(); test_upnp(); test_icy(); test_ogg();
    if(fails){ fprintf(stderr, "%d radio core check(s) failed\n", fails); return 1; }
    puts("PASS radio core");
    return 0;
}
