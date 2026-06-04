#include "api.h"
#include "debug.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <curl/curl.h>
#include <citro2d.h>
#include <citro3d.h>
#include <tex3ds.h>
#include <3ds.h>

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

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// Background thread data for album cover loading
typedef struct {
    UiState *state;
    char album_id[MAX_ID_LEN];
    LightEvent *event;
} AlbumCoverThreadData;

// Convert linear coordinates to 3DS hardware morton tiled index (Z-Order 8x8 tiling)
static uint32_t tile_index(int x, int y, int w) {
    // 3DS textures are made of 8x8 blocks.
    // Inside each 8x8 block, pixels are ordered in Z-curve.
    // Elements are grouped into 2x2, then 4x4, then 8x8.
    int block_x = x / 8;
    int block_y = y / 8;
    int blocks_per_row = w / 8;
    int block_idx = (block_y * blocks_per_row + block_x) * 64;

    int cx = x % 8;
    int cy = y % 8;

    // Morton Z-order calculation for 8x8 tile
    int offset = 0;
    offset += (cx & 1) << 0;
    offset += (cy & 1) << 1;
    offset += (cx & 2) << 1;
    offset += (cy & 2) << 2;
    offset += (cx & 4) << 2;
    offset += (cy & 4) << 3;

    return block_idx + offset;
}

