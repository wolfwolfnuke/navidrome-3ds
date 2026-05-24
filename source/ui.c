#include "ui.h"
#include "debug.h"
#include "audio.h"
#include <3ds.h>
#include <citro2d.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Layout constants (3DS screens)
//   Top screen: 400x240
//   Bottom screen: 320x240
// ---------------------------------------------------------------------------
#define TOP_W   400
#define TOP_H   240
#define BOT_W   320
#define BOT_H   240

#define ROW_H    22
#define LIST_Y   40
#define VISIBLE_ROWS  8

// Colors
#define COL_BG          C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF)
#define COL_HEADER_BG   C2D_Color32(0x16, 0x21, 0x3e, 0xFF)
#define COL_SELECTED    C2D_Color32(0x0f, 0x3d, 0x66, 0xFF)
#define COL_TEXT        C2D_Color32(0xe0, 0xe0, 0xff, 0xFF)
#define COL_DIM         C2D_Color32(0x80, 0x80, 0xa0, 0xFF)
#define COL_ACCENT      C2D_Color32(0x00, 0xbc, 0xd4, 0xFF)
#define COL_STATUS_BG   C2D_Color32(0x0d, 0x47, 0xa1, 0xFF)

static C2D_TextBuf s_tbuf;

// ---------------------------------------------------------------------------
// Font management — load all system fonts for full Unicode coverage
// ---------------------------------------------------------------------------
#define MAX_FONTS 8
static C2D_Font s_fonts[MAX_FONTS];

// ---------------------------------------------------------------------------
// Text cache — parse/optimize once, draw every frame.
// Without this, draw_text() re-parses and re-optimizes every frame,
// which is extremely slow (especially with non-system fonts).
// ---------------------------------------------------------------------------
#define MAX_CACHED_TEXT 256

typedef struct {
    char     str[128];
    int      font_idx;
    float    scale;
    C2D_Text parsed;   // pre-parsed, pre-optimized text object
} CachedText;

static CachedText s_text_cache[MAX_CACHED_TEXT];
static int        s_cache_count = 0;

static CachedText* cache_get_or_create(const char *str, int font_idx, float scale) {
    // Search for existing entry
    for (int i = 0; i < s_cache_count; i++) {
        if (s_text_cache[i].font_idx == font_idx &&
            s_text_cache[i].scale == scale &&
            strcmp(s_text_cache[i].str, str) == 0) {
            return &s_text_cache[i];
        }
    }
    // Evict oldest if full
    if (s_cache_count >= MAX_CACHED_TEXT) {
        // Shift entries down, evicting the oldest
        for (int i = 0; i < MAX_CACHED_TEXT - 1; i++) {
            s_text_cache[i] = s_text_cache[i + 1];
        }
        s_cache_count--;
    }
    // Insert new entry at the end (newest)
    CachedText *entry = &s_text_cache[s_cache_count++];
    strncpy(entry->str, str, sizeof(entry->str) - 1);
    entry->str[sizeof(entry->str) - 1] = '\0';
    entry->font_idx = font_idx;
    entry->scale = scale;
    // Parse and optimize into the cached text object
    if (s_fonts[font_idx]) {
        C2D_TextFontParse(&entry->parsed, s_fonts[font_idx], s_tbuf, str);
    } else {
        C2D_TextParse(&entry->parsed, s_tbuf, str);
    }
    C2D_TextOptimize(&entry->parsed);
    return entry;
}

static void cache_clear(void) {
    s_cache_count = 0;
}
static int      s_font_count = 0;

// System font region codes (3DS has separate fonts per region/script)
static const CFG_Region s_font_regions[] = {
    CFG_REGION_USA,   // Latin + basic
    CFG_REGION_JPN,   // Japanese (hiragana, katakana, kanji)
    CFG_REGION_CHN,   // Simplified Chinese
    CFG_REGION_TWN,   // Traditional Chinese
    CFG_REGION_KOR,   // Korean
};

static void fonts_init(void) {
    debug_log("[UI] fonts_init called");
    // Try to load the standard system font first
    s_fonts[s_font_count] = C2D_FontLoadSystem(CFG_REGION_USA);
    if (s_fonts[s_font_count]) {
        s_font_count++;
        debug_log("[UI] Loaded system font");
    } else {
        debug_log("[UI] ERROR: Failed to load standard system font (CFG_REGION_USA), trying fallback .bcfnt from romfs");
        // Fallback: load bundled font from romfs
        C2D_Font fallback = C2D_FontLoad("romfs:/popjoy.bcfnt");
        if (fallback) {
            s_fonts[s_font_count++] = fallback;
            debug_log("[UI] Loaded fallback font from romfs/popjoy.bcfnt");
        } else {
            debug_log("[UI] FATAL: No fonts available at all!");
        }
    }
    // Optionally try to load CJK fonts if you have them bundled as .bcfnt files
    debug_log("[UI] fonts_init complete, total fonts=%d", s_font_count);
}

