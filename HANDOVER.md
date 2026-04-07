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
- Thread stack: 1MB (confirmed necessary by crash dump analysis — see below)
- Stack is explicitly malloc'd before threadCreate so a canary pattern can
  be planted at the bottom and checked after the thread exits
- Synchronisation: LightEvent s_exit_event — thread signals it as its
  very last action; audio_stop() waits on it, checks the canary, calls
  ndspChnReset(0), then threadJoin/threadFree

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
   state. The fix: the thread calls ndspSetCallback(NULL, NULL) to silence
   future interrupts, signals s_exit_event, and returns. audio_stop() then
   calls ndspChnReset(0) while the stack is still alive, and only calls
   threadFree() after that.

5. **Thread stack must be 1MB, not 512KB.** Confirmed by crash dump analysis
   (see "Song-Switch Crash" section below). dr_mp3 uses large internal scratch
   buffers that collectively exceed 512KB on 48kHz stereo tracks during decode.

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
    firing after cleanup begins. ndspSetCallback takes 2 arguments
    (callback, data pointer).

11. **API URL buffer must be ≥ 2048 bytes** — `build_url` constructs
    `http://host:port/rest/endpoint?u=user&p=pass&v=...&c=...&f=xml`
    which can exceed 1024 bytes with long hostnames/passwords. All URL
    buffers in api.c are 2048 to avoid truncation warnings.

---

## Song-Switch Crash — Full History

### Bug 1: ndspChnReset race (FIXED, v1)

**Symptom**: Crash on second song. Log ended with `audio: thread exit`
but no `audio_stop: done`.

**Root cause**: `ndspChnReset(0)` was called inside the audio thread as
its final step before `LightEvent_Signal`. The DSP interrupt it schedules
could still be pending when `audio_stop()` called `threadFree()`, which
freed the thread stack. The interrupt then executed on freed memory.

**Fix**: Moved `ndspChnReset(0)` out of the thread and into `audio_stop()`,
called after `LightEvent_Wait` confirms the thread has fully exited. The
thread only calls `ndspSetCallback(NULL, NULL)` to suppress future callbacks.

---

### Bug 2: Audio thread stack overflow (FIXED, v2)

**Symptom**: Crash on song switch (sometimes first play). Log:
```
audio: HTTP 200
audio: downloaded 2732430 bytes
audio: 48000Hz 2ch
audio_stop: start
audio: thread exit
(crash — no audio_stop: done)
```

**Crash dump analysis** (`crash_dump_00000016.dmp`):

| Register | Value | Meaning |
|----------|-------|---------|
| R9 | 0x07C00000 | Audio thread stack base — this IS the overflow marker |
| SP | 0x001BA4F8 | Corrupted: points into app BSS (s_url global visible in dump) |
| PC | 0x00000805 | Corrupted: near-null from stack overwriting globals then vectors |

The dump's memory section contained `s_url` verbatim (the HTTP stream URL),
confirming the stack had grown downward past its base (0x07C00000), through
any guard region, and into the app's BSS segment, overwriting global variables.

The instruction pattern `0xEE777A87` (repeated MCR coprocessor instruction,
which is how `DSP_FlushDataCache` is implemented on ARM11) appeared in the
dump, confirming the crash occurred during the decode→flush loop.

**Root cause**: dr_mp3 uses multiple large internal structures during decode
(`drmp3dec_scratch` contains `grbuf[2][576]`, `syn[18+15][2*32]`, and a
`maindata` buffer of ~1.5KB). Together with the callstack depth during VBR
frame decoding on a 48kHz stereo track, these exceed 512KB. The stack
allocated by the previous `threadCreate(..., 512*1024, ...)` was too small.

**Fix**:
- Increased `AUDIO_THREAD_STACK_SIZE` from 512KB to 1MB.
- Stack is now explicitly `malloc`'d before `threadCreate` so a canary
  pattern (`0xDEADBEEF`) can be written to the bottom 64 bytes before the
  thread starts, and checked by `audio_stop()` after the thread exits.
  If the canary is overwritten, a warning is logged before any crash occurs.

**Cleanup order** (confirmed correct, documented for reference):
`drmp3_uninit(mp3)` is called while `dl_buf` is still allocated. The `done`
block's second `drmp3_uninit` only fires on the error path where `mp3 != NULL`
(init succeeded but something else failed), so there is no double-free.

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
- Song switching (both crash bugs fixed)
- Stack canary check on every audio_stop() — will warn in log if stack
  approaches its limit before it causes a silent crash

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
