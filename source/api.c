#include "api.h"
#include "debug.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <curl/curl.h>
#include <citro2d.h>
#include <citro3d.h>
#include <3ds.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

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
    if (b.data) {
        b.data[0] = '\0';
        debug_log("[API] Buffer allocated: capacity=%zu", b.cap);
    } else {
        debug_log("[API] Buffer allocation failed");
    }
    return b;
}

static void buf_free(Buffer *b) {
    if (b->data) {
        free(b->data);
        debug_log("[API] Buffer freed: capacity=%zu, length=%zu", b->cap, b->len);
    }
    b->data = NULL;
    b->len  = 0;
    b->cap  = 0;
}

// ---------------------------------------------------------------------------
// Perform a GET request, return HTTP status code (0 on curl error)
// ---------------------------------------------------------------------------
static int http_get(const char *url, Buffer *out) {
    debug_log("[API] http_get called: url=%s", url);
    CURL *curl = curl_easy_init();
    if (!curl) {
        debug_log("[API] curl_easy_init failed");
        return 0;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    // Accept self-signed certs on local network (set to 1L for strict)
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);

    debug_log("[API] Performing curl_easy_perform...");
    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        debug_log("[API] HTTP response code: %ld", http_code);
    } else {
        debug_log("[API] curl_easy_perform error: %d", res);
    }

    curl_easy_cleanup(curl);
    debug_log("[API] http_get finished: url=%s, code=%ld", url, http_code);
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
    debug_log("[API] Built URL for endpoint '%s': %s", endpoint, out);
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
// Read 16-bit big-endian from buffer
static u16 read_be16(const u8 *p) { return (u16)p[0] << 8 | p[1]; }
static u32 read_be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u16 read_le16(const u8 *p) { return (u16)p[1] << 8 | p[0]; }
static u32 read_le32(const u8 *p) { return (u32)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }

// Parse EXIF orientation from JPEG buffer. Returns 1-8, or 0 if not found/valid.
static int parse_exif_orientation(const u8 *data, u32 size) {
    for (u32 i = 0; i + 4 < size; i++) {
        if (data[i] != 0xFF) continue;
        u8 marker = data[i+1];
        if (marker == 0xD9 || marker == 0xDA) break; // SOS / EOI
        if (marker != 0xE1) {
            if (marker == 0x00) continue;
            u16 seg_len = read_be16(&data[i+2]);
            i += seg_len;
            continue;
        }
        // APP1 segment: FF E1 len "Exif\0\0" TIFF...
        if (i + 14 > size) break;
        u16 seg_len = read_be16(&data[i+2]);
        if (seg_len < 14 || i + 2 + seg_len > size) break;
        if (memcmp(&data[i+4], "Exif\0\0", 6) != 0) { i += seg_len; continue; }
        u32 tiff_off = i + 10;
        u32 tiff_end = i + 2 + seg_len;
        if (tiff_off + 8 > tiff_end) break;
        int little = (data[tiff_off] == 'I');
        if (data[tiff_off+1] != (little ? 'I' : 'M')) break;
        u16 magic = little ? read_le16(&data[tiff_off+2]) : read_be16(&data[tiff_off+2]);
        if (magic != 0x002A) break;
        u32 ifd0_off = little ? read_le32(&data[tiff_off+4]) : read_be32(&data[tiff_off+4]);
        if (tiff_off + ifd0_off + 2 > tiff_end) break;
        u32 pos = tiff_off + ifd0_off;
        u16 count = little ? read_le16(&data[pos]) : read_be16(&data[pos]);
        pos += 2;
        for (u16 j = 0; j < count && pos + 12 <= tiff_end; j++, pos += 12) {
            u16 tag = little ? read_le16(&data[pos]) : read_be16(&data[pos]);
            if (tag == 0x0112) {
                u16 type = little ? read_le16(&data[pos+2]) : read_be16(&data[pos+2]);
                if (type != 3) break; // SHORT
                u16 orient = little ? read_le16(&data[pos+8]) : read_be16(&data[pos+8]);
                if (orient >= 1 && orient <= 8) return orient;
            }
        }
        break;
    }
    return 0;
}

