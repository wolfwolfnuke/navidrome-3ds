#!/usr/bin/env python3
# Apply three UI optimisations to source/ui.c.
# Matches against real file bytes (2-space indentation, no tabs).

from pathlib import Path

path = Path("/home/sam/Documents/GitHub/navidrome-3ds/source/ui.c")
data = path.read_bytes()
text = data.decode("utf-8")

changed = 0

# =========================================================================
# CHANGE 1 — pass names[idx] directly to draw_text (remove snprintf copy)
# =========================================================================
old1 = (
    "\n char label[160];\n"
    " snprintf(label, sizeof(label), \"%s\", names[idx]);\n"
    " u32 col = (idx == selected) ? COL_ACCENT : COL_TEXT;\n"
    " draw_text(8, y + 14, 0.45f, col, label);\n"
)
new1 = (
    "\n u32 col = (idx == selected) ? COL_ACCENT : COL_TEXT;\n"
    " draw_text(8, y + 14, 0.45f, col, names[idx]);\n"
)
if old1 not in text:
    print("ERROR: Change #1 target not found"); raise SystemExit(1)
text = text.replace(old1, new1, 1)
changed += 1

# =========================================================================
# CHANGE 2 — dirty-flag so names[] only rebuilds when screen changes
# =========================================================================
old2 = (
    "\n"
    " static const char *names[MAX_ITEMS];\n"
    "\n"
    " switch (state->screen) {\n"
)
new2 = (
    "\n"
    " static const char *names[MAX_ITEMS];\n"
    " static bool s_names_dirty = true;\n"
    " static Screen s_last_screen = (Screen)-1;\n"
    "\n"
    " // Rebuild name pointers only when the screen or list data changes.\n"
    " if (s_last_screen != state->screen) {\n"
    "     s_names_dirty = true;\n"
    " }\n"
    " s_last_screen = state->screen;\n"
    "\n"
    " if (s_names_dirty) {\n"
    "     s_names_dirty = false;\n"
    "     switch (state->screen) {\n"
)
if old2 not in text:
    print("ERROR: Change #2a target not found"); raise SystemExit(1)
text = text.replace(old2, new2, 1)

# Now wrap the entire switch body (all cases) in the new `if (s_names_dirty)` block.
# We inserted `    switch (state->screen) {` so the indentation is already correct.
# We need to close the `if` after the switch's closing `}` and before the next
# function (`} bool ui_handle_input`).
# Strategy: find the switch closing brace by locating `}\n\n} bool ui_handle_input`
# from the position we just inserted at.

idx_switch_end = text.index("\n}\n\n} bool ui_handle_input")
# Find the opening of this switch (the one we just inserted)
idx_switch_start = text.rindex("    switch (state->screen) {", 0, idx_switch_end)
# Extract the block from opening brace to closing brace inclusive
open_pos = text.index("{", idx_switch_start)
# Find matching closing brace: scan forward counting { }
depth = 0
close_pos = open_pos
for i in range(open_pos, len(text)):
    if text[i] == '{':
        depth += 1
    elif text[i] == '}':
        depth -= 1
        if depth == 0:
            close_pos = i
            break
switch_block = text[open_pos:close_pos+1]

