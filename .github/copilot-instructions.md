# Copilot Instructions: Navidrome 3DS

Navidrome 3DS is a native homebrew music client for the Nintendo 3DS that connects to a Navidrome server via the Subsonic REST API.

## Build, Test, and Lint

### Prerequisites
DevkitPro must be installed with 3DS support. Run the included setup script (requires sudo):
```bash
sudo ./install-devkitpro-pacman  # Sets up devkitPro repository and installs devkitpro-pacman
dkp-pacman -S 3ds-dev 3ds-curl 3ds-mbedtls  # Install 3DS libraries
```
Also manually fetch the MP3 decoder:
```bash
curl -o source/dr_mp3.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
```

### Build
```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
make                    # Compile to navidrome.3dsx
make clean              # Clean build artifacts
```

Key build details:
- Uses devkitARM (ARM11 for 3DS)
- Links: `citro2d`, `citro3d`, `curl`, `mbedtls`, `ctru`, `z`, `m`
- Library link order matters: `-lmbedcrypto` appears twice, `-lctru` at end to resolve circular deps
- Output: `navidrome.3dsx` (executable) + `navidrome.smdh` (metadata for 3DS menu)

### Deployment
```bash
cp navidrome.3dsx /path/to/sd/3ds/navidrome/
```
Also ensure config exists: `/3ds/navidrome/config.ini` on SD card.

### No automated tests
This project has no test suite — only manual testing on actual 3DS hardware.

## Architecture Overview

### High-Level Design
- **Single-threaded UI loop** (main.c) dispatches user actions to subsystems
- **Audio runs in a separate worker thread** with synchronized handshake (LightEvent)
- **Network I/O via libcurl + httpc** — Subsonic API calls through libcurl, audio downloads through 3DS native httpcContext
- **UI renders to both screens** (top screen: now-playing info; bottom screen: scrollable list) using citro2d

### File Breakdown

| File | Purpose |
|------|---------|
| **main.c** | Entry point, initialization order (gfx → citro2d → romfs → soc → httpc → ndsp → audio), main loop, cleanup |
| **ui.c / ui.h** | citro2d rendering, input handling, UI state machine (Artists → Albums → Tracks → Player), UTF-8 aware text drawing with system fonts (CJK support) |
| **api.c / api.h** | Subsonic REST API calls via libcurl (ping, getArtists, getArtist, getAlbum, stream URLs), hand-rolled XML attribute parser with entity decoding |
| **audio.c / audio.h** | MP3 download + decode (dr_mp3.h) + playback (NDSP), threaded audio worker, synchronization with exit event, stack canary checking |
| **config.c / config.h** | Reads/writes `/3ds/navidrome/config.ini` from SD card (server host, port, username, password) |
| **debug.c / debug.h** | Writes debug log to `sdmc:/3ds/navidrome/debug.log` using raw FSUSER (not fopen — fopen unreliable on this firmware) |

## Key Conventions and Critical Details

### Memory and Initialization Order
- **socInit requires exactly 0x100000 bytes, page-aligned** — smaller sizes return error 0xE0A01835
- UiState is **heap-allocated** (calloc) — stack allocation causes immediate crash due to size (~96KB)
- Initialization sequence is strict: gfx → citro2d → romfs → soc → httpc → ndsp → config → api → audio

### API Constants
- `MAX_ITEMS = 200` — max items per API response (artists, albums, tracks)
- `MAX_NAME_LEN = 64` — max length for names
- `MAX_ID_LEN = 32` — max length for Subsonic IDs
- **URL buffers must be ≥ 2048 bytes** — Subsonic URLs with long hostnames/passwords can exceed 1024 bytes

### Audio Thread (Critical — See HANDOVER.md for full crash history)

**Thread Stack:**
- **Must be 1MB** (not 512KB) — dr_mp3 internal structures (scratch buffers, frame decode arrays) exceed 512KB on 48kHz stereo
- Explicitly malloc'd, not threadCreate's default, so canary pattern (0xDEADBEEF) can be planted at bottom
- Stack canary checked on every audio_stop() call and logged if approached

**Synchronization:**
- Audio thread signals `s_exit_event` as its very last action (after calling `ndspSetCallback(NULL, NULL)`)
- Main thread calls `LightEvent_Wait` to wait for signal before cleanup
- **ndspChnReset(0) MUST be called from main thread after thread exits** — calling inside thread causes DSP interrupt to fire on freed memory
- Thread calls `ndspSetCallback(NULL, NULL)` to suppress future DSP callbacks before signaling exit

**Download Buffer:**
- Fixed size `MAX_DL_SIZE` (5MB) — **never realloc'd after dr_mp3_init_memory()** because dr_mp3 stores internal pointer to buffer

**HTTP Download Loop:**
- httpcDownloadData returns 0xD840A02B ("download pending") which is NOT fatal — continue loop, don't break
- audio_stop() calls `httpcCloseContext()` to unblock stuck httpcDownloadData calls

### XML Parsing
- Hand-rolled XML attribute parser (not a full parser)
- Handles numeric entities (&#34;) and named entities (&amp;, &lt;, etc.)
- Used for Subsonic API responses

### UI and Text Rendering
- **citro2d for all rendering** — consoleInit conflicts with citro2d, cannot use both
- **System fonts loaded via C2D_FontLoadSystem** for CJK support (Japanese, Simplified/Traditional Chinese, Korean)
- Custom UTF-8 aware text renderer (`draw_text`) — decodes codepoints, finds first font with glyph, renders in segments
- Top screen: now-playing (title/artist/album, play state, volume bar)
- Bottom screen: scrollable list with 200-item limit per level

### File I/O
- Debug logging uses raw FSUSER API (not fopen) — fopen was unreliable on this firmware
- Writes to `sdmc:/3ds/navidrome/debug.log` with fallback to `sdmc:/debug.log`
- Config read/write also uses FSUSER

## Configuration

User config on SD card at `/3ds/navidrome/config.ini`:
```ini
[server]
host=192.168.1.100
port=4533
username=admin
password=yourpassword
```

Default config embedded in `romfs/config.ini` and copied on first run.

## Important: Required Dependency

**dr_mp3.h must be manually added (single-header MP3 decoder):**
```bash
curl -o source/dr_mp3.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
```
- Source: https://github.com/mackron/dr_libs
- Already #included by audio.c, so if build fails on missing dr_mp3.h, fetch it with the command above

## When Modifying Code

1. **Respect initialization order** in main.c — each subsystem depends on previous ones
2. **Thread stack changes** — if adding large local variables to audio thread, check canary is still safe
3. **URL/buffer lengths** — keep ≥ 2048 for URLs; check MAX_ITEMS fits memory
4. **Memory limits** — audio download buffer is 5MB fixed; UI state is ~96KB; total heap pressure is high on 3DS
5. **Audio stop sequence** — always call audio_stop() before reloading/changing tracks
6. **Library link order** matters (see Makefile notes above)
