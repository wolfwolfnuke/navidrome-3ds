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
    // Always load the standard font first
    s_fonts[s_font_count] = C2D_FontLoadSystem(CFG_REGION_USA);
    if (s_fonts[s_font_count]) s_font_count++;
    else debug_log("[UI] ERROR: Failed to load standard system font (CFG_REGION_USA)");
    debug_log("[UI] Loaded standard font, count=%d", s_font_count);

    // Load CJK fonts
    for (size_t i = 1; i < sizeof(s_font_regions)/sizeof(s_font_regions[0]); i++) {
        C2D_Font f = C2D_FontLoadSystem(s_font_regions[i]);
        if (f && s_font_count < MAX_FONTS) {
            s_fonts[s_font_count++] = f;
            debug_log("[UI] Loaded CJK font region %d, count=%d", (int)i, s_font_count);
        }
    }
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
// ---------------------------------------------------------------------------
static void draw_text(float x, float y, float sz, u32 color, const char *str) {
    debug_log("[UI] draw_text called: x=%.2f, y=%.2f, sz=%.2f, color=0x%08X, str=%.32s", x, y, sz, color, str ? str : "(null)");
    if (!str || !str[0]) {
        debug_log("[UI] draw_text: empty or null string");
        return;
    }

    // Build a segment list: split the string into runs, each rendered
    // with the first font that contains the glyph.
    // For simplicity, render character by character using the right font.
    // We accumulate runs of characters that share the same font.

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
        int found_font = 0; // default to font 0
        for (int fi = 0; fi < s_font_count; fi++) {
            if (s_fonts[fi] && C2D_FontGlyphIndexFromCodePoint(s_fonts[fi], cp) != 0) {
                found_font = fi;
                break;
            }
        }

        // If font changed or buffer full, flush current segment
        if ((found_font != seg_font || seg_len + seq_len >= (int)sizeof(seg) - 1) && seg_len > 0) {
            seg[seg_len] = '\0';
            C2D_Text txt;
            C2D_TextBufClear(s_tbuf);
            if (s_fonts[seg_font])
                C2D_TextFontParse(&txt, s_fonts[seg_font], s_tbuf, seg);
            else
                C2D_TextParse(&txt, s_tbuf, seg);
            C2D_TextOptimize(&txt);

            // Measure width to advance cur_x
            float tw = 0, th = 0;
            C2D_TextGetDimensions(&txt, sz, sz, &tw, &th);
            C2D_DrawText(&txt, C2D_WithColor | C2D_AtBaseline,
                         cur_x, y, 0.5f, sz, sz, color);
            debug_log("[UI] draw_text: drew segment '%s' with font %d at x=%.2f", seg, seg_font, cur_x);
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
        C2D_Text txt;
        C2D_TextBufClear(s_tbuf);
        if (s_fonts[seg_font])
            C2D_TextFontParse(&txt, s_fonts[seg_font], s_tbuf, seg);
        else
            C2D_TextParse(&txt, s_tbuf, seg);
        C2D_TextOptimize(&txt);
        C2D_DrawText(&txt, C2D_WithColor | C2D_AtBaseline,
                     cur_x, y, 0.5f, sz, sz, color);
        debug_log("[UI] draw_text: drew final segment '%s' with font %d at x=%.2f", seg, seg_font, cur_x);
    }
    debug_log("[UI] draw_text complete");
}

static void draw_rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.0f, w, h, color);
}