// Background thread function for loading album covers
static void album_cover_thread_func(void *arg) {
    AlbumCoverThreadData *data = (AlbumCoverThreadData *)arg;
    UiState *state = data->state;
    const char *album_id = data->album_id;
    LightEvent *event = data->event;

    debug_log("[API] Background thread started for album: %s", album_id);

    char url[2048];
    snprintf(url, sizeof(url), "%s/rest/getCoverArt?u=%s&p=%s&v=1.16.1&c=Navidrome3DS&id=%s",
            g_base_url, g_cfg.username, g_cfg.password, album_id);

    Buffer buf = buf_new();
    int http_code = http_get(url, &buf);
    debug_log("[API] Album cover HTTP response: %d (size: %d bytes)", http_code, buf.len);
    
    if (http_code != 200 || !buf.data || buf.len == 0) {
        debug_log("[API] Failed to fetch album cover, HTTP code: %d", http_code);
        buf_free(&buf);
        LightEvent_Signal(event);
        free(data);
        return;
    }

    // Use stb_image to decode the image
    int width, height, channels;
    unsigned char *image_data = stbi_load_from_memory((const unsigned char *)buf.data, buf.len, &width, &height, &channels, 4);
    if (!image_data) {
        debug_log("[API] stb_image failed to decode image: %s", stbi_failure_reason());
        buf_free(&buf);
        LightEvent_Signal(event);
        free(data);
        return;
    }
    debug_log("[API] Decoded album cover: %dx%d, %d channels", width, height, channels);

    // Resize the image to fit 3DS texture constraints (max 1024x1024, power-of-2)
    int tex_width = width;
    int tex_height = height;
    
    // Clamp to 1024x1024 max
    if (tex_width > 1024) tex_width = 1024;
    if (tex_height > 1024) tex_height = 1024;
    
    // Round down to nearest power of 2
    tex_width = 1 << (31 - __builtin_clz(tex_width));
    tex_height = 1 << (31 - __builtin_clz(tex_height));
    
    // Clamp to 1024x1024 again (in case rounding up exceeded it)
    if (tex_width > 1024) tex_width = 1024;
    if (tex_height > 1024) tex_height = 1024;
    
    debug_log("[API] Resized album cover to: %dx%d", tex_width, tex_height);

    // Allocate a C3D_Tex
    C3D_Tex* tex = calloc(1, sizeof(C3D_Tex));
    if (!tex) {
        debug_log("[API] Failed to allocate texture memory");
        stbi_image_free(image_data);
        buf_free(&buf);
        LightEvent_Signal(event);
        free(data);
        return;
    }

    // Initialize the texture
    if (!C3D_TexInit(tex, (u16)tex_width, (u16)tex_height, GPU_RGBA8)) {
        debug_log("[API] C3D_TexInit failed");
        free(tex);
        stbi_image_free(image_data);
        buf_free(&buf);
        LightEvent_Signal(event);
        free(data);
        return;
    }

    // If the image is smaller than the texture, clear the texture first
    memset(tex->data, 0, tex->size);

    // Copy the image data into the texture (resizing if needed with bilinear interpolation)
    for (int y = 0; y < tex_height; y++) {
        for (int x = 0; x < tex_width; x++) {
            // Calculate source coordinates (floating-point for interpolation)
            float src_x = (x + 0.5f) * (float)width / (float)tex_width - 0.5f;
            float src_y = (y + 0.5f) * (float)height / (float)tex_height - 0.5f;
            
            // Clamp to image bounds
            if (src_x < 0) src_x = 0;
            if (src_y < 0) src_y = 0;
            if (src_x >= width - 1) src_x = width - 1.001f;
            if (src_y >= height - 1) src_y = height - 1.001f;
            
            // Get the 4 nearest pixels for bilinear interpolation
            int x1 = (int)src_x;
            int y1 = (int)src_y;
            int x2 = x1 + 1;
            int y2 = y1 + 1;
            
            // Calculate interpolation weights
            float x_weight = src_x - x1;
            float y_weight = src_y - y1;
            
            // Sample the 4 pixels (RGBA)
            u32 p1 = ((u32*)image_data)[y1 * width + x1];
            u32 p2 = ((u32*)image_data)[y1 * width + x2];
            u32 p3 = ((u32*)image_data)[y2 * width + x1];
            u32 p4 = ((u32*)image_data)[y2 * width + x2];
            
            // Interpolate R, G, B, A separately
            float r = (1.0f - y_weight) * ((1.0f - x_weight) * ((p1 >> 0) & 0xFF) + x_weight * ((p2 >> 0) & 0xFF))
                   + y_weight * ((1.0f - x_weight) * ((p3 >> 0) & 0xFF) + x_weight * ((p4 >> 0) & 0xFF));
            float g = (1.0f - y_weight) * ((1.0f - x_weight) * ((p1 >> 8) & 0xFF) + x_weight * ((p2 >> 8) & 0xFF))
                   + y_weight * ((1.0f - x_weight) * ((p3 >> 8) & 0xFF) + x_weight * ((p4 >> 8) & 0xFF));
            float b = (1.0f - y_weight) * ((1.0f - x_weight) * ((p1 >> 16) & 0xFF) + x_weight * ((p2 >> 16) & 0xFF))
                   + y_weight * ((1.0f - x_weight) * ((p3 >> 16) & 0xFF) + x_weight * ((p4 >> 16) & 0xFF));
            float a = (1.0f - y_weight) * ((1.0f - x_weight) * ((p1 >> 24) & 0xFF) + x_weight * ((p2 >> 24) & 0xFF))
                   + y_weight * ((1.0f - x_weight) * ((p3 >> 24) & 0xFF) + x_weight * ((p4 >> 24) & 0xFF));
            
            u32 color = ((u32)a << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
            
            // Write to the texture using 3DS tiled indexing
            ((u32*)tex->data)[tile_index(x, tex_height - 1 - y, tex_width)] = color;
        }
    }

    // Set up the subtexture metadata (use the full texture, let C2D_DrawImageAt scale it)
    state->album_cover_subtex.width = tex_width;
    state->album_cover_subtex.height = tex_height;
    state->album_cover_subtex.left = 0.0f;
    state->album_cover_subtex.top = 0.0f;
    state->album_cover_subtex.right = 1.0f;
    state->album_cover_subtex.bottom = 1.0f;

    // Set up the C2D_Image
    state->album_cover.tex = tex;
    state->album_cover.subtex = &state->album_cover_subtex;

    // Log texture info for debugging
    debug_log("[API] Texture setup: %dx%d, subtex: %dx%d (%.2f,%.2f)-(%.2f,%.2f)",
              tex_width, tex_height,
              state->album_cover_subtex.width, state->album_cover_subtex.height,
              state->album_cover_subtex.left, state->album_cover_subtex.top,
              state->album_cover_subtex.right, state->album_cover_subtex.bottom);

    // Clean up
    stbi_image_free(image_data);
    buf_free(&buf);
    LightEvent_Signal(event);
    free(data);
}

// Function to fetch album cover image
int api_get_album_cover(const char *album_id, void *out) {
    UiState *state = (UiState *)out;
    
    // If already loading this album, skip
    if (state->album_cover_loading && strcmp(state->album_cover_id, album_id) == 0) {
        debug_log("[API] Already loading album cover for: %s", album_id);
        return 0;
    }
    
    // If already loaded this album, skip
    if (state->album_cover.tex && strcmp(state->album_cover_id, album_id) == 0) {
        debug_log("[API] Album cover already loaded for: %s", album_id);
        return 0;
    }
    
    // Mark as loading
    state->album_cover_loading = true;
    strncpy(state->album_cover_id, album_id, sizeof(state->album_cover_id) - 1);
    state->album_cover_id[sizeof(state->album_cover_id) - 1] = '\0';
    
    // Free previous cover if it exists
    if (state->album_cover.tex) {
        C3D_TexDelete(state->album_cover.tex);
        free(state->album_cover.tex);
        state->album_cover.tex = NULL;
    }
    
    // Set up thread data
    AlbumCoverThreadData *data = calloc(1, sizeof(AlbumCoverThreadData));
    if (!data) {
        state->album_cover_loading = false;
        return -1;
    }
    
    data->state = state;
    strncpy(data->album_id, album_id, sizeof(data->album_id) - 1);
    data->album_id[sizeof(data->album_id) - 1] = '\0';
    
    // Set up event for thread synchronization
    LightEvent *event = calloc(1, sizeof(LightEvent));
    if (!event) {
        free(data);
        state->album_cover_loading = false;
        return -1;
    }
    LightEvent_Init(event, RESET_STICKY);
    data->event = event;
    
    // Create the background thread
    Thread thread = threadCreate(album_cover_thread_func, data, 32 * 1024, 0x30, -2, false);
    if (!thread) {
        debug_log("[API] Failed to create album cover thread");
        free(event);
        free(data);
        state->album_cover_loading = false;
        return -1;
    }
    
    // Detach the thread (we don't need to join it)
    threadDetach(thread);
    
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
    int code = http_get(url, &buf);
    debug_log("[API] Ping HTTP code: %d", code);
    int ok = (code == 200 && buf.data && strstr(buf.data, "status=\"ok\""));
    debug_log("[API] Ping response: %s", buf.data ? buf.data : "(null)");
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
