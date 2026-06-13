# Navidrome 3DS — Agent Guide

A native **homebrew music client** for the Nintendo 3DS, written in C. Connects to a Navidrome server via the Subsonic REST API, browses Artists → Albums → Tracks, and streams MP3 audio through the 3DS DSP in a background worker thread.

**Stack:** devkitARM toolchain | libctru | citro2d + citro3d | libcurl (API) + httpc (audio download) + mbedTLS | dr_mp3.h (single-header MP3 decoder) | NDSP (hardware DMA audio)

---

## Build

### Prerequisites
```bash
sudo ./install-devkitpro-pacman
dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls

# dr_mp3.h and stb_image.h are NOT in git — manually fetch:
curl -o source/dr_mp3.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
curl -o source/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
```

### Build & Deploy
```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
make clean && make                # produces navidrome.3dsx + navidrome.smdh
cp navidrome.3dsx /3ds/navidrome/ # onto SD card
```

### Link Quirks (Makefile line 50)
`-lmbedcrypto` appears **twice**; `-lctru` appears **twice** — both resolve circular dependency quirks in the devkitPro SDK. Link order:

```
-lcitro2d -lcitro3d -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lctru -lmbedcrypto -lctru -lm
```

Rules include `$(DEVKITARM)/3ds_rules`. `romfs/` is embedded at build time via `--romfs=$(CURDIR)/romfs`; runtime path: `romfs:/path`.

---

## Init Order (main.c) — Strict, Do Not Reorder

```
 1. gfxInitDefault()
 2. romfsInit()
 3. debug_init()                    — FSUSER debug logger
 4. C3D_Init() → C2D_Init() → C2D_Prepare()
 5. C2D_CreateScreenTarget() × 2   — top (400×240) + bottom (320×240)
 6. socInit(buf, 0x100000)          — 1MB page-aligned buffer
 7. httpcInit(0)
 8. ndspInit()                      — may fail (audio becomes unavailable)
 9. calloc(1, sizeof(UiState))      — ~96KB, heap (NOT stack!)
10. ui_init()                       — text buffer + fonts
11. config_load()                   — sdmc:/3ds/navidrome/config.ini
12. api_init()                      — curl_global_init + base URL
13. audio_init()                    — NDSP + linearAlloc PCM buffers
14. api_ping()                      — verify server reachable
15. api_get_artists()               — initial data load
```

**Shutdown** (reverse): `audio_stop`/`audio_cleanup` → `ndspExit` → `api_cleanup` → `ui_cleanup` → `free(state)` → `debug_cleanup` → `socExit`/`httpcExit`/`romfsExit` → `C2D_Fini`/`C3D_Fini` → `gfxExit`.

---

## Critical Gotchas

### socInit Buffer
- Exactly `0x100000` (1MB), **page-aligned** (`__attribute__((aligned(0x1000)))`).
- Smaller size returns error `0xE0A01835`.

### Audio Thread Stack — MUST be 1MB
- dr_mp3's internal scratch buffers (`grbuf[2][576]`, `syn[18+15][2*32]`, `maindata ~1.5KB`) exceed 512KB on 48kHz stereo. Confirmed by crash dump — SP corrupted into app BSS.
- Stack is explicitly `malloc`'d before `threadCreate` so a **canary** (`16 × 0xDEADBEEF` at bottom) can be planted. Checked in `audio_stop()` after thread exits.

### ndspChnReset(0) Location — CRITICAL
- **MUST be called in `audio_stop()`, NOT in the audio thread.**
- Calling it in the thread fires a DSP interrupt that can execute **after `threadFree()` frees the stack** → execution on freed memory.
- Thread calls `ndspSetCallback(NULL, NULL)` to suppress future callbacks, then signals `LightEvent`. `audio_stop()` waits on the event, calls `ndspChnReset(0)` (stack still alive), then `threadJoin`/`threadFree`.

### audio_stop() Synchronization Sequence
```
1. s_stop_req = true
2. httpcCloseContext(s_ctx)   ← unblocks stuck download
3. LightEvent_Wait(&s_exit_event)
4. check_stack_canary()
5. ndspChnReset(0)            ← stack still alive
6. threadJoin / threadFree
7. free(s_thread_stack)
```
`audio_stop()` is idempotent (checks `!s_thread` early-return).

### httpcDownloadData Return Code
- `0xD840A02B` = "download pending" — **NOT an error**, `continue` the loop.
- `0x00000000` (HTTPC_STATUS_DOWNLOAD_READY) = done.

### Download Buffer — Fixed 5MB, Never Realloc
- `malloc(MAX_DL_SIZE)` before `drmp3_init_memory()`. dr_mp3 stores an **internal pointer** to this buffer — reallocating invalidates it.

### UiState — MUST Be Heap-Allocated
- ~96KB (3 × 1000-item lists). Stack allocation crashes the main thread (~128KB stack).

### URL Buffers ≥ 2048 Bytes
- `build_url()` produces `http://host:port/rest/endpoint?u=user&p=pass&v=1.16.1&c=Navidrome3DS&f=xml{extra}`. Long hostnames/passwords push past 1024 bytes.

### consoleInit vs citro2d
- They conflict. Debug output goes exclusively to file (`debug_log()` → `sdmc:/3ds/navidrome/debug.log` via raw FSUSER). No console output.

### APT Sleep Hook
- `aptSetSleepAllowed(false)` while `audio_is_playing()` — prevents sleep during playback.
- Hook for `APTHOOK_ONSLEEP`/`APTHOOK_ONEXIT` calls `audio_stop()`.

