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
// Font management - load all system fonts for full Unicode coverage
// ---------------------------------------------------------------------------
#define MAX_FONTS 8
static C2D_Font s_fonts[MAX_FONTS];

// ---------------------------------------------------------------------------
// Text cache - parse/optimize once, draw every frame.
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
// CJK support requires manually adding .bcfnt files to romfs/ and updating fonts_init()


static void fonts_init(void) {
    debug_log("[UI] fonts_init called");
    // Try to load the standard system font first
    debug_log("[UI] Attempting to load system font (CFG_REGION_USA)");
    s_fonts[s_font_count] = C2D_FontLoadSystem(CFG_REGION_USA);
    if (s_fonts[s_font_count]) {
        s_font_count++;
        debug_log("[UI] Successfully loaded system font (CFG_REGION_USA)");
    } else {
        debug_log("[UI] ERROR: Failed to load standard system font (CFG_REGION_USA), trying fallback .bcfnt from romfs");
        // Fallback: load bundled font from romfs
        debug_log("[UI] Attempting to load fallback font from romfs/popjoy.bcfnt");
        C2D_Font fallback = C2D_FontLoad("romfs:/popjoy.bcfnt");
        if (fallback) {
            s_fonts[s_font_count++] = fallback;
            debug_log("[UI] Successfully loaded fallback font from romfs/popjoy.bcfnt");
        } else {
            debug_log("[UI] FATAL: No fonts available at all!");
        }
    }
    // Optionally try to load CJK fonts if you have them bundled as .bcfnt files
    debug_log("[UI] fonts_init complete, total fonts loaded: %d", s_font_count);
}

static void fonts_cleanup(void) {
    debug_log("[UI] fonts_cleanup called");
    for (int i = 0; i < s_font_count; i++) {
        if (s_fonts[i]) {
            C2D_FontFree(s_fonts[i]);
            debug_log("[UI] Freed font %d", i);
        }
        s_fonts[i] = NULL;
    }
    s_font_count = 0;
    debug_log("[UI] fonts_cleanup complete, all fonts freed");
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

            // Use cached text object - parse/optimize only once per unique string
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
        if (idx == selected) {
            draw_rect(0, y - 2, BOT_W, ROW_H, COL_SELECTED);
        }

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

            // Draw album cover if available
            if (state->album_cover.tex) {
                float coverX = TOP_W - 100 - 8; // 100px wide, 8px margin
                float coverY = 40;
                // Album cover dimensions (fixed for now)
                float coverW = 100;
                float coverH = 100;
                C2D_DrawImageAt(state->album_cover, coverX, coverY, 0.5f, NULL, 1.0f, 1.0f);
            } else {
            }

            // Draw text info to the left of the album cover
            draw_text(8, 55,  0.5f,  COL_TEXT, t->title);
            draw_text(8, 78,  0.45f, COL_DIM,  t->artist);
            draw_text(8, 98,  0.42f, COL_DIM,  t->album);
        } else {
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

    switch (state->screen) {
        case SCREEN_ARTISTS: {
            const NaviArtistList *src = &state->artists;
            int display_count = state->artists.count;
            int display_selected = state->selected_artist;
            int display_scroll = state->scroll_offset;
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
                      "Artists",
                      true);
            break;
        }
        case SCREEN_ALBUMS: {
            const NaviAlbumList *src = &state->albums;
            int display_count = state->albums.count;
            int display_selected = state->selected_album;
            int display_scroll = state->scroll_offset;
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
                      "Albums",
                      true);
            break;
        }
        case SCREEN_TRACKS: {
            const NaviTrackList *src = &state->tracks;
            int display_count = state->tracks.count;
            int display_selected = state->selected_track;
            int display_scroll = state->scroll_offset;
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
                      "Tracks",
                      true);
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

}



