#pragma once
#include "api.h"
#include <citro2d.h>

typedef enum {
    SCREEN_ARTISTS,
    SCREEN_ALBUMS,
    SCREEN_TRACKS,
    SCREEN_PLAYER,
    SCREEN_SEARCH,
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

    // Search — unified across all screens
    char  search_query[128];
    int   filter_fields;    // bitmask: bit0=name/title, bit1=artist, bit2=album
    int   search_selected;  // index into filtered results on SCREEN_SEARCH
    int   search_scroll;    // scroll offset for search results list
    int   search_prev_screen; // which screen we navigated from (for B-back)

    bool  loading;
    char  status_msg[128];
} UiState;

void ui_search_toggle_field(UiState *state, int field_idx);

void ui_init(void);
void ui_cleanup(void);

// Draw both screens. Call once per frame inside the citro2d begin/end block.
void ui_draw(const UiState *state, C3D_RenderTarget *top, C3D_RenderTarget *bottom);

// Returns true if input caused a state change (caller should re-query data)
bool ui_handle_input(UiState *state);