### Always Call audio_stop() Before Reloading Tracks
Pattern in main.c's action dispatch:
```c
case SCREEN_TRACKS:
    audio_stop();
    // ... load tracks ...
    break;
```

### Bounds Checking
All list-filling loops must check `i < MAX_ITEMS` (1000). `xml_attr()` does not enforce buffer sizes — truncation may silently occur if attribute values exceed `MAX_NAME_LEN` (64) or `MAX_ID_LEN` (32).

### Font Loading
1. `C2D_FontLoadSystem(CFG_REGION_USA)` — may return NULL (missing archive, custom firmware).
2. Fallback: `C2D_FontLoad("romfs:/popjoy.bcfnt")` bundled in romfs.
3. Only one system font loaded; CJK glyphs depend on the font having them.

### Text Cache (Performance)
- `C2D_TextFontParse()` + `C2D_TextOptimize()` are expensive per-frame operations. Fix: `CachedText` cache (256 entries, LRU eviction) in `ui.c`.
- `s_tbuf` = 65536 bytes (not 8192). **Never call `C2D_TextBufClear()` per frame** — cache holds references into this buffer.

### Debug Logger
- Uses **raw FSUSER** (`FSFILE_Write`), not `fopen` — `fopen` was unreliable on this firmware.
- Path: `sdmc:/3ds/navidrome/debug.log`, fallback `sdmc:/debug.log`.
- Thread-safe (LightLock) — called from both main and audio threads.

### Config Storage
- Uses **`fopen()`** (not FSUSER) — this path works fine for config.
- Primary: `sdmc:/3ds/navidrome/config.ini`, alt: `/3ds/navidrome/config.ini`.
- If missing, writes defaults and exits after 3s error screen.

### Album Art
- Fetched via `api_get_album_cover()` (`getCoverArt` endpoint), loaded into `C2D_SpriteSheet` from memory.
- Triggered when navigating to SCREEN_TRACKS. Displayed in the top-screen now-playing panel.

### Large Stack Locals in Main Thread
Main thread stack is ~128KB. In `ui_draw()`, the `names[]` pointer array and `s_last_names_screen` are **static** (BSS), not local — Bug 3 in crash history was a ~100KB stack overflow from local filtered lists.

---

## Key Architecture Facts

| File | Role |
|---|---|
| `source/main.c` | Entry point, strict 15-step init, main loop, cleanup |
| `source/api.c/h` | Subsonic REST over libcurl, hand-rolled XML parser (`xml_attr()`), dynamic `Buffer` (4KB start, realloc-doubles) |
| `source/audio.c/h` | httpc download → dr_mp3 decode → NDSP ping-pong (2 × 4096 samples), 1MB thread stack, canary |
| `source/ui.c/h` | citro2d rendering, Screen FSM, text cache, album cover display |
| `source/config.c/h` | INI parser/writer via `fopen()` |
| `source/debug.c/h` | FSUSER-based logging |

**Note:** `audio_init()` setup (NDSP, PCM buffers) happens before `api_ping()` in init order. `audio_play_url()` is called on demand from main loop.

---

## Constants

| Constant | Value | Defined In |
|---|---|---|
| `MAX_ITEMS` | 1000 | api.h |
| `MAX_NAME_LEN` | 64 | api.h |
| `MAX_ID_LEN` | 32 | api.h |
| `MAX_DL_SIZE` | 5MB | audio.c |
| `AUDIO_THREAD_STACK_SIZE` | 1MB | audio.c |
| `BUF_SAMPLES` | 4096 | audio.c |
| `SAMPLE_RATE` | 44100 | audio.c |
| `SOC_BUFSIZE` | 0x100000 | main.c |
| `s_tbuf` size | 65536 | ui.c |
| `MAX_CACHED_TEXT` | 256 | ui.c |
| `URL buffer size` | 2048 | api.c |
| `CONFIG_PATH` | `sdmc:/3ds/navidrome/config.ini` | config.h |

---

## Controls

| Button | Action |
|---|---|
| D-Pad Up/Down | Navigate list (repeat: 15-frame delay, then 4-frame interval) |
| A | Select / Enter screen |
| B | Back |
| START | Pause / Resume |
| SELECT | Stop playback |
| L / R | Volume -/+ (0.1 steps, clamped [0, 1]) |

---

## Crash History (5 bugs fixed)

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Crash on 2nd song switch | `ndspChnReset(0)` in thread → DSP interrupt on freed memory | Moved to `audio_stop()`, thread calls `ndspSetCallback(NULL,NULL)` |
| 2 | SP corrupted into BSS | 512KB stack too small for dr_mp3 on 48kHz stereo | 1MB stack, explicit malloc, canary |
| 3 | Data abort in `ui_draw()` | Local ~100KB filtered lists overflowed ~128KB main thread stack | Made lists `static` |
| 4 | Black screen | `C2D_FontLoadSystem()` returned NULL | Fallback to `romfs:/popjoy.bcfnt` |
| 5 | Single-digit FPS | Text re-parse/optimize every frame | Text cache (256 entries, LRU) |

Details in `HANDOVER.md`.

---

## Known Limitations

- **MAX_ITEMS = 1000** — beyond this, the static arrays overflow. Network response could exceed this silently.
- **MP3 only** — dr_mp3 decodes MP3; Navidrome serves MP3 via stream endpoint.
- **Fixed 5MB download buffer** — songs larger than 5MB won't play (duration depends on bitrate; ~5 min at 128kbps).
- **Single track playback** — no queue or next-track. Must go back to track list.
- **128kbps max** — hardcoded in `api_stream_url()`.
