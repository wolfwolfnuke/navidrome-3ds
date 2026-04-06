# Navidrome 3DS Client — Handover Document

## Project Summary

A native homebrew music client for the New Nintendo 3DS written in C.
It connects to a Navidrome server using the Subsonic REST API, browses
artists/albums/tracks, and streams/plays music via the 3DS DSP hardware.

Built with devkitPro (devkitARM), libctru, citro2d/citro3d, libcurl (for
API calls), httpc (3DS native HTTP, for audio download), and dr_mp3.h
(single-header MP3 decoder).

---

## File Structure

```
navidrome-3ds/
├── Makefile
├── romfs/
│   └── config.ini         # default config, user edits on SD card
└── source/
    ├── main.c             # entry point, main loop, init/cleanup
    ├── config.c/h         # reads/writes sdmc:/3ds/navidrome/config.ini
    ├── api.c/h            # Subsonic REST API (libcurl, XML parsing)
    ├── audio.c/h          # httpc download + dr_mp3 decode + ndsp playback
    ├── ui.c/h             # citro2d rendering, input handling
    ├── debug.c/h          # logs to sdmc:/3ds/navidrome/debug.log via FSUSER
    └── dr_mp3.h           # single-header MP3 decoder (user must supply)
```

---

## Architecture

### main.c
- Initialises everything in order: gfx → citro2d → romfs → socInit (1MB
  page-aligned global buffer) → httpcInit → ndspInit → config → api → audio
- UiState is heap-allocated (calloc) to avoid stack overflow
- Main loop: ui_handle_input() → action dispatch → render
- On SCREEN_TRACKS action: always calls audio_stop() then reloads tracks
- On SCREEN_PLAYER action: calls audio_play_url()
- On B from SCREEN_PLAYER: ui returns true, main.c calls audio_stop()

### api.c
- Uses libcurl for all Subsonic API calls (ping, getArtists, getArtist,
  getAlbum, stream URL construction)