static void fonts_cleanup(void) {
    debug_log("[ENTER] fonts_cleanup()");
    debug_log("[ENTER] fonts_cleanup()");
    for (int i = 0; i < s_font_count; i++) {
        if (s_fonts[i]) C2D_FontFree(s_fonts[i]);
        s_fonts[i] = NULL;
    }
    s_font_count = 0;
}

// ---------------------------------------------------------------------------
// UTF-8 decoder: returns next codepoint and advances *str
// ---------------------------------------------------------------------------
static uint32_t utf8_next(const char **str) {
    const unsigned char *s = (const unsigned char *)*str;
    uint32_t cp;
    if (*s < 0x80) {
        cp = *s++;
    } else if (*s < 0xE0) {
        cp = (*s++ & 0x1F) << 6;
        if (*s >= 0x80) cp |= (*s++ & 0x3F);
    } else if (*s < 0xF0) {
        cp = (*s++ & 0x0F) << 12;
        if (*s >= 0x80) cp |= (*s++ & 0x3F) << 6;
        if (*s >= 0x80) cp |= (*s++ & 0x3F);
    } else {
        cp = (*s++ & 0x07) << 18;
        if (*s >= 0x80) cp |= (*s++ & 0x3F) << 12;
        if (*s >= 0x80) cp |= (*s++ & 0x3F) << 6;
        if (*s >= 0x80) cp |= (*s++ & 0x3F);
    }
    *str = (const char *)s;
    return cp;
}

// ---------------------------------------------------------------------------
// draw_text: renders UTF-8 string using system fonts for full CJK support
// Uses a text cache so parse/optimize happen once per unique string+font+scale.
// ---------------------------------------------------------------------------
static void draw_text(float x, float y, float sz, u32 color, const char *str) {
    if (!str || !str[0]) return;

    char seg[256];
    int  seg_font = 0;
    int  seg_len  = 0;
    float cur_x = x;

    const char *p = str;
    while (*p) {
        const char *before = p;
        uint32_t cp = utf8_next(&p);
        int seq_len = (int)(p - before);

        // Find which font has this glyph
        int found_font = 0;
        for (int fi = 0; fi < s_font_count; fi++) {
            if (s_fonts[fi] && C2D_FontGlyphIndexFromCodePoint(s_fonts[fi], cp) != 0) {
                found_font = fi;
                break;
            }
        }

        // If font changed or buffer full, flush current segment
        if ((found_font != seg_font || seg_len + seq_len >= (int)sizeof(seg) - 1) && seg_len > 0) {
            seg[seg_len] = '\0';

            // Use cached text object — parse/optimize only once per unique string
            CachedText *cached = cache_get_or_create(seg, seg_font, sz);

            // Measure width to advance cur_x
            float tw = 0, th = 0;
            C2D_TextGetDimensions(&cached->parsed, sz, sz, &tw, &th);
            C2D_DrawText(&cached->parsed, C2D_WithColor | C2D_AtBaseline,
                         cur_x, y, 0.5f, sz, sz, color);
            cur_x += tw;
            seg_len = 0;
        }

        seg_font = found_font;
        memcpy(seg + seg_len, before, seq_len);
        seg_len += seq_len;
    }

    // Flush remaining segment
    if (seg_len > 0) {
        seg[seg_len] = '\0';
        CachedText *cached = cache_get_or_create(seg, seg_font, sz);
        float tw = 0, th = 0;
        C2D_TextGetDimensions(&cached->parsed, sz, sz, &tw, &th);
        C2D_DrawText(&cached->parsed, C2D_WithColor | C2D_AtBaseline,
                     cur_x, y, 0.5f, sz, sz, color);
    }
}

// ---------------------------------------------------------------------------
// Checkbox helpers — drawn in the search bar
// ---------------------------------------------------------------------------
#define CHECKBOX_X_START  8
#define CHECKBOX_Y        34
#define CHECKBOX_W        48
#define CHECKBOX_H        16
#define CHECKBOX_GAP      8

// Checkbox field indices: 0=name/title, 1=artist, 2=album
static const char *s_checkbox_labels[] = { "Name", "Artist", "Album" };
static const int   s_checkbox_bits[]   = { 1, 2, 4 };
#define NUM_CHECKBOXES 3

// Returns checkbox index if touched, -1 otherwise
static int hit_test_checkboxes(touchPosition touch) {
    for (int i = 0; i < NUM_CHECKBOXES; i++) {
        float x = CHECKBOX_X_START + i * (CHECKBOX_W + CHECKBOX_GAP);
        float y = CHECKBOX_Y;
        if (touch.px >= (int)x && touch.px < (int)(x + CHECKBOX_W) &&
            touch.py >= (int)y && touch.py < (int)(y + CHECKBOX_H)) {
            return i;
        }
    }
    return -1;
}

// Toggle a filter field by index (0=name, 1=artist, 2=album)
void ui_search_toggle_field(UiState *state, int field_idx) {
    if (field_idx < 0 || field_idx >= NUM_CHECKBOXES) return;
    int bit = s_checkbox_bits[field_idx];
    state->filter_fields ^= bit;
    state->scroll_offset = 0;
}