// Draw a scrollable list on the bottom screen
static void draw_list(const char **names, int count,
                       int selected, int scroll, const char *title) {
    // Header bar
    draw_rect(0, 0, BOT_W, 30, COL_HEADER_BG);
    draw_text(8, 20, 0.55f, COL_ACCENT, title);

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
// Search helpers
// ---------------------------------------------------------------------------

static int strcasestr_simple(const char *haystack, const char *needle) {
    if (!needle[0]) return 1;
    for (const char *h = haystack; *h; h++) {
        int i = 0;
        while (needle[i] && h[i] && (tolower(h[i]) == tolower(needle[i]))) i++;
        if (!needle[i]) return 1;
    }
    return 0;
}

static void filter_artists(const NaviArtistList *src, NaviArtistList *dst, const char *query) {
    dst->count = 0;
    for (int i = 0; i < src->count; i++) {
        if (strcasestr_simple(src->items[i].name, query) || strcasestr_simple(src->items[i].artist, query)) {
            dst->items[dst->count++] = src->items[i];
        }
    }
}

static void filter_albums(const NaviAlbumList *src, NaviAlbumList *dst, const char *query) {
    dst->count = 0;
    for (int i = 0; i < src->count; i++) {
        if (strcasestr_simple(src->items[i].name, query) || strcasestr_simple(src->items[i].artist, query)) {
            dst->items[dst->count++] = src->items[i];
        }
    }
}

static void filter_tracks(const NaviTrackList *src, NaviTrackList *dst, const char *query) {
    dst->count = 0;
    for (int i = 0; i < src->count; i++) {
        if (strcasestr_simple(src->items[i].title, query) || strcasestr_simple(src->items[i].artist, query) || strcasestr_simple(src->items[i].album, query)) {
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

    // Draw search bar if active
    if (state->search_active) {
        draw_rect(0, 0, BOT_W, 30, COL_HEADER_BG);
        char bar[80];
        snprintf(bar, sizeof(bar), "Search: %s", state->search_query);
        draw_text(8, 20, 0.55f, COL_ACCENT, bar);
    }
    switch (state->screen) {
        case SCREEN_ARTISTS: {
            NaviArtistList filtered;
            const NaviArtistList *src = &state->artists;
            if (state->search_active && state->search_type == 0 && state->search_query[0]) {
                filter_artists(&state->artists, &filtered, state->search_query);
                src = &filtered;
            }
            for (int i = 0; i < src->count; i++)
                names[i] = src->items[i].name;
            draw_list(names, src->count,
                      state->selected_artist, state->scroll_offset, state->search_active ? "Artists (Search)" : "Artists");
            break;
        }
        case SCREEN_ALBUMS: {
            NaviAlbumList filtered;
            const NaviAlbumList *src = &state->albums;
            if (state->search_active && state->search_type == 1 && state->search_query[0]) {
                filter_albums(&state->albums, &filtered, state->search_query);
                src = &filtered;
            }
            for (int i = 0; i < src->count; i++)
                names[i] = src->items[i].name;
            draw_list(names, src->count,
                      state->selected_album, state->scroll_offset, state->search_active ? "Albums (Search)" : "Albums");
            break;
        }
        case SCREEN_TRACKS: {
            NaviTrackList filtered;
            const NaviTrackList *src = &state->tracks;
            if (state->search_active && state->search_type == 2 && state->search_query[0]) {
                filter_tracks(&state->tracks, &filtered, state->search_query);
                src = &filtered;
            }
            for (int i = 0; i < src->count; i++)
                names[i] = src->items[i].title;
            draw_list(names, src->count,
                      state->selected_track, state->scroll_offset, state->search_active ? "Tracks (Search)" : "Tracks");
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

// --- Search input helpers ---
void ui_search_activate(UiState *state, int type) {
    state->search_active = 1;
    state->search_type = type;
    state->search_query[0] = '\0';
    state->scroll_offset = 0;
}
void ui_search_deactivate(UiState *state) {
    state->search_active = 0;
    state->search_query[0] = '\0';
    state->scroll_offset = 0;
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

    // --- Search input mode ---
    if (state->search_active) {
        // Accept text input (A-Z, 0-9, space, backspace, etc.)
        // For demo: use X to exit search, Y to clear, A to apply, B to backspace
        if (down & KEY_X) { ui_search_deactivate(state); return false; }
        if (down & KEY_Y) { ui_search_clear(state); return false; }
        if (down & KEY_A) { ui_search_apply(state); return false; }
        if (down & KEY_B) { ui_search_backspace(state); return false; }
        // Use 3DS software keyboard for input
        SwkbdState swkbd;
        char kbdout[64] = {0};
        swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, sizeof(kbdout)-1);
        swkbdSetHintText(&swkbd, "Enter search query");
        if (swkbdInputText(&swkbd, kbdout, sizeof(kbdout)) == SWKBD_BUTTON_CONFIRM) {
            strncpy(state->search_query, kbdout, sizeof(state->search_query)-1);
            state->search_query[sizeof(state->search_query)-1] = '\0';
            state->scroll_offset = 0;
        }
        ui_search_deactivate(state);
        return false;
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
        // Start search: L = artist, R = album, START = track
        if (down & KEY_L) { ui_search_activate(state, 0); return false; }
        if (down & KEY_R) { ui_search_activate(state, 1); return false; }
        if (down & KEY_START) { ui_search_activate(state, 2); return false; }
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
    s_tbuf = C2D_TextBufNew(8192);
    fonts_init();
}

void ui_cleanup(void) {
    debug_log("[ENTER] ui_cleanup()");
    debug_log("[ENTER] ui_cleanup()");
    fonts_cleanup();
    C2D_TextBufDelete(s_tbuf);
}