bool ui_handle_input(UiState *state) {
        hidScanInput();
    u32 down  = hidKeysDown();
    u32 held  = hidKeysHeld();
    static u32 repeat_timer = 0;
    static u32 last_held = 0;

    // Key repeat logic (simple, per frame)
    if (held & (KEY_DUP | KEY_DDOWN)) {
        if (last_held != held) {
            repeat_timer = 0; // reset on new press
        } else {
            repeat_timer++;
        }
        // After initial delay, treat as repeated press
        int interval = 4;
        if (repeat_timer > 90) interval = 2;
        if (repeat_timer == 15 || (repeat_timer > 15 && (repeat_timer % interval == 0))) {
            down |= held & (KEY_DUP | KEY_DDOWN);
        }
    } else {
        repeat_timer = 0;
    }
    last_held = held;

    int *sel = NULL;
    int  max = 0;

    switch (state->screen) {
        case SCREEN_ARTISTS:
            sel = &state->selected_artist; 
            max = state->artists.count; 
            break;
        case SCREEN_ALBUMS:
            sel = &state->selected_album; 
            max = state->albums.count; 
            break;
        case SCREEN_TRACKS:
            sel = &state->selected_track; 
            max = state->tracks.count; 
            break;
        default:
            break;
    }

    // Volume control (works on all screens)
    if (down & KEY_L) {
        float vol = audio_get_volume() - 0.1f;
        audio_set_volume(vol);
        debug_log("[UI] Volume decreased: %.2f", vol);
    }
    if (down & KEY_R) {
        float vol = audio_get_volume() + 0.1f;
        audio_set_volume(vol);
        debug_log("[UI] Volume increased: %.2f", vol);
    }

    // Pause / resume
    if (down & KEY_START) {
        audio_toggle_pause();
        debug_log("[UI] Playback toggled (pause/resume)");
    }
    if (down & KEY_SELECT) {
        audio_stop();
        debug_log("[UI] Playback stopped");
    }

    if (sel && max > 0) {
        int step = 1;
        // Acceleration: After 1.5s (approx 90 frames), increase step to average 2.5 lines per repeat
        if (repeat_timer > 90) {
            step = (repeat_timer % 4 == 0) ? 2 : 3;
        }

        if (down & KEY_DUP) {
            if (*sel > 0) {
                *sel -= step;
                if (*sel < 0) *sel = 0;
                debug_log("[UI] Selected item moved up: %d", *sel);
            }
            if (*sel < state->scroll_offset) {
                state->scroll_offset = *sel;
                debug_log("[UI] Scroll offset updated: %d", state->scroll_offset);
            }
        }
        if (down & KEY_DDOWN) {
            if (*sel < max - 1) {
                *sel += step;
                if (*sel > max - 1) *sel = max - 1;
                debug_log("[UI] Selected item moved down: %d", *sel);
            }
            if (*sel >= state->scroll_offset + VISIBLE_ROWS) {
                state->scroll_offset = *sel - VISIBLE_ROWS + 1;
                debug_log("[UI] Scroll offset updated: %d", state->scroll_offset);
            }
        }
    }

    if (down & KEY_A) {
        debug_log("[UI] A button pressed");
        switch (state->screen) {
            case SCREEN_ARTISTS:
                state->screen       = SCREEN_ALBUMS;
                state->selected_album = 0;
                state->scroll_offset  = 0;
                debug_log("[UI] Transitioning to ALBUMS screen");
                return true; // signal: load albums
            case SCREEN_ALBUMS:
                state->screen        = SCREEN_TRACKS;
                state->selected_track = 0;
                state->scroll_offset  = 0;
                debug_log("[UI] Transitioning to TRACKS screen");
                return true; // signal: load tracks
            case SCREEN_TRACKS:
                state->screen = SCREEN_PLAYER;
                debug_log("[UI] Transitioning to PLAYER screen");
                return true; // signal: play track
            default:
                debug_log("[UI] A button pressed on unknown screen");
                break;
        }
    }

    if (down & KEY_B) {
        debug_log("[UI] B button pressed");
        switch (state->screen) {
            case SCREEN_ALBUMS:
                state->screen        = SCREEN_ARTISTS;
                state->scroll_offset = state->selected_artist;
                if (state->scroll_offset > 0) state->scroll_offset--;
                debug_log("[UI] Transitioning to ARTISTS screen");
                break;
            case SCREEN_TRACKS:
                state->screen        = SCREEN_ALBUMS;
                state->scroll_offset = state->selected_album;
                if (state->scroll_offset > 0) state->scroll_offset--;
                debug_log("[UI] Transitioning to ALBUMS screen");
                break;
            case SCREEN_PLAYER:
                state->screen        = SCREEN_TRACKS;
                state->scroll_offset = state->selected_track;
                if (state->scroll_offset > 0) state->scroll_offset--;
                debug_log("[UI] Transitioning to TRACKS screen");
                // Signal stop but don't block - main loop calls audio_stop()
                // after ui_handle_input returns true
                return true;  // main.c handles the actual stop
            default:
                debug_log("[UI] B button pressed on unknown screen");
                break;
        }
    }

    return false;
}

void ui_init(UiState *state) {
    debug_log("[UI] ui_init called");
    // Larger buffer needed because we no longer clear it every frame -
    // the cache holds parsed text objects that reference into this buffer.
    debug_log("[UI] Creating text buffer with size 65536");
    s_tbuf = C2D_TextBufNew(65536);
    if (!s_tbuf) {
        debug_log("[UI] ERROR: Failed to create text buffer");
    }
    fonts_init();
    // Initialize album cover image
    debug_log("[UI] Initializing album cover image");
    memset(&state->album_cover, 0, sizeof(C2D_Image));
    state->album_cover_sheet = NULL;
    debug_log("[UI] ui_init complete");
}

void ui_cleanup(UiState *state) {
    debug_log("[UI] ui_cleanup called");
    debug_log("[UI] Clearing text cache");
    cache_clear();
    fonts_cleanup();
    // Clean up album cover image
    if (state->album_cover_sheet) {
        debug_log("[UI] Freeing album cover image");
        C2D_SpriteSheetFree(state->album_cover_sheet);
        state->album_cover_sheet = NULL;
        state->album_cover.tex = NULL;
    }
    debug_log("[UI] Deleting text buffer");
    C2D_TextBufDelete(s_tbuf);
    debug_log("[UI] ui_cleanup complete");
}
