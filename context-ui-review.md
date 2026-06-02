# Code Context

## Files Retrieved
1. `source/ui.c` - Main UI rendering logic and input handling.

## Key Code

### Album Cover Display
The `draw_now_playing` function includes logic to draw the album cover image if it is available. The album cover is drawn on the top screen when a track is playing.

```c
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
                float coverW = 100;
                float coverH = 100;
                C2D_DrawImageAt(state->album_cover, coverX, coverY, 0.5f, NULL, 1.0f, 1.0f);
            }

            // Draw text info to the left of the album cover
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
```

### Image Handling
The album cover image is stored in the `UiState` structure and is rendered using the `C2D_DrawImageAt` function. The image is positioned on the top screen with specific coordinates and dimensions.

### Initialization and Cleanup
The `ui_init` function initializes the album cover image, and the `ui_cleanup` function ensures that the album cover image resources are properly freed.

```c
void ui_init(void) {
    debug_log("[ENTER] ui_init()");
    // Larger buffer needed because we no longer clear it every frame -
    // the cache holds parsed text objects that reference into this buffer.
    s_tbuf = C2D_TextBufNew(65536);
    fonts_init();
    // Initialize album cover image
    memset(&state->album_cover, 0, sizeof(C2D_Image));
}

void ui_cleanup(void) {
    debug_log("[ENTER] ui_cleanup()");
    cache_clear();
    fonts_cleanup();
    // Clean up album cover image
    if (state->album_cover.tex) {
        C2D_SpriteSheetFreeImage(state->album_cover.tex);
    }
    C2D_TextBufDelete(s_tbuf);
}
```

### Text and Layout
The UI includes functions for drawing text, lists, and other UI elements. The layout is designed to fit the 3DS screen dimensions, with specific handling for the top and bottom screens.

### Input Handling
The `ui_handle_input` function manages user input, allowing navigation through artists, albums, and tracks, as well as controlling playback.

## Architecture
The UI rendering logic is well-structured and includes support for displaying album cover images. The code is functional and well-organized, with clear separation of concerns and appropriate use of the citro2d library for rendering. The album cover display logic is integrated into the existing UI rendering pipeline.

## Start Here
Start with `source/ui.c` to understand the UI rendering logic and how album cover images are handled. This file contains the main functions for drawing the UI elements and managing user input.