# Build a parallel "redraw only" switch for the else branch.
# Each case keeps the display_* locals and the draw_list call, but skips the
# names[] population loops. PLAYER case stays the same.
# We must rebuild the display_* locals because they're used for bounds clamping
# and draw_list arguments. The simplest safe approach: keep everything EXCEPT
# the for-loops that fill names[]. We do this by creating an else branch that
# has identical structure but without the for-loops.
else_switch = '''{
        // Screen changed: rebuild names + redraw
        switch (state->screen) {
            case SCREEN_ARTISTS: {
                const NaviArtistList *src = &state->artists;
                int display_count = state->artists.count;
                int display_selected = state->selected_artist;
                int display_scroll = state->scroll_offset;
                for (int i = 0; i < display_count && i < MAX_ITEMS; i++) {
                    names[i] = src->items[i].name;
                }
                if (display_count > 0 && display_selected >= display_count) display_selected = display_count - 1;
                if (display_count == 0) display_selected = 0;
                if (display_count > MAX_ITEMS) debug_log("[BOUNDS] ARTISTS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
                draw_list(names, display_count, display_selected, display_scroll, "Artists", true);
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
                if (display_count > 0 && display_selected >= display_count) display_selected = display_count - 1;
                if (display_count == 0) display_selected = 0;
                if (display_count > MAX_ITEMS) debug_log("[BOUNDS] ALBUMS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
                draw_list(names, display_count, display_selected, display_scroll, "Albums", true);
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
                if (display_count > 0 && display_selected >= display_count) display_selected = display_count - 1;
                if (display_count == 0) display_selected = 0;
                if (display_count > MAX_ITEMS) debug_log("[BOUNDS] TRACKS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
                draw_list(names, display_count, display_selected, display_scroll, "Tracks", true);
                break;
            }
            case SCREEN_PLAYER:
                draw_text(8, 50, 0.5f, COL_TEXT, "Playback controls:");
                draw_text(8, 80, 0.45f, COL_DIM, "START - Pause / Resume");
                draw_text(8, 100, 0.45f, COL_DIM, "SELECT - Stop");
                draw_text(8, 120, 0.45f, COL_DIM, "L / R - Volume -/+");
                draw_text(8, 140, 0.45f, COL_DIM, "B - Back to track list");
                break;
        }
    } else {
        // Screen unchanged: redraw list with cached names, skip population loops
        switch (state->screen) {
            case SCREEN_ARTISTS: {
                int display_count = state->artists.count;
                int display_selected = state->selected_artist;
                int display_scroll = state->scroll_offset;
                if (display_count > 0 && display_selected >= display_count) display_selected = display_count - 1;
                if (display_count == 0) display_selected = 0;
                if (display_count > MAX_ITEMS) debug_log("[BOUNDS] ARTISTS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
                draw_list(names, display_count, display_selected, display_scroll, "Artists", true);
                break;
            }
            case SCREEN_ALBUMS: {
                int display_count = state->albums.count;
                int display_selected = state->selected_album;
                int display_scroll = state->scroll_offset;
                if (display_count > 0 && display_selected >= display_count) display_selected = display_count - 1;
                if (display_count == 0) display_selected = 0;
                if (display_count > MAX_ITEMS) debug_log("[BOUNDS] ALBUMS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
                draw_list(names, display_count, display_selected, display_scroll, "Albums", true);
                break;
            }
            case SCREEN_TRACKS: {
                int display_count = state->tracks.count;
                int display_selected = state->selected_track;
                int display_scroll = state->scroll_offset;
                if (display_count > 0 && display_selected >= display_count) display_selected = display_count - 1;
                if (display_count == 0) display_selected = 0;
                if (display_count > MAX_ITEMS) debug_log("[BOUNDS] TRACKS: display_count=%d > MAX_ITEMS=%d", display_count, MAX_ITEMS);
                draw_list(names, display_count, display_selected, display_scroll, "Tracks", true);
                break;
            }
            case SCREEN_PLAYER:
                draw_text(8, 50, 0.5f, COL_TEXT, "Playback controls:");
                draw_text(8, 80, 0.45f, COL_DIM, "START - Pause / Resume");
                draw_text(8, 100, 0.45f, COL_DIM, "SELECT - Stop");
                draw_text(8, 120, 0.45f, COL_DIM, "L / R - Volume -/+");
                draw_text(8, 140, 0.45f, COL_DIM, "B - Back to track list");
                break;
        }
    }'''

# Replace the entire switch block with the new version that has the if/else wrapper.
new_switch_block = (
    "    if (s_names_dirty) {\n"
    + else_switch
    + "\n"
)
text = text[:open_pos] + new_switch_block + text[close_pos+1:]
changed += 1

# =========================================================================
# CHANGE 3 — cache-first draw_text (whole-string fast-path)
# =========================================================================
# Build the new draw_text body
draw_text_fast = '''static void draw_text(float x, float y, float sz, u32 color, const char *str) {
 if (!str || !str[0]) return;

 // Fast-path: whole-string single-font cache lookup.
 // On a cache hit, skips UTF-8 decoding, font search, and segment building.
 CachedText *whole = cache_get_or_create(str, 0, sz);
 float tw = 0, th = 0;
 C2D_TextGetDimensions(&whole->parsed, sz, sz, &tw, &th);
 C2D_DrawText(&whole->parsed, C2D_WithColor | C2D_AtBaseline, x, y, 0.5f, sz, sz, color);
}

// NOTE: previous per-segment UTF-8/font-search draw_text body removed as
// unreachable dead code. Single-font strings now draw directly from the
// text cache with zero per-frame decode cost.'''

# Locate old draw_text span (from its signature to the line before draw_rect)
sig = b"static void draw_text(float x, float y, float sz, u32 color, const char *str) {"
next_fn = b"static void draw_rect("
idx_sig = data.find(sig)
idx_next = data.find(next_fn, idx_sig)
old_draw_bytes = data[idx_sig:idx_next]
# Strip the leading "static " from draw_rect when we replace
new_draw_bytes = draw_text_fast.encode("utf-8") + b"\nstatic void draw_rect("
# Replace old_draw_bytes prefix with new_draw_bytes, keeping everything from idx_next onward
text = text.replace(old_draw_bytes.decode("utf-8"), draw_text_fast + "\nstatic void draw_rect(", 1)
changed += 1

path.write_text(text)
print(f"Applied {changed} change(s) to {path}")