static void draw_rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

// Draw a scrollable list on the bottom screen
// If draw_header is 0, skip the header bar (caller draws it instead)
static void draw_list(const char **names, int count,
                       int selected, int scroll, const char *title,
                       int draw_header) {
    if (count == 0) {
        draw_rect(0, 0, BOT_W, BOT_H, COL_BG);
        draw_text(8, BOT_H/2 - 8, 0.5f, COL_DIM, "No results found.");
        return;
    }

    if (draw_header) {
        // Header bar
        draw_rect(0, 0, BOT_W, 30, COL_HEADER_BG);
        draw_text(8, 20, 0.55f, COL_ACCENT, title);
    }

    // Items
    for (int i = 0; i < VISIBLE_ROWS; i++) {
        int idx = scroll + i;
        if (idx >= count) break;

        float y = LIST_Y + i * ROW_H;
        if (idx == selected)
            draw_rect(0, y - 2, BOT_W, ROW_H, COL_SELECTED);

        char label[160];
        snprintf(label, sizeof(label), "%s", names[idx]);
        u32 col = (idx == selected) ? COL_ACCENT : COL_TEXT;
        draw_text(8, y + 14, 0.45f, col, label);
    }

    // Scrollbar
    if (count > VISIBLE_ROWS) {
        float bar_h = (float)VISIBLE_ROWS / count * (BOT_H - LIST_Y);
        float bar_y = LIST_Y + (float)scroll / count * (BOT_H - LIST_Y);
        draw_rect(BOT_W - 4, LIST_Y, 4, BOT_H - LIST_Y, COL_DIM);
        draw_rect(BOT_W - 4, bar_y,  4, bar_h,           COL_ACCENT);
    }
}

// ---------------------------------------------------------------------------
// Top-screen "Now Playing" panel
// ---------------------------------------------------------------------------
static void draw_now_playing(const UiState *state) {
    draw_rect(0, 0, TOP_W, TOP_H, COL_BG);
    draw_text(8, 20, 0.65f, COL_ACCENT, "Navidrome 3DS");

    if (state->screen == SCREEN_PLAYER || audio_is_playing()) {
        // Show current track info from UiState
        if (state->tracks.count > 0 && state->selected_track < state->tracks.count) {
            const NaviTrack *t = &state->tracks.items[state->selected_track];
            draw_text(8, 55,  0.5f,  COL_TEXT, t->title);
            draw_text(8, 78,  0.45f, COL_DIM,  t->artist);
            draw_text(8, 98,  0.42f, COL_DIM,  t->album);
        }

        if (audio_is_playing()) {
            const char *playstate = audio_is_paused() ? "|| PAUSED" : "> PLAYING";
            draw_text(8, 128, 0.5f, COL_ACCENT, playstate);

            float vol = audio_get_volume();
            draw_text(8,   158, 0.42f, COL_DIM, "Volume");
            draw_rect(8,   168, 180, 7, COL_DIM);
            draw_rect(8,   168, 180.0f * vol, 7, COL_ACCENT);
        } else {
            draw_text(8, 128, 0.45f, COL_DIM, "Downloading...");
        }
    } else {
        draw_text(8, 90,  0.5f,  COL_DIM, "No track playing.");
        draw_text(8, 115, 0.45f, COL_DIM, "Browse on the bottom screen.");
    }

    // Controls bar
    draw_rect(0, TOP_H - 24, TOP_W, 24, COL_HEADER_BG);
    draw_text(4, TOP_H - 6, 0.38f, COL_DIM,
        "START:Pause  L/R:Vol  B:Back  A:Select");
}

// ---------------------------------------------------------------------------
// Search helpers — unified filter with per-field checkboxes
// ---------------------------------------------------------------------------
// filter_fields bitmask:
//   bit 0 (1) = search name / title
//   bit 1 (2) = search artist
//   bit 2 (4) = search album
// If no bits are set, all fields are searched (backward-compatible default).
// ---------------------------------------------------------------------------

static int strcasestr_simple(const char *haystack, const char *needle) {
    if (!needle[0]) return 1;
    // Pre-compute lowercase needle to avoid repeated tolower() calls
    char needle_lower[128];
    int nlen = 0;
    for (; needle[nlen] && nlen < 127; nlen++)
        needle_lower[nlen] = (char)tolower(needle[nlen]);
    needle_lower[nlen] = '\0';

    for (const char *h = haystack; *h; h++) {
        int i = 0;
        while (needle_lower[i] && h[i] && ((char)tolower(h[i]) == needle_lower[i])) i++;
        if (!needle_lower[i]) return 1;
    }
    return 0;
}

static int match_field(const char *field, const char *query, int field_bit, int filter_fields) {
    // If no filter bits set, match everything (all fields active)
    if (filter_fields == 0) return 1;
    // If this field's bit is set, check it
    if (filter_fields & field_bit) {
        return strcasestr_simple(field, query);
    }
    // Field not in filter — skip it
    return 0;
}