- Minimal hand-rolled XML attribute parser — handles named and numeric
  XML entities (&#34; etc.)
- MAX_ITEMS=200, MAX_NAME_LEN=64, MAX_ID_LEN=32

### audio.c
- Downloads full MP3 into a fixed MAX_DL_SIZE (5MB) malloc buffer —
  never realloc'd because drmp3_init_memory() stores an internal pointer
  to the buffer that must remain stable
- Uses httpcContext* stored in a global (protected by LightLock) so
  audio_stop() can call httpcCloseContext() to unblock httpcDownloadData
- Decode loop uses ndspWaveBuf ping-pong (2 buffers, 4096 samples each)
- Thread stack: 512KB (dr_mp3 has large internal decode frames)
- Synchronisation: LightEvent s_exit_event — thread signals it as its
  very last action; audio_stop() waits on it before threadJoin to
  guarantee no use-after-free between thread cleanup and next song start

### ui.c
- citro2d for all rendering
- Loads system fonts via C2D_FontLoadSystem for CJK support
  (Japanese, Simplified Chinese, Traditional Chinese, Korean)
- UTF-8 aware draw_text: decodes codepoints, finds first font with glyph,
  renders in segments
- Top screen: now-playing info (title/artist/album, play state, volume bar)
- Bottom screen: scrollable list (Artists → Albums → Tracks → Player)

### debug.c
- Uses raw FSUSER API (not fopen) to write to SD card — fopen was
  unreliable on this firmware
- Writes to sdmc:/3ds/navidrome/debug.log, falls back to sdmc:/debug.log

---

## Key Hardware/API Gotchas Discovered

1. **socInit requires exactly 0x100000 bytes, page-aligned (0x1000)**
   — smaller sizes return error 0xE0A01835

2. **httpcDownloadData returns 0xD840A02B** ("download pending") which
   is NOT a fatal error — must continue the loop, not break

3. **drmp3_init_memory stores a pointer** to your buffer internally.
   Never realloc the download buffer after calling it.

4. **ndspChnReset(0) from audio_stop** sets wave buffer status to
   NDSP_WBUF_FREE — the decode loop checks for this to exit cleanly.
   Do NOT manually set s_wave_bufs[i].status from outside the thread.

5. **Thread stack**: 512KB needed. dr_mp3 has large stack frames in its
   decode functions. Earlier attempts with 64KB/128KB all stack-overflowed
   (crash signature: SP=0x07c00000, R4=0x54555555 "TUUU" uninitialized).

6. **UiState must be heap-allocated** — it contains three lists of 200
   items each (~96KB total). Stack allocation causes immediate crash.

7. **consoleInit conflicts with citro2d** — cannot use both. Debug output
   goes to file only in the full UI build.

8. **httpcCloseContext from audio_stop** unblocks a stuck
   httpcDownloadData call in the audio thread immediately.

9. **LightEvent synchronisation** is required between audio_stop() and
   the thread — threadJoin alone is not sufficient because the ndsp DSP
   interrupt handler can fire after join returns and corrupt memory.

---

## Current Status

### Working
- App launches, reads config from SD card
- Connects to Navidrome server, lists 200 artists
- Navigate: Artists → Albums → Tracks → Player
- Full song download and playback (MP3 at 48kHz stereo)
- Top screen shows track title/artist/album and play state
- CJK character rendering (Japanese/Chinese/Korean via system fonts)
- XML entity decoding (&#34; → ", &amp; → &, etc.)
- Volume control (L/R buttons)
- Pause/resume (START button)
- Debug logging to SD card

### Current Issue — Crash on Song Switch

**Symptom**: Playing a song works. Pressing B to go back and selecting
a new song crashes the 3DS.

**What the log shows**:
```
audio_stop: signalling
audio: playback loop ended (stop=1 frames=N)
  *garbage characters here*
audio: thread exit
```

**Root cause being investigated**: Memory corruption occurs during
drmp3_uninit(mp3) or free(mp3) inside the audio thread, apparently
caused by a race between the thread's cleanup and audio_stop()'s
ndspChnReset(0) triggering the ndsp DSP interrupt handler.

**Latest fix attempt** (not yet confirmed working): Added a LightEvent
(s_exit_event) that the thread signals as its absolute last action.
audio_stop() now calls LightEvent_Wait(&s_exit_event) before
threadJoin, guaranteeing the thread has finished all memory operations
before audio_stop returns and the next song starts.

**The sequence in the latest code**:
1. audio_stop: s_stop_req = true
2. audio_stop: httpcCloseContext(s_ctx) → unblocks download
3. audio_stop: ndspChnReset(0) → sets wave bufs to NDSP_WBUF_FREE
4. Thread: sees NDSP_WBUF_FREE, exits decode loop
5. Thread: drmp3_uninit(mp3), free(mp3), free(dl_buf)
6. Thread: s_playing = false
7. Thread: debug_log("audio: thread exit")
8. Thread: LightEvent_Signal(&s_exit_event)  ← NEW
9. audio_stop: LightEvent_Wait returns        ← NEW
10. audio_stop: threadJoin (formality)
11. audio_stop: returns, new song can start

**If this still crashes**, the next things to investigate:
- Whether ndspChnReset in step 3 is firing a DSP callback that
  runs concurrently with step 5
- Whether the ndsp interrupt handler needs to be disabled during cleanup
  (ndspSetCallback(NULL, NULL) before reset)
- Whether dl_buf needs to outlive drmp3_uninit (drmp3 may access the
  buffer during uninit, not just during decode)

---

## Build Instructions

```bash
# Install devkitPro with 3DS support
dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls

# Drop dr_mp3.h into source/ from:
# https://github.com/mackron/dr_libs

# Build
make clean && make

# Deploy
cp navidrome.3dsx /path/to/sd/3ds/navidrome/
# Also ensure sdmc:/3ds/navidrome/config.ini exists:
# [server]
# host=192.168.x.x
# port=4533
# username=youruser
# password=yourpass
```

## Makefile Key Settings
```makefile
include $(DEVKITARM)/3ds_rules   # note: DEVKITARM not DEVKITPRO
LIBS := -lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 \
        -lmbedcrypto -lmbedcrypto -lctru -lz -lm
# Note: -lmbedcrypto twice and -lctru last resolves circular deps
```
