#include "api.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------
static NaviConfig g_cfg;
static char g_base_url[512];

// Subsonic API version and client name
#define API_VERSION "1.16.1"
#define API_CLIENT  "Navidrome3DS"

// ---------------------------------------------------------------------------
// HTTP response buffer
// ---------------------------------------------------------------------------
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} Buffer;

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    Buffer *buf = (Buffer *)userdata;
    size_t incoming = size * nmemb;

    if (buf->len + incoming + 1 > buf->cap) {
        buf->cap = (buf->len + incoming + 1) * 2;
        buf->data = realloc(buf->data, buf->cap);
        if (!buf->data) return 0;
    }
    memcpy(buf->data + buf->len, ptr, incoming);
    buf->len += incoming;
    buf->data[buf->len] = '\0';
    return incoming;
}

static Buffer buf_new(void) {
    Buffer b;
    b.cap  = 4096;
    b.len  = 0;
    b.data = malloc(b.cap);
    if (b.data) b.data[0] = '\0';
    return b;
}

static void buf_free(Buffer *b) {
    if (b->data) free(b->data);
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

// ---------------------------------------------------------------------------
// Perform a GET request, return HTTP status code (0 on curl error)
// ---------------------------------------------------------------------------
static int http_get(const char *url, Buffer *out) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    // Accept self-signed certs on local network (set to 1L for strict)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_easy_cleanup(curl);
    return (int)http_code;
}

// ---------------------------------------------------------------------------
// Build a Subsonic API URL
// ---------------------------------------------------------------------------
static void build_url(char *out, size_t len, const char *endpoint,
                      const char *extra_params) {
    snprintf(out, len,
        "%s/rest/%s?u=%s&p=%s&v=" API_VERSION "&c=" API_CLIENT "&f=xml%s",
        g_base_url,
        endpoint,
        g_cfg.username,
        g_cfg.password,
        extra_params ? extra_params : "");
}