static void apply_orientation(unsigned char **pixels, int *w, int *h, int orient) {
    if (orient <= 1) return;
    int ow = *w, oh = *h;
    unsigned char *src = *pixels;
    // For 90/270 rotations, output dims are swapped
    int nw = (orient >= 5 && orient <= 8) ? oh : ow;
    int nh = (orient >= 5 && orient <= 8) ? ow : oh;
    unsigned char *dst = (unsigned char*)malloc(nw * nh * 4);
    if (!dst) return;
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int sx, sy;
            switch (orient) {
                case 2: sx = ow - 1 - x; sy = y; break;
                case 3: sx = ow - 1 - x; sy = oh - 1 - y; break;
                case 4: sx = x; sy = oh - 1 - y; break;
                case 5: sx = y; sy = x; break;
                case 6: sx = oh - 1 - y; sy = x; break;
                case 7: sx = oh - 1 - y; sy = ow - 1 - x; break;
                case 8: sx = y; sy = ow - 1 - x; break;
                default: sx = x; sy = y; break;
            }
            memcpy(&dst[sy * nw * 4 + sx * 4], &src[y * ow * 4 + x * 4], 4);
        }
    }
    free(src);
    *pixels = dst;
    *w = nw;
    *h = nh;
}

// 3DS texture swizzle: convert linear RGBA8 → tiled GPU format
// ---------------------------------------------------------------------------
// Z-order (Morton) interleave: compute in-tile offset for texel (px, py) in an 8x8 tile
static int z_order(int px, int py) {
    return ((px & 1) << 0) | ((py & 1) << 1) |
           ((px & 2) << 1) | ((py & 2) << 2) |
           ((px & 4) << 2) | ((py & 4) << 3);
}

// Convert RGBA8 to RGB565 and swizzle into GPU tiled format
static void swizzle_rgb565(u16 *out, const u32 *in, int w, int h, int pot_w, int pot_h) {
    int tiles_per_row = pot_w / 8;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 pixel = in[y * w + x];
            u8 r = (pixel >> 0) & 0xFF;
            u8 g = (pixel >> 8) & 0xFF;
            u8 b = (pixel >> 16) & 0xFF;
            u16 rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            int tile_x = x / 8;
            int tile_y = y / 8;
            int tile_idx = tile_y * tiles_per_row + tile_x;
            int dst = tile_idx * 64 + z_order(x % 8, y % 8);
            out[dst] = rgb565;
        }
    }
}

