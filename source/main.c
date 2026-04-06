/*
 * main.c — Navidrome 3DS Client
 */

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "config.h"
#include "api.h"
#include "audio.h"
#include "ui.h"
#include "debug.h"

// socInit needs 0x100000 minimum — this is a hard requirement, can't reduce
#define SOC_BUFSIZE 0x100000
static u32 soc_buf[SOC_BUFSIZE / 4] __attribute__((aligned(0x1000)));

static void set_status(UiState *s, const char *msg) {
    strncpy(s->status_msg, msg, sizeof(s->status_msg) - 1);
}

int main(void) {
    gfxInitDefault();

    // romfs first so sdmc: is ready for logging
    romfsInit();
    debug_init();
    debug_log("Boot OK");

    // ---- citro3d / citro2d ----
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    debug_log("citro2d OK");

    // ---- Render targets (created once) ----
    C3D_RenderTarget *top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    // ---- Networking ----
    Result rc = socInit(soc_buf, SOC_BUFSIZE);
    debug_log("socInit: %s", R_SUCCEEDED(rc) ? "OK" : "FAIL");
    httpcInit(0);
    debug_log("httpcInit OK");

    // ---- Audio ----
    rc = ndspInit();
    bool audio_available = R_SUCCEEDED(rc);
    debug_log("ndspInit: %s", audio_available ? "OK" : "FAIL");

    // ---- UI state (heap allocated) ----
    UiState *state = (UiState *)calloc(1, sizeof(UiState));
    if (!state) { gfxExit(); return 1; }
    state->screen = SCREEN_ARTISTS;
    ui_init();

    // ---- Config ----
    NaviConfig cfg;
    if (config_load(&cfg) != 0) {
        config_defaults(&cfg);
        config_save(&cfg);
        set_status(state, "No config. Edit /3ds/navidrome/config.ini");
    }
    debug_log("Server: %s:%d", cfg.host, cfg.port);

    // ---- API + Audio ----
    api_init(&cfg);
    if (audio_available) {
        int ainit = audio_init();
        debug_log("audio_init: %d", ainit);
        if (ainit != 0) audio_available = false;
    }

    // ---- Show connecting screen ----
    set_status(state, "Connecting to server...");
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TargetClear(top,    C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
    C2D_TargetClear(bottom, C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
    ui_draw(state, top, bottom);
    C3D_FrameEnd(0);

    // ---- Ping + initial artist load ----
    if (api_ping() != 0) {
        debug_log("Ping failed");
        set_status(state, "Cannot reach server! Check config.ini");
    } else {
        debug_log("Ping OK, loading artists...");
        state->loading = true;
        if (api_get_artists(&state->artists) == 0) {
            debug_log("Artists loaded: %d", state->artists.count);
            // Log raw bytes of first 5 artists to debug encoding
            for (int i = 0; i < state->artists.count && i < 5; i++) {
                const char *name = state->artists.items[i].name;
                char hexbuf[128] = {0};
                for (int j = 0; name[j] && j < 12; j++) {
                    char tmp[8];
                    snprintf(tmp, sizeof(tmp), "%02X ", (unsigned char)name[j]);
                    strcat(hexbuf, tmp);
                }
                debug_log("Artist[%d]: '%s' bytes: %s", i, name, hexbuf);
            }
            state->loading       = false;
            state->status_msg[0] = '\0';
        } else {
            debug_log("Artist load failed");
            state->loading = false;
            set_status(state, "Failed to load artists.");
        }
    }

    // ---- Main loop ----
    while (aptMainLoop()) {
        bool action = ui_handle_input(state);

        if (action) {
            switch (state->screen) {
                case SCREEN_ALBUMS: {
                    state->loading = true;
                    const char *id = state->artists.items[state->selected_artist].id;
                    debug_log("Loading albums for artist: %s", id);
                    if (api_get_albums(id, &state->albums) != 0)
                        set_status(state, "Failed to load albums.");
                    else
                        state->status_msg[0] = '\0';
                    state->loading = false;
                    break;
                }
                case SCREEN_TRACKS: {
                    // Always stop audio when navigating to tracks screen
                    audio_stop();
                    // Always reload tracks for the selected album
                    state->loading = true;
                    const char *id = state->albums.items[state->selected_album].id;
                    debug_log("Loading tracks for album: %s", id);
                    if (api_get_tracks(id, &state->tracks) != 0)
                        set_status(state, "Failed to load tracks.");
                    else
                        state->status_msg[0] = '\0';
                    state->loading = false;
                    break;
                }
                case SCREEN_PLAYER: {
                    const char *id = state->tracks.items[state->selected_track].id;
                    char url[512];
                    api_stream_url(id, url, sizeof(url));
                    debug_log("Playing: %s", url);
                    debug_log("audio_available: %d", audio_available);
                    debug_log("mem free: %lu KB", (unsigned long)osGetMemRegionFree(MEMREGION_ALL) / 1024);
                    debug_log("linear free: %lu KB", (unsigned long)linearSpaceFree() / 1024);
                    if (!audio_available) {
                        debug_log("Audio unavailable");
                        set_status(state, "Audio unavailable.");
                    } else {
                        int play_rc = audio_play_url(url);
                        debug_log("audio_play_url rc: %d", play_rc);
                        if (play_rc != 0) {
                            set_status(state, "Playback failed.");
                        } else {
                            debug_log("Playback started OK");
                            state->status_msg[0] = '\0';
                        }
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // ---- Render ----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top,    C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
        C2D_TargetClear(bottom, C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
        ui_draw(state, top, bottom);
        C3D_FrameEnd(0);
    }

    // ---- Cleanup ----
    debug_log("Shutting down");
    if (audio_available) { audio_stop(); audio_cleanup(); ndspExit(); }
    api_cleanup();
    ui_cleanup();
    free(state);
    debug_cleanup();

    socExit();
    httpcExit();
    romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}
