# Navidrome 3DS — Agent Guide

> Complete reference for human developers and AI coding agents.
> Read this file first. It supersedes README.md and HANDOVER.md for
> implementation questions; those remain useful for user-facing docs.

---

## 1. What is This Project

A native **homebrew music client** for the Nintendo 3DS (New 3DS preferred)
written in C. It connects to a local **Navidrome** server via the Subsonic
REST API, browses a hierarchy of **Artists → Albums → Tracks**, and streams
MP3 audio through the 3DS DSP hardware in a background worker thread.

| Aspect | Detail |
|---|---|
| **Language** | C11 (with `-D__3DS__ -DARM11`) |
| **Toolchain** | devkitARM / devkitPro |
| **Target** | Nintendo 3DS (ARM11, dual screens, NDSP audio) |
| **Output** | `navidrome.3dsx` + `navidrome.smdh` |
| **Network** | libcurl (API) + httpc (audio download) + mbedTLS (HTTPS) |
| **UI** | citro2d (both screens), system fonts + fallback |
| **Audio** | dr_mp3.h (single-header MP3 decoder) + NDSP (hardware DMA) |
| **Storage** | SD card (`sdmc:`) for config + debug log |
| **External deps** | dr_mp3.h must be manually fetched (see §6) |

---

## 2. File Layout

```
navidrome-3ds/
├── Makefile                    # Build system (devkitARM 3DS rules)
├── README.md                   # End-user documentation
├── HANDOVER.md                 # Crash history & gotchas (kept for reference)
├── install-devkitpro-pacman    # Helper script to install devkitPro apt repo
├── .devcontainer/devcontainer.json
├── .github/copilot-instructions.md
├── .gitignore
├── romfs/
│   ├── config.ini              # Default config shipped with app
│   └── popjoy.bcfnt            # Fallback bitmap font (system fonts missing)
├── source/
│   ├── main.c                  # Entry point, init order, main loop, cleanup
│   ├── config.c / config.h     # Config INI parser / writer (sdmc: FSUSER)
│   ├── api.c / api.h           # Subsonic REST API (libcurl, XML parser)
│   ├── audio.c / audio.h       # MP3 download + decode + NDSP playback
│   ├── ui.c / ui.h             # citro2d rendering, input
│   ├── debug.c / debug.h       # FSUSER-based debug logging to SD
│   └── dr_mp3.h                # Single-header MP3 decoder (external)
├── build/                      # Generated (objects, ELF, maps)
├── navidrome.3dsx              # Built executable
└── navidrome.smdh              # 3DS menu metadata icon
```

---

## 3. Build System

### Prerequisites

```bash
# Install devkitPro-pacman (requires sudo)
sudo ./install-devkitpro-pacman

# Install 3DS toolchain
dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls 3ds-citro2d 3ds-citro3d

# Fetch the MP3 decoder (NOT bundled in git)
curl -o source/dr_mp3.h \
  https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
```

### Build

```bash
export DEVKITPRO=/opt/devkitpro          # or wherever devkitPro lives
export DEVKITARM=$DEVKITPRO/devkitARM
make clean && make
```