// ---------------------------------------------------------------------------
// Fetch and decode album cover using stb_image, create a C2D_Image
// ---------------------------------------------------------------------------
int api_get_album_cover(const char *album_id, AlbumCoverResult *out) {
    char url[2048];
    snprintf(url, sizeof(url), "%s/rest/getCoverArt?u=%s&p=%s&v=1.16.1&c=Navidrome3DS&id=%s",
            g_base_url, g_cfg.username, g_cfg.password, album_id);

    debug_log("[API] Fetching album cover: %s", url);

    Buffer buf = buf_new();
    int http_code = http_get(url, &buf);
    if (http_code != 200 || !buf.data || buf.len == 0) {
        debug_log("[API] Failed to fetch album cover, HTTP code: %d", http_code);
        buf_free(&buf);
        return -1;
    }

    // Decode image to RGBA8 using stb_image
    int w, h, channels;

    // Parse EXIF orientation from raw JPEG before freeing buffer
    int orient = parse_exif_orientation((u8*)buf.data, buf.len);

    unsigned char *pixels = stbi_load_from_memory((unsigned char*)buf.data, buf.len, &w, &h, &channels, 4);
    buf_free(&buf);

    if (!pixels) {
        debug_log("[API] stb_image failed to decode cover image");
        return -1;
    }
    debug_log("[API] Decoded cover: %dx%d (%d channels)", w, h, channels);

    // Apply EXIF orientation transform if needed
    if (orient > 1) {
        debug_log("[API] Applying EXIF orientation: %d", orient);
        apply_orientation(&pixels, &w, &h, orient);
        debug_log("[API] After orientation: %dx%d", w, h);
    }

    // 3DS PICA200 GPU max texture size is 1024. Downscale if needed.
    int max_tex = 1024;
    if (w > max_tex || h > max_tex) {
        float scale = (w > h) ? (float)max_tex / w : (float)max_tex / h;
        int nw = (int)(w * scale);
        int nh = (int)(h * scale);
        unsigned char *resized = (unsigned char*)malloc(nw * nh * 4);
        if (resized) {
            for (int wy = 0; wy < nh; wy++) {
                float sy = (float)wy / nh * h;
                int sy0 = (int)sy;
                int sy1 = (sy0 + 1 < h) ? sy0 + 1 : sy0;
                float fy = sy - sy0;
                for (int wx = 0; wx < nw; wx++) {
                    float sx = (float)wx / nw * w;
                    int sx0 = (int)sx;
                    int sx1 = (sx0 + 1 < w) ? sx0 + 1 : sx0;
                    float fx = sx - sx0;
                    for (int c = 0; c < 4; c++) {
                        float v = (1-fy)*(1-fx)*pixels[sy0*w*4 + sx0*4 + c]
                                + (1-fy)*fx   *pixels[sy0*w*4 + sx1*4 + c]
                                + fy   *(1-fx)*pixels[sy1*w*4 + sx0*4 + c]
                                + fy   *fx   *pixels[sy1*w*4 + sx1*4 + c];
                        resized[wy*nw*4 + wx*4 + c] = (unsigned char)(v + 0.5f);
                    }
                }
            }
            stbi_image_free(pixels);
            pixels = resized;
            w = nw;
            h = nh;
            debug_log("[API] Downscaled cover to: %dx%d", w, h);
        }
    }

    // Power-of-two texture dimensions (at most 1024)
    int pot_w = 1; while (pot_w < w) pot_w <<= 1;
    int pot_h = 1; while (pot_h < h) pot_h <<= 1;

    // Allocate C3D texture
    C3D_Tex *tex = (C3D_Tex*)calloc(1, sizeof(C3D_Tex));
    if (!tex) { stbi_image_free(pixels); return -1; }
    C3D_TexInit(tex, pot_w, pot_h, GPU_RGB565);
    if (!tex->data) {
        debug_log("[API] C3D_TexInit failed for POT: %dx%d", pot_w, pot_h);
        free(tex); stbi_image_free(pixels); return -1;
    }
    C3D_TexSetFilter(tex, GPU_LINEAR, GPU_LINEAR);

    // Convert RGBA8→RGB565 and swizzle into GPU tiled format
    swizzle_rgb565((u16*)tex->data, (u32*)pixels, w, h, pot_w, pot_h);
    stbi_image_free(pixels);

    // Flush data cache so GPU sees the pixels
    GSPGPU_FlushDataCache(tex->data, pot_w * pot_h * 2);

    // Subtexture covering the actual image area within the POT texture
    Tex3DS_SubTexture *subtex = (Tex3DS_SubTexture*)malloc(sizeof(Tex3DS_SubTexture));
    if (!subtex) { linearFree(tex->data); free(tex); return -1; }
    subtex->width  = w;
    subtex->height = h;
    subtex->left   = 0.0f;
    subtex->top    = (float)h / pot_h;
    subtex->right  = (float)w / pot_w;
    subtex->bottom = 0.0f;

    out->image.tex    = tex;
    out->image.subtex = subtex;
    out->tex          = tex;
    out->subtex       = subtex;

    debug_log("[API] Successfully loaded album cover: %dx%d (POT: %dx%d)", w, h, pot_w, pot_h);
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void api_init(const NaviConfig *cfg) {
    debug_log("[ENTER] api_init()");
    debug_log("[API] Initializing with host=%s, port=%d, user=%s", cfg->host, cfg->port, cfg->username);
    g_cfg = *cfg;
    snprintf(g_base_url, sizeof(g_base_url),
             "http://%s:%d", cfg->host, cfg->port);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    debug_log("[API] curl_global_init complete");
}

void api_cleanup(void) {
    debug_log("[API] Cleaning up curl global state");
    curl_global_cleanup();
    debug_log("[API] Cleanup complete");
}

int api_ping(void) {
    debug_log("[API] api_ping called");
    char url[2048];
    build_url(url, sizeof(url), "ping", NULL);
    debug_log("[API] Ping URL: %s", url);

    Buffer buf = buf_new();
    debug_log("[API] Ping buf.data=%p buf.len=%zu", buf.data, buf.len);
    int code = http_get(url, &buf);
    debug_log("[API] Ping HTTP code: %d", code);
    debug_log("[API] Ping buf.data=%p buf.len=%zu", buf.data, buf.len);

    // Hex dump the response body for exact inspection
    if (buf.data && buf.len > 0) {
        char hexdump[513] = {0};
        size_t dump_len = buf.len > 256 ? 256 : buf.len;
        for (size_t i = 0; i < dump_len; i++) {
            snprintf(hexdump + strlen(hexdump), sizeof(hexdump) - strlen(hexdump), "%02X ", (unsigned char)buf.data[i]);
        }
        debug_log("[API] Ping body hex (%zu bytes): %s", buf.len, hexdump);
        debug_log("[API] Ping body text: %s", buf.data);
    } else {
        debug_log("[API] Ping body: (null or zero length)");
    }

    const char *needle = "status=\"ok\"";
    char *found = buf.data ? strstr(buf.data, needle) : NULL;
    debug_log("[API] Ping strstr(\"status=\\\"ok\\\"\") = %p", (void*)found);

    int ok = (code == 200 && buf.data && buf.len > 0 && found);
    debug_log("[API] Ping ok=%d code=%d data=%p len=%zu", ok, code, (void*)buf.data, buf.len);
    buf_free(&buf);
    debug_log("[API] api_ping returning %d", ok ? 0 : -1);
    return ok ? 0 : -1;
}

int api_get_artists(NaviArtistList *out) {
    debug_log("[API] api_get_artists called");
    out->count = 0;
    char url[2048];
    build_url(url, sizeof(url), "getArtists", NULL);
    debug_log("[API] getArtists URL: %s", url);

    Buffer buf = buf_new();
    int http_code = http_get(url, &buf);
    debug_log("[API] getArtists HTTP code: %d", http_code);
    if (http_code != 200 || !buf.data) {
        debug_log("[API] getArtists failed, buf.data=%p", buf.data);
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
        debug_log("[API] Parsed artist: id=%s, name=%s", a->id, a->name);

        if (a->id[0] && a->name[0]) out->count++;
        p++; // advance so we don't match same tag
    }

    debug_log("[API] api_get_artists loaded %d artists", out->count);
    buf_free(&buf);
    return 0;
}

int api_get_albums(const char *artist_id, NaviAlbumList *out) {
    debug_log("[API] api_get_albums called for artist_id=%s", artist_id);
    out->count = 0;
    char extra[128], url[2048];
    snprintf(extra, sizeof(extra), "&id=%s", artist_id);
    build_url(url, sizeof(url), "getArtist", extra);

    Buffer buf = buf_new();
    int http_code = http_get(url, &buf);
    if (http_code != 200 || !buf.data) {
        debug_log("[API] api_get_albums failed, HTTP code: %d", http_code);
        buf_free(&buf);
        return -1;
    }

    debug_log("[API] Parsing album data for artist_id=%s", artist_id);
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

        debug_log("[API] Parsed album: id=%s, name=%s, artist=%s, year=%d, songCount=%d", 
                 al->id, al->name, al->artist, al->year, al->songCount);

        if (al->id[0]) out->count++;
        p++;
    }

    debug_log("[API] api_get_albums loaded %d albums for artist_id=%s", out->count, artist_id);
    buf_free(&buf);
    return 0;
}

