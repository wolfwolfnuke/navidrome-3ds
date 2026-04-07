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
  very last action; audio_stop() waits on it before calling ndspChnReset
  and threadJoin, guaranteeing no use-after-free at any point

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

4. **ndspChnReset(0) must be called from audio_stop(), not the audio thread.**
   Calling it inside the thread fires a DSP interrupt that can still be
   pending when audio_stop() calls threadFree(). threadFree() releases the
   thread stack, so the interrupt then executes on freed memory and corrupts
   state. The fix is: the thread calls ndspSetCallback(NULL, NULL) to silence
   future interrupts, signals s_exit_event, and returns. audio_stop() then
   calls ndspChnReset(0) while the stack is still alive, and only calls
   threadFree() after that. See the "Song-Switch Crash" section below.

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

10. **ndspSetCallback(NULL, NULL)** must be called by the audio thread
    before it signals s_exit_event, to prevent future DSP callbacks from
    firing after cleanup begins. Note: ndspSetCallback takes 2 arguments
    (callback, data pointer), not 3.

11. **API URL buffer must be ≥ 2048 bytes** — `build_url` constructs
    `http://host:port/rest/endpoint?u=user&p=pass&v=...&c=...&f=xml`
    which can exceed 1024 bytes with long hostnames/passwords. All URL
    buffers in api.c are 2048 to avoid truncation warnings.

---

## Song-Switch Crash — Root Cause & Fix

### Symptom
Playing a song works. Pressing B to go back and selecting a new song
crashes the 3DS. The debug log ends with:

```
audio_stop: start
audio: thread exit
(no "audio_stop: done" line — crash occurs before it is reached)
```

### Root Cause
`ndspChnReset(0)` fires a DSP interrupt via the ndsp interrupt handler.
The previous code called `ndspChnReset` inside the audio thread as the
last cleanup step, immediately before `LightEvent_Signal`. The sequence
was:

```
Thread:      ndspChnReset(0)        ← schedules DSP interrupt
Thread:      LightEvent_Signal
audio_stop:  LightEvent_Wait returns
audio_stop:  threadJoin             ← thread proc has returned, join is instant
audio_stop:  threadFree(s_thread)   ← FREES THE THREAD STACK
DSP IRQ:     fires on freed stack   ← memory corruption / crash
```

`ndspSetCallback(NULL, NULL)` prevents *future* callbacks from being
registered, but does not cancel a DSP interrupt that has already been
queued by `ndspChnReset`. So the interrupt handler fired on the freed
stack regardless.

### Fix
Move `ndspChnReset(0)` from the audio thread into `audio_stop()`, called
**after** `LightEvent_Wait` confirms the thread has fully exited but
**before** `threadFree`. The thread only calls `ndspSetCallback(NULL, NULL)`
to prevent further callbacks, then signals the event and returns.

Correct sequence:

```
Thread:      ndspSetCallback(NULL, NULL)   ← silence future callbacks
Thread:      drmp3_uninit / free           ← clean up decode state
Thread:      LightEvent_Signal             ← "I am done with all memory"
audio_stop:  LightEvent_Wait returns
audio_stop:  ndspChnReset(0)              ← reset now; stack still alive
                                           ← any DSP interrupt fires here,
                                              stack is valid
audio_stop:  threadJoin
audio_stop:  threadFree(s_thread)         ← safe: no more IRQs pending
```

The decode loop still checks `wb->status == NDSP_WBUF_FREE` to detect
an externally-issued channel reset (e.g. if audio_stop is called while
the loop is sleeping), so it exits cleanly in all cases.

### Cleanup Order (confirmed correct)
`drmp3_uninit(mp3)` is called while `dl_buf` is still allocated.
The `done` block's second `drmp3_uninit` only fires when `mp3 == NULL`
(error path where init never succeeded), so there is no double-free.

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
- Song switching (song-switch crash fixed — see above)

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