// ---------------------------------------------------------------------------
// Minimal XML attribute extractor
// Finds the value of `attr="..."` within `tag` occurrences in xml.
// Writes into dst (up to dst_len bytes). Returns pointer after match or NULL.
// ---------------------------------------------------------------------------
static const char *xml_attr(const char *xml, const char *attr, char *dst, size_t dst_len) {
    char needle[MAX_NAME_LEN + 2];
    snprintf(needle, sizeof(needle), "%s=\"", attr);
    const char *p = strstr(xml, needle);
    if (!p) return NULL;
    p += strlen(needle);

    size_t i = 0;
    // Copy bytes verbatim — handles UTF-8 multibyte sequences correctly
    // Only stop at closing quote or end of string
    while (*p && *p != '"' && i < dst_len - 1) {
        // Handle XML entities
        if (*p == '&') {
            if      (strncmp(p, "&amp;",  5) == 0) { dst[i++] = '&';  p += 5; }
            else if (strncmp(p, "&lt;",   4) == 0) { dst[i++] = '<';  p += 4; }
            else if (strncmp(p, "&gt;",   4) == 0) { dst[i++] = '>';  p += 4; }
            else if (strncmp(p, "&apos;", 6) == 0) { dst[i++] = '\''; p += 6; }
            else if (strncmp(p, "&quot;", 6) == 0) { dst[i++] = '"';  p += 6; }
            else if (strncmp(p, "&#",     2) == 0) {
                // Numeric entity: &#dd; or &#xhh;
                p += 2;
                unsigned int codepoint = 0;
                if (*p == 'x' || *p == 'X') {
                    p++;
                    while (*p && *p != ';') {
                        codepoint *= 16;
                        if      (*p >= '0' && *p <= '9') codepoint += *p - '0';
                        else if (*p >= 'a' && *p <= 'f') codepoint += *p - 'a' + 10;
                        else if (*p >= 'A' && *p <= 'F') codepoint += *p - 'A' + 10;
                        p++;
                    }
                } else {
                    while (*p && *p != ';') { codepoint = codepoint * 10 + (*p - '0'); p++; }
                }
                if (*p == ';') p++;
                // Encode codepoint as UTF-8
                if (codepoint < 0x80 && i < dst_len - 1) {
                    dst[i++] = (char)codepoint;
                } else if (codepoint < 0x800 && i < dst_len - 2) {
                    dst[i++] = (char)(0xC0 | (codepoint >> 6));
                    dst[i++] = (char)(0x80 | (codepoint & 0x3F));
                } else if (codepoint < 0x10000 && i < dst_len - 3) {
                    dst[i++] = (char)(0xE0 | (codepoint >> 12));
                    dst[i++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                    dst[i++] = (char)(0x80 | (codepoint & 0x3F));
                }
            }
            else { dst[i++] = *p++; }
        } else {
            dst[i++] = *p++;
        }
    }
    dst[i] = '\0';
    return (*p == '"') ? p + 1 : NULL;
}

// Advance past the next occurrence of `tag` in xml, return pointer into xml
static const char *xml_next_tag(const char *xml, const char *tag) {
    return strstr(xml, tag);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void api_init(const NaviConfig *cfg) {
    g_cfg = *cfg;
    snprintf(g_base_url, sizeof(g_base_url),
             "http://%s:%d", cfg->host, cfg->port);
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void api_cleanup(void) {
    curl_global_cleanup();
}

int api_ping(void) {
    char url[2048];
    build_url(url, sizeof(url), "ping", NULL);

    Buffer buf = buf_new();
    int code = http_get(url, &buf);
    int ok = (code == 200 && buf.data && strstr(buf.data, "status=\"ok\""));
    buf_free(&buf);
    return ok ? 0 : -1;
}

int api_get_artists(NaviArtistList *out) {
    out->count = 0;
    char url[2048];
    build_url(url, sizeof(url), "getArtists", NULL);

    Buffer buf = buf_new();
    if (http_get(url, &buf) != 200 || !buf.data) {
        buf_free(&buf);
        return -1;
    }

    // Parse <artist id="..." name="..." .../>
    const char *p = buf.data;
    while (out->count < MAX_ITEMS) {
        p = xml_next_tag(p, "<artist ");
        if (!p) break;

        NaviArtist *a = &out->items[out->count];
        const char *after = xml_attr(p, "id",   a->id,   MAX_ID_LEN);
        if (after) xml_attr(p, "name", a->name, MAX_NAME_LEN);

        if (a->id[0] && a->name[0]) out->count++;
        p++; // advance so we don't match same tag
    }

    buf_free(&buf);
    return 0;
}

int api_get_albums(const char *artist_id, NaviAlbumList *out) {
    out->count = 0;
    char extra[128], url[2048];
    snprintf(extra, sizeof(extra), "&id=%s", artist_id);
    build_url(url, sizeof(url), "getArtist", extra);

    Buffer buf = buf_new();
    if (http_get(url, &buf) != 200 || !buf.data) {
        buf_free(&buf);
        return -1;
    }

    const char *p = buf.data;
    while (out->count < MAX_ITEMS) {
        p = xml_next_tag(p, "<album ");
        if (!p) break;

        NaviAlbum *al = &out->items[out->count];
        xml_attr(p, "id",        al->id,     MAX_ID_LEN);
        xml_attr(p, "name",      al->name,   MAX_NAME_LEN);
        xml_attr(p, "artist",    al->artist, MAX_NAME_LEN);
        char tmp[16] = {0};
        xml_attr(p, "year",      tmp, sizeof(tmp));
        al->year = tmp[0] ? atoi(tmp) : 0;
        tmp[0] = 0;
        xml_attr(p, "songCount", tmp, sizeof(tmp));
        al->songCount = tmp[0] ? atoi(tmp) : 0;

        if (al->id[0]) out->count++;
        p++;
    }

    buf_free(&buf);
    return 0;
}

int api_get_tracks(const char *album_id, NaviTrackList *out) {
    out->count = 0;
    char extra[128], url[2048];
    snprintf(extra, sizeof(extra), "&id=%s", album_id);
    build_url(url, sizeof(url), "getAlbum", extra);

    Buffer buf = buf_new();
    if (http_get(url, &buf) != 200 || !buf.data) {
        buf_free(&buf);
        return -1;
    }

    const char *p = buf.data;
    while (out->count < MAX_ITEMS) {
        p = xml_next_tag(p, "<song ");
        if (!p) break;

        NaviTrack *t = &out->items[out->count];
        xml_attr(p, "id",       t->id,     MAX_ID_LEN);
        xml_attr(p, "title",    t->title,  MAX_NAME_LEN);
        xml_attr(p, "artist",   t->artist, MAX_NAME_LEN);
        xml_attr(p, "album",    t->album,  MAX_NAME_LEN);
        char tmp[16] = {0};
        xml_attr(p, "duration", tmp, sizeof(tmp));
        t->duration = tmp[0] ? atoi(tmp) : 0;
        tmp[0] = 0;
        xml_attr(p, "track",    tmp, sizeof(tmp));
        t->track = tmp[0] ? atoi(tmp) : 0;

        if (t->id[0]) out->count++;
        p++;
    }

    buf_free(&buf);
    return 0;
}

void api_stream_url(const char *track_id, char *buf, size_t len) {
    char extra[128];
    snprintf(extra, sizeof(extra), "&id=%s&format=mp3&maxBitRate=128", track_id);
    build_url(buf, len, "stream", extra);
}