Output: `navidrome.3dsx` (dropped into `build/` by the rules, then moved to
project root by the Makefile's `$(OUTPUT).3dsx` target).

### Key Makefile Details

- **Rules include**: `$(DEVKITARM)/3ds_rules` (not `3dsx_rules`)
- **Arch flags**: `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mword-relocations -fomit-frame-pointer`
- **Optimization**: `-O2` with frame pointer omitted
- **Link order is deliberate and matters**:
  ```
  -lcitro2d -lcitro3d -lcurl \
    -lmbedtls -lmbedx509 -lmbedcrypto \
    -lz -lctru -lmbedcrypto -lm              # -lmbedcrypto appears twice; -lctru last
  ```
  The duplicate `-lmbedcrypto` and trailing `-lctru` resolve circular
  dependency quirks in the devkitPro 3DS SDK.
- **ROMFS**: `romfs/` directory is embedded into the `.3dsx` at build time
  via `--romfs=$(CURDIR)/romfs` flag. At runtime, files are accessed via `romfs:/path`.
- **SMDH**: Generated from `navidrome.png` using `smdhtool`:
```bash
smdhtool -c navidrome.smdh -i navidrome.png -s "Navidrome 3DS" -l "Navidrome 3DS Client" -p "Sam" -v "1.0.0"
```

---

## 4. Initialization Order (main.c)

The init sequence is strict — each step depends on the previous one completing
successfully. Never reorder.

```
1. gfxInitDefault()              — graphics (both screens)
2. romfsInit()                   — ROMFS (bundled assets)
3. debug_init()                  — FSUSER debug logger (fd open)
4. C3D_Init(C3D_DEFAULT_CMDBUF_SIZE) — citro3d
5. C2D_Init(C2D_DEFAULT_MAX_OBJECTS) — citro2d
6. C2D_Prepare()                 — citro2d prepare
7. C2D_CreateScreenTarget(GFX_TOP, ...)     — top screen target (~800KB)
8. C2D_CreateScreenTarget(GFX_BOTTOM, ...)  — bottom screen target (~600KB)
9. socInit(soc_buf, 0x100000)    — networking (exactly 1MB, page-aligned)
10. httpcInit(0)                 — 3DS native HTTP client
11. ndspInit()                   — DSP audio subsystem
12. calloc(1, sizeof(UiState))  — heap-allocated UI state (~96KB)
13. ui_init()                    — citro2d buffers + fonts
14. config_load()                — read server credentials from SD card
15. api_init(&cfg)               — curl global init + build base URL
16. audio_init()                 — NDSP setup + linearAlloc PCM buffers
17. api_ping()                   — verify server connectivity
18. api_get_artists()            — initial data load (artists list)
```

**Shutdown order** (reverse of init, minus data loading):
```
aptSetSleepAllowed(true)          → re-enable sleep
audio_stop()                      → wait for audio thread to exit
audio_cleanup()                   → free PCM buffers, thread stack
ndspExit()
api_cleanup()                     → curl_global_cleanup()
ui_cleanup()                      → font/cache cleanup, citro2d/3d deinit
free(state)                       → UiState heap allocation
debug_cleanup()                   → close log file, fsExit()
socExit() → httpcExit() → romfsExit() → C2D_Fini() → C3D_Fini() → gfxExit()
```

---

## 5. Subsystem Reference

### 5.1 api.c — Subsonic REST API

**Protocol**: Subsonic API 1.16.1 via HTTP GET, response in XML.

**Endpoint summary:**

| Function | Subsonic Endpoint | Params | Returns |
|---|---|---|---|
| `api_ping()` | `ping` | — | `"status=\"ok\""` |
| `api_get_artists()` | `getArtists` | — | List of `<artist id="" name="" />` |
| `api_get_albums(id)` | `getArtist` | `&id=...` | `<album ...>` inside `<artist>` |
| `api_get_tracks(id)` | `getAlbum` | `&id=...` | `<song ...>` |
| `api_stream_url(id)` | `stream` | `&id=...&format=mp3&maxBitRate=128` | Full stream URL |

**URL construction:**
```
http://{host}:{port}/rest/{endpoint}?u={user}&p={pass}&v=1.16.1&c=Navidrome3DS&f=xml{extra}
```

**URL buffers are 2048 bytes** — never shrink this. Long hostnames or
passwords push URLs past 1024 bytes.

**XML parsing** is hand-rolled (`xml_attr()` helper). It:
- Searches for `attr="value"` within a given XML tag string
- Decodes named entities: `&amp;`, `&lt;`, `&gt;`, `&apos;`, `&quot;`
- Decodes numeric entities: `&#34;` (decimal) and `&#x20;` (hex)
- Emits UTF-8 output bytes (up to 3-byte sequences)

**HTTP response buffer** is a dynamic `realloc`-based `Buffer` struct
(starts at 4KB, doubles on growth). Freed after each API call.

**Key types:**
```c
#define MAX_ITEMS    1000   // max artists/albums/tracks per query
#define MAX_NAME_LEN 64    // max chars for names/titles
#define MAX_ID_LEN   32    // max chars for Subsonic IDs

typedef struct { char id[MAX_ID_LEN]; char name[MAX_NAME_LEN]; } NaviArtist;
typedef struct { char id[MAX_ID_LEN]; char name[MAX_NAME_LEN];
                 char artist[MAX_NAME_LEN]; int year; int songCount; } NaviAlbum;

**Album Art**: The `api_get_album_cover()` function fetches album art via the Subsonic `getCoverArt` endpoint and stores it in a `C2D_Image` struct.
typedef struct { char id[MAX_ID_LEN]; char title[MAX_NAME_LEN];
                 char artist[MAX_NAME_LEN]; char album[MAX_NAME_LEN];
                 int duration; int track; } NaviTrack;
```

### 5.2 audio.c — MP3 Streaming (Background Thread)

**Architecture:** Single audio worker thread handles download → decode →
playback. Main thread controls it via shared state + `LightEvent`.

**Thread lifecycle:**
```
audio_play_url(url)
  ├─ malloc(1MB stack) for the thread
  ├─ plant_stack_canary() at bottom of stack
  ├─ threadCreate(audio_thread, ..., stack_ptr, ..., 0x31, ...)
  └─ return 0

audio_thread()
  ├─ httpcOpenContext → httpcBeginRequest
  ├─ httpcDownloadData loop (32KB chunks, fixed buffer)
  ├─ drmp3_init_memory(dl_buf, size)
  ├─ ndspChnReset(0) + NDSP format setup
  ├─ ndsp ping-pong buffer loop (2 × 4096 samples)
  ├─ drmp3_read_pcm_frames_s16() per frame
  ├─ DSP_FlushDataCache() on each buffer
  ├─ ndspChnWaveBufAdd(0, wb) to queue
  ├─ natural end OR s_stop_req → loop exits
  ├─ ndspSetCallback(NULL, NULL)  ← suppress future interrupts
  ├─ drmp3_uninit(mp3), free(mp3)
  ├─ httpcCloseContext(ctx), free(ctx)
  ├─ LightEvent_Signal(&s_exit_event)  ← last action
  └─ return

audio_stop()
  ├─ s_stop_req = true
  ├─ httpcCloseContext(s_ctx)  ← unblocks stuck download
  ├─ LightEvent_Wait(&s_exit_event)  ← wait for thread exit
  ├─ check_stack_canary()  ← warn if stack overflowed
  ├─ ndspChnReset(0)  ← safe: thread done, stack still alive
  ├─ threadJoin(s_thread, U64_MAX)
  ├─ threadFree(s_thread)
  ├─ free(s_thread_stack)
  └─ s_playing = false
```

**Critical constraints:**
- **Thread stack: 1MB** (not 512KB). dr_mp3's internal scratch buffers
  (`drmp3dec_scratch`, `grbuf[2][576]`, `syn[18+15][2*32]`, maindata ~1.5KB)
  collectively exceed 512KB on 48kHz stereo tracks. Confirmed by crash dump
  analysis — SP was corrupted into app BSS.
- **Download buffer: 5MB fixed** (`MAX_DL_SIZE`). Never realloc after
  `drmp3_init_memory()` because dr_mp3 stores an internal pointer to it.
- **ndspChnReset(0) must be in audio_stop(), NOT the audio thread.**
  Calling it in the thread fires a DSP interrupt that can execute after
  `threadFree()` releases the stack, corrupting freed memory.
- **`ndspSetCallback(NULL, NULL)` before `LightEvent_Signal`.** Prevents
  future DSP callbacks from firing during cleanup.
- **`httpcCloseContext()` from audio_stop() unblocks a stuck**
  `httpcDownloadData` call. Without this, `LightEvent_Wait` would hang.
- **httpcDownloadData returns `0xD840A02B`** ("download pending") which is
  **NOT an error** — the loop must `continue`, not `break`.
- **Stack canary:** 16 × 0xDEADBEEF words planted at stack bottom before
  thread starts, checked by `audio_stop()` after thread exits. Logs warning
  if corrupted.

**Audio thread globals:**
```c
static volatile bool s_playing;
static volatile bool s_paused;
static volatile bool s_stop_req;
static volatile float s_volume;
static char s_url[1024];
static httpcContext *s_ctx;       // protected by s_ctx_lock
static LightLock s_ctx_lock;
static LightEvent s_exit_event;
```

**PCM buffer:** `linearAlloc(4096 * 2 * 2 bytes) = 16KB` for 2 ping-pong
wave buffers. Uses `linearAlloc` (contiguous linear memory required by NDSP).

### 5.3 ui.c — citro2d Rendering & Input

**Screens** (finite state machine):
```
SCREEN_ARTISTS → A → SCREEN_ALBUMS → A → SCREEN_TRACKS → A → SCREEN_PLAYER
     ↑B               ↑B                ↑B                ↑B
     └────────────────┴─────────────────┴─────────────────┘
```

**Album Art**: The `SCREEN_PLAYER` screen displays album art fetched via `api_get_album_cover()`. The `UiState` struct includes a `C2D_Image album_cover` field for this purpose.

**Screen dimensions:**
- Top: 400 × 240
- Bottom: 320 × 240
- Row height: 22px, visible rows: 8
- List start Y: 40px

**Font loading:**
1. Try `C2D_FontLoadSystem(CFG_REGION_USA)` (Latin + basic)
2. Fallback: `C2D_FontLoad("romfs:/popjoy.bcfnt")` (bundled bitmap font)
3. System font: `CFG_REGION_USA` (Latin + basic) is loaded by default. CJK support requires manually adding `.bcfnt` files (e.g., `CFG_REGION_JPN`, `CFG_REGION_CHN`) to `romfs/` and updating `ui.c` to load them. Falls back to `romfs:/popjoy.bcfnt` if system fonts are unavailable.

**Text cache** (§5.3.1) — see below.

**Input handling:**
- `hidScanInput()` + `hidKeysDown()` + `hidKeysHeld()`
- Key repeat: D-Pad UP/DOWN repeats after 15 frames, then every 4 frames
- Volume: L/R buttons (±0.1, clamped to [0, 1])
- Pause: START button toggles `audio_toggle_pause()`
- Stop: SELECT button calls `audio_stop()`
### 5.3.1 Text Cache (Performance Fix)

**Problem:** `C2D_TextFontParse()` + `C2D_TextOptimize()` are expensive
O(n) operations called every frame. With custom fonts, this caused UI to
crawl (single-digit FPS).

**Solution:** `CachedText` struct caches pre-parsed, pre-optimized `C2D_Text`
objects keyed by `(string, font_idx, scale)`. Parse/optimize happens once;
subsequent frames just draw the cached object.

```c
typedef struct {
    char     str[128];
    int      font_idx;
    float    scale;
    C2D_Text parsed;   // pre-parsed, pre-optimized
} CachedText;

static CachedText s_text_cache[MAX_CACHED_TEXT];  // 256 entries
static int        s_cache_count;
```

**Eviction:** LRU shift-down when cache reaches 256 entries. Oldest entry
is evicted.

**Buffer size:** `s_tbuf` increased from 8192 → 65536 bytes because the
cache holds references into the text buffer — we can no longer clear it
every frame.

### 5.4 config.c — Server Configuration

**Paths:**
- Primary: `sdmc:/3ds/navidrome/config.ini`
- Alt: `/3ds/navidrome/config.ini`

**Format:**
```ini
[server]
host=192.168.1.100
port=4533
username=admin
password=yourpassword
```

**Config defaults** (`config_defaults()`): `192.168.1.100:4533`, user `admin`,
pass `password`.

**Save:** Creates parent directories with `mkdir()` recursively, writes
INI format via `fprintf()`. Falls back to alt path on error.

**Note:** Uses `fopen()` here (not raw FSUSER) — this path works fine for
the 3DS. Only the debug logger needed raw FSUSER due to `fopen`
unreliability in that context.

### 5.5 debug.c — SD Card Logging

**Uses raw FSUSER API** (not `fopen`) — `fopen` was unreliable on this
firmware for file logging.

```c
FSUSER_OpenArchive(ARCHIVE_SDMC, ...)
FSUSER_OpenFile(..., fsMakePath(PATH_ASCII, "/3ds/navidrome/debug.log"),
                FS_OPEN_WRITE | FS_OPEN_CREATE, ...)
FSFILE_Write(..., FS_WRITE_FLUSH)
```

**Fallback path:** `sdmc:/debug.log` if the directory creation fails.

**Locking:** `LightLock` protects concurrent writes (debug_log can be
called from both main thread and audio thread).

---

## 6. External Dependencies

### dr_mp3.h

**NOT included in git.** Must be manually fetched:

```bash
curl -o source/dr_mp3.h \
  https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
```

- Single-header MP3 decoder by David Reid (mackron)
- MIT-0 or Public Domain
- Version ~0.7.4 (based on minimp3)
- Included with `#define DR_MP3_IMPLEMENTATION` before `#include`
- Used exclusively by `audio.c` for MP3 decoding

### devkitPro 3DS SDK

Required packages:
| Package | Provides |
|---|---|
| `3ds-dev` | libctru, lib3ds, toolchain |
| `3ds-curl` | libcurl for HTTP |
| `3ds-mbedtls` | mbedTLS for HTTPS |

Optional (not required but useful):
| Package | Provides |
|---|---|
| `3ds-citro2d` | 2D rendering (usually in 3ds-dev) |
| `3ds-citro3d` | 3D rendering (usually in 3ds-dev) |

---

## 7. Memory Map & Heap Concerns

| Allocation | Size | Location | Notes |
|---|---|---|---|
| `soc_buf` | 1MB | static (aligned 0x1000) | socInit requirement |
| `UiState` | ~96KB | heap (calloc) | 3 × 200-item lists + strings |
| Top screen target | ~800KB | linear | C2D_CreateScreenTarget |
| Bottom screen target | ~600KB | linear | C2D_CreateScreenTarget |
| PCM buffers | 16KB | linearAlloc | 2 × 4096 stereo s16 |
| Audio thread stack | 1MB | heap malloc | Must be explicitly allocated |
| Download buffer | 5MB | heap malloc | Fixed, never realloc |
| s_tbuf | 64KB | heap | citro2d text buffer |
| s_text_cache | ~128KB | static BSS | 256 × (128 + C2D_Text) |

**Total heap pressure:** ~12-15MB during playback. The 3DS has ~128MB
physical RAM but homebrew typically sees less (~64-96MB). This is tight
but works because the 3DS's ARM11 is not memory-bandwidth constrained.

**Critical:** `UiState` **must be heap-allocated** — stack allocation
(~96KB) immediately crashes because the main thread only has ~128KB stack
space.

---

## 8. Critical Gotchas (Do Not Break These)

### 8.1 socInit Buffer
- **Must be exactly 0x100000 bytes (1MB), page-aligned (0x1000)**
- Smaller sizes return error `0xE0A01835`
- Declared as static global: `static u32 soc_buf[SOC_BUFSIZE / 4] __attribute__((aligned(0x1000)))`

### 8.2 Audio Thread Stack
- **1MB minimum** (dr_mp3 scratch buffers exceed 512KB on 48kHz stereo)
- Must be `malloc`'d before `threadCreate` so canary can be planted
- Canary check runs in `audio_stop()` after thread exits

### 8.3 ndspChnReset(0) Location
- **MUST be in `audio_stop()`, NOT in the audio thread**
- Calling it in the thread schedules a DSP interrupt that fires after
  `threadFree()` releases the stack → execution on freed memory
- Thread calls `ndspSetCallback(NULL, NULL)` to suppress future callbacks

### 8.4 Bounds Checking
- All loops filling `names[]` must check `i < MAX_ITEMS` (1000)
- **Note**: `xml_attr()` does not enforce buffer sizes. Truncation may occur if attribute values exceed `MAX_ID_LEN` or `MAX_NAME_LEN`.
- **Bounds checking**: All loops filling `names[]` must check `i < MAX_ITEMS` (1000)
- Out-of-bounds writes corrupt memory and cause data aborts

### 8.5 URL Buffer Sizes
- URL buffers: **≥ 2048 bytes** (Subsonic URLs with credentials can be long)
- Name buffers: 64 bytes (`MAX_NAME_LEN`)
- ID buffers: 32 bytes (`MAX_ID_LEN`)

### 8.6 httpcDownloadData Return Code
- `0xD840A02B` = "download pending" — **NOT an error**, continue loop
- `0x00000000` (HTTPC_STATUS_DOWNLOAD_READY) = download complete

### 8.7 LightEvent Synchronization
- `audio_stop()` must call `LightEvent_Wait(&s_exit_event)` before
  `ndspChnReset(0)` — ensures thread has fully exited and freed
- `ndspChnReset(0)` before `threadFree()` — stack still alive
- `threadFree()` only after `ndspChnReset(0)` completes

### 8.8 Font Loading
- System fonts may not be available (missing archive, custom firmware)
- Always have `popjoy.bcfnt` in romfs as fallback
- Text cache (`s_tbuf`) must be 64KB (not 8KB) because cache holds
  references into the buffer — cannot clear every frame

### 8.9 APT Sleep Hook
- `aptSetSleepAllowed(false)` when music is playing — prevents system sleep
- `aptHook` callback for `APTHOOK_ONSLEEP` and `APTHOOK_ONEXIT` calls
  `audio_stop()` to gracefully stop playback during sleep/exit

### 8.10 Console vs citro2d
- `consoleInit()` and citro2d **cannot coexist** — they conflict
- Debug output goes exclusively to file (`debug_log()`)
- No console output in the UI build

### 8.11 Audio Stop Sequence
Always call `audio_stop()` **before** reloading tracks or changing songs.
The main loop in `main.c` follows this pattern:
```c
case SCREEN_TRACKS:
    audio_stop();         // ← always before data reload
    // ... load tracks ...
    break;
case SCREEN_PLAYER:
    // ... play new track ...
    break;
```

---

## 9. Crash History Summary

Five major bugs were discovered and fixed. The HANDOVER.md documents each
in detail with crash dump analysis. Quick summary:

| # | Symptom | Root Cause | Fix |
|---|---|---|---|
| 1 | Crash on 2nd song switch | `ndspChnReset(0)` in thread → DSP interrupt on freed memory | Moved reset to `audio_stop()`, thread calls `ndspSetCallback(NULL,NULL)` |
| 2 | Stack overflow → SP corrupted to BSS | 512KB thread stack too small for dr_mp3 on 48kHz stereo | Increased to 1MB, explicit malloc + canary |
| 3 | Data abort in `ui_draw()` | Local filtered lists (~100KB) overflow main thread stack | Made lists `static`, added bounds checking (later removed when search was removed)
| 4 | Black screen — no fonts | `C2D_FontLoadSystem()` returns NULL (missing archive) | Fallback to `romfs:/popjoy.bcfnt` |
| 5 | UI crawl (single-digit FPS) | Parse/optimize text every frame with custom font | Text cache with LRU eviction (256 entries) |

---

## 10. Controls

| Button | Action |
|---|---|
| **D-Pad Up/Down** | Navigate list |
| **A** | Select / Enter screen |
| **B** | Back |
| **START** | Pause / Resume |
| **SELECT** | Stop playback |
| **L / R** | Volume down / up |
| **Y** | N/A (reserved) |
| **Touch (bottom)** | Tap list items |

---

## 11. Configuration

Place `/3ds/navidrome/config.ini` on the SD card:

```ini
[server]
host=192.168.1.100
port=4533
username=admin
password=yourpassword
```

The app reads this at startup. If missing, it uses defaults, saves to SD
card, and shows an error screen for ~3 seconds before exiting.

---

## 12. Deployment

```bash
# Build
make clean && make

# Deploy to SD card
cp navidrome.3dsx /path/to/sd/3ds/navidrome/navidrome.3dsx

# Ensure config exists:
echo -e "[server]\nhost=192.168.1.100\nport=4533\nusername=admin\npassword=yourpass" \
  > /path/to/sd/3ds/navidrome/config.ini
```

Launch from the 3DS homebrew launcher (Homebrew Menu / FBI / etc.).

---

## 13. Debugging

**Log file:** `sdmc:/3ds/navidrome/debug.log`

The log contains:
- Every init step with OK/FAIL status
- API calls with URLs and HTTP response codes
- Artist/album/track parsing with hex dumps (for encoding debugging)
- Memory state: heap free before/after allocations
- Audio thread events: download size, sample rate, channels, frame counts
- Stack canary results
- Bounds checking violations
- Font loading results

**When something crashes:** Copy the log from the SD card and look for:
- `FATAL` or `ERROR` lines
- `[BOUNDS]` violations
- `STACK OVERFLOW DETECTED` from canary check
- HTTP error codes (anything other than 200)
- `httpcDownloadData error` with hex code

**Crash dumps:** `crash_dump_*.dmp` files appear on SD card after hard
crashes. Register analysis:
- R9 = thread stack base marker (if present)
- SP = stack pointer (corrupted if overflowed)
- PC = program counter (where crash occurred)
- Memory sections dump show overwritten variables

---

## 14. Modifying the Code

### Adding a new screen
1. Add screen enum value to `UiState` → `ui.h`
2. Add navigation cases in `ui_handle_input()` → `ui.c`
3. Add rendering case in `ui_draw()` → `ui.c`
4. Add data loading case in main loop → `main.c`
5. Add API function if needed → `api.c` / `api.h`

### Adding a new API call
1. Add function declaration to `api.h`
2. Implement in `api.c` using `http_get()` helper
3. Use `build_url()` to construct the endpoint URL
4. Parse XML with `xml_attr()` — be careful with bounds

### Modifying the audio thread
1. **Respect the init/cleanup order** — no new allocations in the thread
   after `drmp3_init_memory()` except what dr_mp3 itself does
2. **Never realloc `dl_buf`** after `drmp3_init_memory()` — dr_mp3 stores
   an internal pointer to it
3. **If adding large locals**, verify the canary still has margin
4. **Always call `ndspSetCallback(NULL, NULL)`** before `LightEvent_Signal`
5. **Never call `ndspChnReset(0)` in the thread** — only in `audio_stop()`

### Modifying the UI
1. **Keep bounds checks** on all list-filling loops
2. **Don't call `C2D_TextBufClear()` every frame** — the cache needs stable references into the buffer
3. **Remember `s_tbuf` is 64KB** — don't shrink it or clear it during rendering

### Adding a new font
1. Place `.bcfnt` file in `romfs/`
2. Add `C2D_FontLoad("romfs:/newfont.bcfnt")` call in `fonts_init()`
3. The text renderer (`draw_text`) already searches all fonts for glyphs

---

## 15. Known Limitations

| Limitation | Reason |
|---|---|
| 200 items per list | `MAX_ITEMS` constant — larger would overflow stack without restructuring |
| MP3 only | dr_mp3 decodes MP3; Navidrome serves MP3 via stream endpoint |
| 5MB download buffer | Fixed allocation; longer songs exceed this and won't play |
| No album art | Subsonic API supports it but not implemented |
| No playlist support | API calls for playlists not implemented |
| Single track playback | No queue/next-track; must navigate back to track list |
| No album art display | Not yet implemented |
| 128kbps max bitrate | Hardcoded in `api_stream_url()` stream endpoint |

---

## 16. Quick Reference — Constants

| Constant | Value | Where |
|---|---|---|
| `SOC_BUFSIZE` | `0x100000` (1MB) | main.c |
| `MAX_ITEMS` | 1000 | api.h |
| `MAX_NAME_LEN` | 64 | api.h |
| `MAX_ID_LEN` | 32 | api.h |
| `API_VERSION` | `"1.16.1"` | api.c |
| `API_CLIENT` | `"Navidrome3DS"` | api.c |
| `SAMPLE_RATE` | 44100 | audio.c |
| `CHANNEL_COUNT` | 2 | audio.c |
| `BUF_SAMPLES` | 4096 | audio.c |
| `NUM_BUFS` | 2 | audio.c |
| `MAX_DL_SIZE` | 5MB | audio.c |
| `AUDIO_THREAD_STACK_SIZE` | 1MB | audio.c |
| `STACK_CANARY_WORDS` | 16 (64 bytes) | audio.c |
| `STACK_CANARY_VALUE` | `0xDEADBEEF` | audio.c |
| `TOP_W` / `TOP_H` | 400 / 240 | ui.c |
| `BOT_W` / `BOT_H` | 320 / 240 | ui.c |
| `ROW_H` | 22 | ui.c |
| `VISIBLE_ROWS` | 8 | ui.c |
| `MAX_FONTS` | 8 | ui.c |
| `MAX_CACHED_TEXT` | 256 | ui.c |
| `s_tbuf size` | 65536 | ui.c (C2D_TextBufNew) |
| `MAX_FONTS` | 8 | ui.c | Maximum number of fonts loaded |
| `MAX_CACHED_TEXT` | 256 | ui.c | Maximum number of cached text objects |
| `MAX_STR` | 256 | config.h | Maximum string length for config values |
| URL buffer size | 2048 | api.c (all URL buffers) |
| `CONFIG_PATH` | `sdmc:/3ds/navidrome/config.ini` | config.h |
| `MAX_STR` | 256 | config.h |

---

## 17. Git Branches

| Branch | Description |
|---|---|
| `main` | Stable release branch |
| `small-worker` | Development branch with latest changes |

---

## 18. Tips for AI Agents

1. **Always read AGENTS.md first** — it contains everything you need to
   understand the architecture, constraints, and gotchas.
2. **Never call `ndspChnReset(0)` in the audio thread** — only in
   `audio_stop()`.
4. **Never shrink URL buffers below 2048 bytes.**
5. **Always check `i < MAX_ITEMS`** in list-filling loops.
6. **Don't reorder the init sequence** in `main.c` — dependencies are strict.
7. **Don't call `C2D_TextBufClear()` every frame** — the text cache needs
   stable references into the buffer.
8. **The download buffer must be 5MB fixed** — never realloc after
   `drmp3_init_memory()`.
9. **httpcDownloadData `0xD840A02B` is "pending", not an error.**
10. **When adding any large stack variables**, consider making them static
    — the main thread only has ~128KB stack space.
11. **Font loading can fail** — always check for NULL and have a fallback.
12. **Audio thread stack canary is your safety net** — if it triggers,
    increase `AUDIO_THREAD_STACK_SIZE`.
13. **All audio thread globals are `static`** — be careful not to introduce
    new globals without proper synchronization.
14. **`audio_stop()` is idempotent** — it checks `if (!s_thread) return;`.
    Safe to call multiple times.
15. **When modifying audio.c**, re-read the crash history (§8) — the audio
    thread has a very specific cleanup sequence that is easy to break.
