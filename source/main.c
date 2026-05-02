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
    debug_log("[ENTER] main()");
    debug_log("Initializing graphics...");
    gfxInitDefault();

    debug_log("Initializing ROMFS...");
    romfsInit();
    debug_log("ROMFS initialized");
    debug_init();
    debug_log("Boot OK");

    debug_log("Initializing citro3d/citro2d...");
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    debug_log("citro2d OK");

    debug_log("Creating render targets...");
    debug_log("[DBG] mem free before targets: %lu KB", (unsigned long)osGetMemRegionFree(MEMREGION_ALL) / 1024);
    debug_log("[DBG] linear free before targets: %lu KB", (unsigned long)linearSpaceFree() / 1024);
    C3D_RenderTarget *top    = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    debug_log("[DBG] after top target alloc");
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    debug_log("[DBG] after bottom target alloc");
    debug_log("[DBG] top target: %p, bottom target: %p", top, bottom);
    debug_log("[DBG] mem free after targets: %lu KB", (unsigned long)osGetMemRegionFree(MEMREGION_ALL) / 1024);
    debug_log("[DBG] linear free after targets: %lu KB", (unsigned long)linearSpaceFree() / 1024);
    if (!top || !bottom) {
        debug_log("FATAL: Failed to create screen targets: top=%p bottom=%p", top, bottom);
        return 1;
    }

    debug_log("Initializing networking...");
    Result rc = socInit(soc_buf, SOC_BUFSIZE);
    debug_log("socInit: %s", R_SUCCEEDED(rc) ? "OK" : "FAIL");
    httpcInit(0);
    debug_log("httpcInit OK");

    debug_log("Initializing audio subsystem...");
    rc = ndspInit();
    bool audio_available = R_SUCCEEDED(rc);
    debug_log("ndspInit: %s", audio_available ? "OK" : "FAIL");
    if (!audio_available) {
        debug_log("WARNING: DSP unavailable, running in no-audio mode");
    }

    debug_log("Allocating UI state...");
    UiState *state = (UiState *)calloc(1, sizeof(UiState));
    if (!state) { debug_log("FATAL: UiState allocation failed"); gfxExit(); return 1; }
    debug_log("UiState allocation OK: %p", state);
    state->screen = SCREEN_ARTISTS;
    debug_log("Initializing UI...");
    ui_init();

    debug_log("Loading config...");
    NaviConfig cfg;
    debug_log("Before config_load: state=%p", state);
    if (config_load(&cfg) != 0) {
        debug_log("Config load failed, using defaults and saving...");
        config_defaults(&cfg);
        config_save(&cfg);
        set_status(state, "Config missing or unreadable. Check /3ds/navidrome/config.ini");
        debug_log("FATAL: Config missing or unreadable. Exiting.");
        // Show error for a few seconds, then exit
        for (int i = 0; i < 180; ++i) { // ~3 seconds at 60fps
            debug_log("Rendering error screen, frame %d", i);
            C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
            C2D_TargetClear(top,    C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
            C2D_TargetClear(bottom, C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
            ui_draw(state, top, bottom);
            C3D_FrameEnd(0);
            svcSleepThread(16666666LL); // ~1/60s
        }
        debug_log("Cleaning up after config error...");
        debug_cleanup();
        socExit();
        httpcExit();
        romfsExit();
        C2D_Fini();
        C3D_Fini();
        gfxExit();
        debug_log("Exiting due to config error");
        return 1;
    }
    debug_log("Server: %s:%d", cfg.host, cfg.port);

    debug_log("Initializing API and audio...");
    debug_log("Before api_init: state=%p", state);
    api_init(&cfg);
    if (audio_available) {
        debug_log("Calling audio_init...");
        int ainit = audio_init();
        debug_log("audio_init: %d", ainit);
        if (ainit != 0) {
            debug_log("audio_init failed, disabling audio");
            audio_available = false;
        }
    } else {
        set_status(state, "Audio unavailable (DSP not present)");
    }

    debug_log("Showing connecting screen...");
    set_status(state, "Connecting to server...");
    debug_log("[DBG] About to C3D_FrameBegin");
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    debug_log("[DBG] After C3D_FrameBegin");
    debug_log("[DBG] About to C2D_TargetClear top");
    C2D_TargetClear(top,    C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
    debug_log("[DBG] After C2D_TargetClear top");
    debug_log("[DBG] About to C2D_TargetClear bottom");
    C2D_TargetClear(bottom, C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
    debug_log("[DBG] After C2D_TargetClear bottom");
    debug_log("[DBG] About to ui_draw");
    ui_draw(state, top, bottom);
    debug_log("[DBG] After ui_draw");
    debug_log("[DBG] About to C3D_FrameEnd");
    C3D_FrameEnd(0);
    debug_log("[AFTER] C3D_FrameEnd connecting screen");

    debug_log("Pinging server...");
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

    debug_log("Entering main loop...");
    while (aptMainLoop()) {
        debug_log("Main loop iteration");
        bool action = ui_handle_input(state);
        debug_log("ui_handle_input returned: %d", action);

        if (action) {
            debug_log("Action detected, screen=%d", state->screen);
            switch (state->screen) {
                case SCREEN_ALBUMS: {
                    state->loading = true;
                    const char *id = state->artists.items[state->selected_artist].id;
                    debug_log("Loading albums for artist: %s", id);
                    if (api_get_albums(id, &state->albums) != 0) {
                        debug_log("Failed to load albums for artist: %s", id);
                        set_status(state, "Failed to load albums.");
                    } else {
                        debug_log("Albums loaded for artist: %s", id);
                        state->status_msg[0] = '\0';
                    }
                    state->loading = false;
                    break;
                }
                case SCREEN_TRACKS: {
                    debug_log("Navigating to tracks screen, stopping audio");
                    audio_stop();
                    state->loading = true;
                    const char *id = state->albums.items[state->selected_album].id;
                    debug_log("Loading tracks for album: %s", id);
                    if (api_get_tracks(id, &state->tracks) != 0) {
                        debug_log("Failed to load tracks for album: %s", id);
                        set_status(state, "Failed to load tracks.");
                    } else {
                        debug_log("Tracks loaded for album: %s", id);
                        state->status_msg[0] = '\0';
                    }
                    state->loading = false;
                    break;
                }
                case SCREEN_PLAYER: {
                    debug_log("Navigating to player screen");
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
                            debug_log("Playback failed for url: %s", url);
                            set_status(state, "Playback failed.");
                        } else {
                            debug_log("Playback started OK");
                            state->status_msg[0] = '\0';
                        }
                    }
                    break;
                }
                default:
                    debug_log("Unknown screen state: %d", state->screen);
                    break;
            }
        }

        debug_log("Rendering frame");
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_TargetClear(top,    C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
        C2D_TargetClear(bottom, C2D_Color32(0x1a, 0x1a, 0x2e, 0xFF));
        ui_draw(state, top, bottom);
        C3D_FrameEnd(0);
    }

    debug_log("Main loop exited, starting cleanup...");
    debug_log("Shutting down");
    if (audio_available) {
        debug_log("Stopping audio and cleaning up audio subsystem");
        audio_stop();
        audio_cleanup();
        ndspExit();
    }
    debug_log("Cleaning up API...");
    api_cleanup();
    debug_log("Cleaning up UI...");
    ui_cleanup();
    debug_log("Freeing UI state...");
    free(state);
    debug_log("Cleaning up debug log...");
    debug_cleanup();

    debug_log("Exiting networking and system services...");
    socExit();
    httpcExit();
    romfsExit();
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    debug_log("[EXIT] main()");
    return 0;
}