static int artist_matches(const NaviArtist *a, const char *query, int filter_fields) {
    int name_ok = match_field(a->name, query, 1, filter_fields);
    int artist_ok = match_field(a->artist, query, 2, filter_fields);
    return name_ok || artist_ok;
}

static int album_matches(const NaviAlbum *a, const char *query, int filter_fields) {
    int name_ok = match_field(a->name, query, 1, filter_fields);
    int artist_ok = match_field(a->artist, query, 2, filter_fields);
    return name_ok || artist_ok;
}

static int track_matches(const NaviTrack *t, const char *query, int filter_fields) {
    int title_ok = match_field(t->title, query, 1, filter_fields);
    int artist_ok = match_field(t->artist, query, 2, filter_fields);
    int album_ok = match_field(t->album, query, 4, filter_fields);
    return title_ok || artist_ok || album_ok;
}

static void filter_artists(const NaviArtistList *src, NaviArtistList *dst, const char *query, int filter_fields) {
    dst->count = 0;
    for (int i = 0; i < src->count; i++) {
        if (artist_matches(&src->items[i], query, filter_fields)) {
            if (dst->count >= MAX_ITEMS) {
                debug_log("[BOUNDS] filter_artists: dst->count=%d >= MAX_ITEMS=%d", dst->count, MAX_ITEMS);
                break;
            }
            dst->items[dst->count++] = src->items[i];
        }
    }
}

static void filter_albums(const NaviAlbumList *src, NaviAlbumList *dst, const char *query, int filter_fields) {
    dst->count = 0;
    for (int i = 0; i < src->count; i++) {
        if (album_matches(&src->items[i], query, filter_fields)) {
            if (dst->count >= MAX_ITEMS) {
                debug_log("[BOUNDS] filter_albums: dst->count=%d >= MAX_ITEMS=%d", dst->count, MAX_ITEMS);
                break;
            }
            dst->items[dst->count++] = src->items[i];
        }
    }
}