int api_get_tracks(const char *album_id, NaviTrackList *out) {
    debug_log("[API] api_get_tracks called for album_id=%s", album_id);
    out->count = 0;
    char extra[128], url[2048];
    snprintf(extra, sizeof(extra), "&id=%s", album_id);
    build_url(url, sizeof(url), "getAlbum", extra);

    Buffer buf = buf_new();
    int http_code = http_get(url, &buf);
    if (http_code != 200 || !buf.data) {
        debug_log("[API] api_get_tracks failed, HTTP code: %d", http_code);
        buf_free(&buf);
        return -1;
    }

    debug_log("[API] Parsing track data for album_id=%s", album_id);
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

        debug_log("[API] Parsed track: id=%s, title=%s, artist=%s, album=%s, duration=%d, track=%d", 
                 t->id, t->title, t->artist, t->album, t->duration, t->track);

        if (t->id[0]) out->count++;
        p++;
    }

    debug_log("[API] api_get_tracks loaded %d tracks for album_id=%s", out->count, album_id);
    buf_free(&buf);
    return 0;
}

void api_stream_url(const char *track_id, char *buf, size_t len) {
    debug_log("[API] api_stream_url called for track_id=%s", track_id);
    char extra[128];
    snprintf(extra, sizeof(extra), "&id=%s&format=mp3&maxBitRate=128", track_id);
    build_url(buf, len, "stream", extra);
    debug_log("[API] Generated stream URL: %s", buf);
}
