#pragma once
#include "api.h"
#include <citro2d.h>

typedef enum {
    SCREEN_ARTISTS,
    SCREEN_ALBUMS,
    SCREEN_TRACKS,
    SCREEN_PLAYER,
} Screen;

typedef struct {
    Screen screen;

    NaviArtistList artists;
    NaviAlbumList  albums;
    NaviTrackList  tracks;

    int   selected_artist;
    int   selected_album;
    int   selected_track;
    int   scroll_offset;

    bool  loading;
    char  status_msg[128];
} UiState;

void ui_init(void);
void ui_cleanup(void);

// Draw both screens. Call once per frame inside the citro2d begin/end block.
void ui_draw(const UiState *state, C3D_RenderTarget *top, C3D_RenderTarget *bottom);

// Returns true if input caused a state change (caller should re-query data)
bool ui_handle_input(UiState *state);
