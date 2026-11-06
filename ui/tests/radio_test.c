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
int main(void){
    test_url(); test_encode(); test_detail(); test_fav(); test_json(); test_upnp();
    if(fails){ fprintf(stderr, "%d radio core check(s) failed\n", fails); return 1; }
    puts("PASS radio core");
    return 0;
}