static void filter_tracks(const NaviTrackList *src, NaviTrackList *dst, const char *query, int filter_fields) {
    dst->count = 0;
    for (int i = 0; i < src->count; i++) {
        if (track_matches(&src->items[i], query, filter_fields)) {
            if (dst->count >= MAX_ITEMS) {
                debug_log("[BOUNDS] filter_tracks: dst->count=%d >= MAX_ITEMS=%d", dst->count, MAX_ITEMS);
                break;
            }
            dst->items[dst->count++] = src->items[i];
        }
    }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

void ui_draw(const UiState *state, C3D_RenderTarget *top, C3D_RenderTarget *bottom) {
    if (s_font_count == 0) {
        debug_log("FATAL: No system fonts loaded. Skipping UI draw and exiting.");
        // Show a blank screen and exit gracefully
        C2D_SceneBegin(top);
        C2D_TargetClear(top, C2D_Color32(0,0,0,0xFF));
        C2D_SceneBegin(bottom);
        C2D_TargetClear(bottom, C2D_Color32(0,0,0,0xFF));
        C3D_FrameEnd(0);
        debug_cleanup();
        socExit();
        httpcExit();
        romfsExit();
        C2D_Fini();
        C3D_Fini();
        gfxExit();
        exit(1);
    }
    // --- Top screen ---
    C2D_SceneBegin(top);
    draw_now_playing(state);

    // Status message overlay
    if (state->status_msg[0]) {
        draw_rect(0, TOP_H/2 - 16, TOP_W, 32, COL_STATUS_BG);
        draw_text(8, TOP_H/2 + 8, 0.45f, COL_TEXT, state->status_msg);
    }
    if (state->loading) {
        draw_rect(0, TOP_H/2 - 16, TOP_W, 32, COL_STATUS_BG);
        draw_text(8, TOP_H/2 + 8, 0.45f, COL_ACCENT, "Loading...");
    }

    // --- Bottom screen ---
    C2D_SceneBegin(bottom);
    draw_rect(0, 0, BOT_W, BOT_H, COL_BG);

    // Build name arrays for the list
    static const char *names[MAX_ITEMS];

    // Determine which fields to search based on current screen
    // Artists screen: search name + artist
    // Albums screen: search name + artist
    // Tracks screen: search title + artist + album
    int screen_filter = 0;
    switch (state->screen) {
        case SCREEN_ARTISTS:  screen_filter = 1 | 2; break; // name + artist
        case SCREEN_ALBUMS:   screen_filter = 1 | 2; break; // name + artist
        case SCREEN_TRACKS:   screen_filter = 1 | 2 | 4; break; // name + artist + album
        default: break;
    }

    // If search is active, intersect user's filter with screen's available fields
    int effective_filter = 0;
    if (state->search_active && state->search_query[0]) {
        // User's chosen fields intersected with what's available on this screen
        for (int i = 0; i < NUM_CHECKBOXES; i++) {
            int bit = s_checkbox_bits[i];
            if (state->filter_fields == 0) {
                // No user filter: use all fields available on this screen
                if (screen_filter & bit) effective_filter |= bit;
            } else {
                // User has explicit filter: only use fields that are both
                // in user's filter AND available on this screen
                if ((state->filter_fields & bit) && (screen_filter & bit)) {
                    effective_filter |= bit;
                }
            }
        }
    }

    // Filtered lists must be static — each is ~32-34KB (200 items) and would
    // overflow the 128KB main thread stack if declared as local variables.
    static NaviArtistList filtered_artists;
    static NaviAlbumList  filtered_albums;
    static NaviTrackList  filtered_tracks;

    switch (state->screen) {
        case SCREEN_ARTISTS: {
            const NaviArtistList *src = &state->artists;
            int display_count = state->artists.count;
            int display_selected = state->selected_artist;
            int display_scroll = state->scroll_offset;
            if (state->search_active && state->search_query[0]) {
                filter_artists(&state->artists, &filtered_artists, state->search_query, effective_filter);
                src = &filtered_artists;
                display_count = filtered_artists.count;
                display_selected = state->search_selected;
                display_scroll = state->search_scroll;
            }
            for (int i = 0; i < display_count && i < MAX_ITEMS; i++) {
                names[i] = src->items[i].name;
            }
            // Clamp selection to prevent out-of-bounds highlight
            if (display_count > 0 && display_selected >= display_count)
                display_selected = display_count - 1;
            if (display_count == 0)
                display_selected = 0;
            if (display_count > MAX_ITEMS) debug_log("[BOUNDS] ARTISTS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
            draw_list(names, display_count,
                      display_selected, display_scroll,
                      state->search_active ? "Artists (Search)" : "Artists",
                      !state->search_active);
            break;
        }
        case SCREEN_ALBUMS: {
            const NaviAlbumList *src = &state->albums;
            int display_count = state->albums.count;
            int display_selected = state->selected_album;
            int display_scroll = state->scroll_offset;
            if (state->search_active && state->search_query[0]) {
                filter_albums(&state->albums, &filtered_albums, state->search_query, effective_filter);
                src = &filtered_albums;
                display_count = filtered_albums.count;
                display_selected = state->search_selected;
                display_scroll = state->search_scroll;
            }
            for (int i = 0; i < display_count && i < MAX_ITEMS; i++) {
                names[i] = src->items[i].name;
            }
            // Clamp selection to prevent out-of-bounds highlight
            if (display_count > 0 && display_selected >= display_count)
                display_selected = display_count - 1;
            if (display_count == 0)
                display_selected = 0;
            if (display_count > MAX_ITEMS) debug_log("[BOUNDS] ALBUMS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
            draw_list(names, display_count,
                      display_selected, display_scroll,
                      state->search_active ? "Albums (Search)" : "Albums",
                      !state->search_active);
            break;
        }
        case SCREEN_TRACKS: {
            const NaviTrackList *src = &state->tracks;
            int display_count = state->tracks.count;
            int display_selected = state->selected_track;
            int display_scroll = state->scroll_offset;
            if (state->search_active && state->search_query[0]) {
                filter_tracks(&state->tracks, &filtered_tracks, state->search_query, effective_filter);
                src = &filtered_tracks;
                display_count = filtered_tracks.count;
                display_selected = state->search_selected;
                display_scroll = state->search_scroll;
            }
            for (int i = 0; i < display_count && i < MAX_ITEMS; i++) {
                names[i] = src->items[i].title;
            }
            // Clamp selection to prevent out-of-bounds highlight
            if (display_count > 0 && display_selected >= display_count)
                display_selected = display_count - 1;
            if (display_count == 0)
                display_selected = 0;
            if (display_count > MAX_ITEMS) debug_log("[BOUNDS] TRACKS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
            draw_list(names, display_count,
                      display_selected, display_scroll,
                      state->search_active ? "Tracks (Search)" : "Tracks",
                      !state->search_active);
            break;
        }
        case SCREEN_PLAYER:
            draw_text(8, 50, 0.5f, COL_TEXT,  "Playback controls:");
            draw_text(8, 80, 0.45f, COL_DIM,  "START  - Pause / Resume");
            draw_text(8, 100, 0.45f, COL_DIM, "SELECT - Stop");
            draw_text(8, 120, 0.45f, COL_DIM, "L / R  - Volume -/+");
            draw_text(8, 140, 0.45f, COL_DIM, "B      - Back to track list");
            break;
    }

    // --- Search bar with checkboxes (drawn on top of list) ---
    if (state->search_active) {
        // Search bar background
        draw_rect(0, 0, BOT_W, 30, COL_HEADER_BG);
        char bar[160];
        snprintf(bar, sizeof(bar), "Search: %s", state->search_query[0] ? state->search_query : "...");
        draw_text(8, 19, 0.55f, COL_ACCENT, bar);

        // Draw checkboxes below the search bar
        for (int i = 0; i < NUM_CHECKBOXES; i++) {
            int active = (state->filter_fields & s_checkbox_bits[i]) != 0;
            // Default: if no bits set, all are active
            if (state->filter_fields == 0) active = 1;
            float x = CHECKBOX_X_START + i * (CHECKBOX_W + CHECKBOX_GAP);
            draw_rect(x, CHECKBOX_Y, CHECKBOX_W, CHECKBOX_H,
                      active ? COL_ACCENT : COL_HEADER_BG);
            if (active) {
                draw_text(x + 4, CHECKBOX_Y + 12, 0.45f, COL_BG, "X");
            }
            draw_text(x + CHECKBOX_W + 2, CHECKBOX_Y + 12, 0.42f, COL_TEXT,
                      s_checkbox_labels[i]);
        }

        // Help text
        draw_text(8, CHECKBOX_Y + CHECKBOX_H + 4, 0.35f, COL_DIM,
                  "Touch:toggle  D-Pad:cycle  A:toggle  Y:clear  B/X:back");
    }
}

// --- Search input helpers ---
void ui_search_activate(UiState *state) {
    switch (state->screen) {
        case SCREEN_ARTISTS:
            state->search_saved_sel     = state->selected_artist;
            state->search_saved_scroll  = state->scroll_offset;
            break;
        case SCREEN_ALBUMS:
            state->search_saved_sel     = state->selected_album;
            state->search_saved_scroll  = state->scroll_offset;
            break;
        case SCREEN_TRACKS:
            state->search_saved_sel     = state->selected_track;
            state->search_saved_scroll  = state->scroll_offset;
            break;
    }

    state->search_active    = 1;
    state->search_query[0]  = '\0';
    state->filter_fields    = 0;
    state->search_focus     = 0;
    state->search_selected  = 0;
    state->search_scroll    = 0;
    state->scroll_offset    = 0;
}
// Restore the user to their exact position before search was activated.
// Used when the user exits search via B/X without selecting an item.
static void ui_search_deactivate(UiState *state) {
    switch (state->screen) {
        case SCREEN_ARTISTS: state->selected_artist = state->search_saved_sel; break;
        case SCREEN_ALBUMS:  state->selected_album  = state->search_saved_sel; break;
        case SCREEN_TRACKS:  state->selected_track  = state->search_saved_sel; break;
        default: break;
    }
    state->scroll_offset = state->search_saved_scroll;

    state->search_active    = 0;
    state->search_query[0]  = '\0';
    state->filter_fields    = 0;
    state->search_focus     = 0;
    state->search_selected  = 0;
    state->search_scroll    = 0;
}

// Map the search_selected index (filtered list) back to the full list
// index via ID lookup. Used when the user A-selects a search result.
static void select_search_item(UiState *state) {
    int *sel = NULL;
    switch (state->screen) {
        case SCREEN_ARTISTS: sel = &state->selected_artist; break;
        case SCREEN_ALBUMS:  sel = &state->selected_album;  break;
        case SCREEN_TRACKS:  sel = &state->selected_track;  break;
        default: break;
    }
    if (!sel || !state->search_query[0]) return;

    // Build effective filter same way as ui_draw does
    int screen_filter = 0;
    switch (state->screen) {
        case SCREEN_ARTISTS:  screen_filter = 1 | 2; break;
        case SCREEN_ALBUMS:   screen_filter = 1 | 2; break;
        case SCREEN_TRACKS:   screen_filter = 1 | 2 | 4; break;
        default: break;
    }
    int effective_filter = 0;
    for (int i = 0; i < NUM_CHECKBOXES; i++) {
        int bit = s_checkbox_bits[i];
        if (state->filter_fields == 0) {
            if (screen_filter & bit) effective_filter |= bit;
        } else {
            if ((state->filter_fields & bit) && (screen_filter & bit))
                effective_filter |= bit;
        }
    }

    // Get the item at search_selected from the filtered list
    // We need to re-filter to find the ID. Build a temporary filtered list.
    NaviArtistList tmp_artists = {0};
    NaviAlbumList  tmp_albums  = {0};
    NaviTrackList  tmp_tracks  = {0};
    const void *filtered = NULL;

    switch (state->screen) {
        case SCREEN_ARTISTS:
            filter_artists(&state->artists, &tmp_artists, state->search_query, effective_filter);
            filtered = &tmp_artists;
            break;
        case SCREEN_ALBUMS:
            filter_albums(&state->albums, &tmp_albums, state->search_query, effective_filter);
            filtered = &tmp_albums;
            break;
        case SCREEN_TRACKS:
            filter_tracks(&state->tracks, &tmp_tracks, state->search_query, effective_filter);
            filtered = &tmp_tracks;
            break;
        default: break;
    }

    if (!filtered) return;

    int idx = state->search_selected;
    if (idx < 0) idx = 0;
    if (state->screen == SCREEN_ARTISTS && idx >= tmp_artists.count) idx = tmp_artists.count - 1;
    if (state->screen == SCREEN_ALBUMS  && idx >= tmp_albums.count)  idx = tmp_albums.count - 1;
    if (state->screen == SCREEN_TRACKS  && idx >= tmp_tracks.count)  idx = tmp_tracks.count - 1;

    // Find the matching ID in the full (unfiltered) list
    char match_id[MAX_ID_LEN];
    if (state->screen == SCREEN_ARTISTS) {
        if (tmp_artists.count == 0) return;
        strncpy(match_id, tmp_artists.items[idx].id, MAX_ID_LEN - 1);
        match_id[MAX_ID_LEN - 1] = '\0';
        for (int i = 0; i < state->artists.count; i++) {
            if (strcmp(state->artists.items[i].id, match_id) == 0) {
                *sel = i;
                return;
            }
        }
    } else if (state->screen == SCREEN_ALBUMS) {
        if (tmp_albums.count == 0) return;
        strncpy(match_id, tmp_albums.items[idx].id, MAX_ID_LEN - 1);
        match_id[MAX_ID_LEN - 1] = '\0';
        for (int i = 0; i < state->albums.count; i++) {
            if (strcmp(state->albums.items[i].id, match_id) == 0) {
                *sel = i;
                return;
            }
        }
    } else if (state->screen == SCREEN_TRACKS) {
        if (tmp_tracks.count == 0) return;
        strncpy(match_id, tmp_tracks.items[idx].id, MAX_ID_LEN - 1);
        match_id[MAX_ID_LEN - 1] = '\0';
        for (int i = 0; i < state->tracks.count; i++) {
            if (strcmp(state->tracks.items[i].id, match_id) == 0) {
                *sel = i;
                return;
            }
        }
    }

    // Fallback: if ID not found, just use the filtered index
    *sel = idx;
}
void ui_search_input(UiState *state, char c) {
    size_t len = strlen(state->search_query);
    if (len < sizeof(state->search_query) - 1) {
        state->search_query[len] = c;
        state->search_query[len+1] = '\0';
        state->scroll_offset = 0;
    }
}
void ui_search_backspace(UiState *state) {
    size_t len = strlen(state->search_query);
    if (len > 0) {
        state->search_query[len-1] = '\0';
        state->scroll_offset = 0;
    }
}
void ui_search_apply(UiState *state) {
    state->scroll_offset = 0;
}
void ui_search_clear(UiState *state) {
    state->search_query[0] = '\0';
    state->scroll_offset = 0;
}

bool ui_handle_input(UiState *state) {
    hidScanInput();
    u32 down  = hidKeysDown();
    u32 held  = hidKeysHeld();
    static u32 repeat_timer = 0;
    static u32 last_held = 0;

    // --- Search mode with checkbox toggling ---
    if (state->search_active) {
        // Touch: toggle checkboxes
        touchPosition touch;
        hidTouchRead(&touch);
        if (touch.px > 0 || touch.py > 0) {
            int cb_idx = hit_test_checkboxes(touch);
            if (cb_idx >= 0) {
                ui_search_toggle_field(state, cb_idx);
                return false;
            }
        }

        // D-pad left/right cycles through focus (0=search bar, 1-3=checkboxes)
        if (down & KEY_DLEFT) {
            state->search_focus--;
            if (state->search_focus < 0) state->search_focus = NUM_CHECKBOXES;
            return false;
        }
        if (down & KEY_DRIGHT) {
            state->search_focus++;
            if (state->search_focus > NUM_CHECKBOXES) state->search_focus = 0;
            return false;
        }

        // Priority-based action system:
        // 1=select item, 2=exit search, 3=open keyboard
        int action = 0;

        // D-pad UP/DOWN navigates filtered list when focus is on search bar
        if (down & KEY_DUP && state->search_focus == 0) {
            if (state->search_selected > 0) state->search_selected--;
            if (state->search_selected < state->search_scroll)
                state->search_scroll = state->search_selected;
        }
        if (down & KEY_DDOWN && state->search_focus == 0) {
            state->search_selected++;
            if (state->search_selected >= state->search_scroll + VISIBLE_ROWS)
                state->search_scroll = state->search_selected - VISIBLE_ROWS + 1;
        }

        // A: highest priority — open keyboard on empty query, or select item
        if (down & KEY_A && state->search_focus == 0) {
            if (state->search_query[0] == '\0') {
                action = 3; // keyboard
            } else {
                action = 1; // select item
            }
        } else if (down & KEY_A && state->search_focus > 0) {
            // Toggle focused checkbox
            ui_search_toggle_field(state, state->search_focus - 1);
            return false;
        }

        // B/X: priority 2 — exit search (only if A wasn't pressed)
        if (!action && (down & (KEY_B | KEY_X))) action = 2;

        // Y: clear query while keeping search active
        if (!action && (down & KEY_Y)) {
            ui_search_clear(state);
            return false;
        }

        switch (action) {
            case 1: // Select item and exit — fall through to main block
                select_search_item(state);
                state->search_active    = 0;
                state->search_query[0]  = '\0';
                state->filter_fields    = 0;
                state->search_focus     = 0;
                state->search_selected  = 0;
                state->search_scroll    = 0;
                // Intentionally NOT returning here — fall through to main block
                // so the A handler below can perform the screen transition.
                break;
            case 2: // Exit search without selecting
                ui_search_deactivate(state);
                return false;
            case 3: // Open keyboard
                {
                SwkbdState swkbd;
                char kbdout[128] = {0};
                swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, sizeof(kbdout)-1);
                swkbdSetHintText(&swkbd, "Search album, artist, or song");
                if (swkbdInputText(&swkbd, kbdout, sizeof(kbdout)) == SWKBD_BUTTON_CONFIRM) {
                    strncpy(state->search_query, kbdout, sizeof(state->search_query)-1);
                    state->search_query[sizeof(state->search_query)-1] = '\0';
                    state->scroll_offset = 0;
                }
                }
                return false;
            default:
                if (down & (KEY_DUP | KEY_DDOWN | KEY_DLEFT | KEY_DRIGHT))
                    return false;
                // Otherwise fall through to main block
                break;
        }
    }

    // Key repeat logic (simple, per frame)
    if (held & (KEY_DUP | KEY_DDOWN)) {
        if (last_held != held) {
            repeat_timer = 0; // reset on new press
        } else {
            repeat_timer++;
        }
        // After initial delay, treat as repeated press every 4 frames
        if (repeat_timer == 15 || (repeat_timer > 15 && (repeat_timer % 4 == 0))) {
            down |= held & (KEY_DUP | KEY_DDOWN);
        }
    } else {
        repeat_timer = 0;
    }
    last_held = held;

    int *sel = NULL;
    int  max = 0;

    switch (state->screen) {
        case SCREEN_ARTISTS: sel = &state->selected_artist; max = state->artists.count; break;
        case SCREEN_ALBUMS:  sel = &state->selected_album;  max = state->albums.count;  break;
        case SCREEN_TRACKS:  sel = &state->selected_track;  max = state->tracks.count;  break;
        default: break;
    }

    // Volume control (works on all screens)
    if (down & KEY_L) audio_set_volume(audio_get_volume() - 0.1f);
    if (down & KEY_R) audio_set_volume(audio_get_volume() + 0.1f);

    // Pause / resume
    if (down & KEY_START)  audio_toggle_pause();
    if (down & KEY_SELECT) audio_stop();

    if (sel && max > 0) {
        if (down & KEY_DUP) {
            if (*sel > 0) (*sel)--;
            if (*sel < state->scroll_offset)
                state->scroll_offset = *sel;
        }
        if (down & KEY_DDOWN) {
            if (*sel < max - 1) (*sel)++;
            if (*sel >= state->scroll_offset + VISIBLE_ROWS)
                state->scroll_offset = *sel - VISIBLE_ROWS + 1;
        }
        // Unified search activation: Y opens search on any screen
        if (down & KEY_Y) { ui_search_activate(state); return false; }
    }

    if (down & KEY_A) {
        switch (state->screen) {
            case SCREEN_ARTISTS:
                state->screen       = SCREEN_ALBUMS;
                state->selected_album = 0;
                state->scroll_offset  = 0;
                return true; // signal: load albums
            case SCREEN_ALBUMS:
                state->screen        = SCREEN_TRACKS;
                state->selected_track = 0;
                state->scroll_offset  = 0;
                return true; // signal: load tracks
            case SCREEN_TRACKS:
                state->screen = SCREEN_PLAYER;
                return true; // signal: play track
            default:
                break;
        }
    }

    if (down & KEY_B) {
        switch (state->screen) {
            case SCREEN_ALBUMS:
                state->screen        = SCREEN_ARTISTS;
                state->scroll_offset = state->selected_artist;
                if (state->scroll_offset > 0) state->scroll_offset--;
                break;
            case SCREEN_TRACKS:
                state->screen        = SCREEN_ALBUMS;
                state->scroll_offset = state->selected_album;
                if (state->scroll_offset > 0) state->scroll_offset--;
                break;
            case SCREEN_PLAYER:
                state->screen        = SCREEN_TRACKS;
                state->scroll_offset = state->selected_track;
                if (state->scroll_offset > 0) state->scroll_offset--;
                // Signal stop but don't block — main loop calls audio_stop()
                // after ui_handle_input returns true
                return true;  // main.c handles the actual stop
            default:
                break;
        }
    }

    return false;
}

void ui_init(void) {
    debug_log("[ENTER] ui_init()");
    // Larger buffer needed because we no longer clear it every frame —
    // the cache holds parsed text objects that reference into this buffer.
    s_tbuf = C2D_TextBufNew(65536);
    fonts_init();
}

void ui_cleanup(void) {
    debug_log("[ENTER] ui_cleanup()");
    cache_clear();
    fonts_cleanup();
    C2D_TextBufDelete(s_tbuf);
}
