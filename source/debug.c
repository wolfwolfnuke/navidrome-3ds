#include "debug.h"
#include <3ds.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

static Handle s_file   = 0;
static u64    s_offset = 0;
static LightLock s_log_lock;

static void write_str(const char *str) {
    if (!s_file) return;
    u32 written = 0;
    u32 len = strlen(str);
    FSFILE_Write(s_file, &written, s_offset, str, len, FS_WRITE_FLUSH);
    s_offset += written;
}

void debug_init(void) {
    LightLock_Init(&s_log_lock);
    fsInit();

    FS_Archive sdmc;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""))))
        return;

    FSUSER_DeleteFile(sdmc, fsMakePath(PATH_ASCII, "/3ds/navidrome/debug.log"));
    if (R_FAILED(FSUSER_OpenFile(&s_file, sdmc,
            fsMakePath(PATH_ASCII, "/3ds/navidrome/debug.log"),
            FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
        // fallback to SD root
        FSUSER_OpenFile(&s_file, sdmc,
            fsMakePath(PATH_ASCII, "/debug.log"),
            FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    }

    FSUSER_CloseArchive(sdmc);
    write_str("=== Navidrome 3DS ===\n");
}

void debug_log(const char *fmt, ...) {
    if (!s_file) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);
    // ensure newline
    int len = strlen(buf);
    if (len > 0 && buf[len-1] != '\n') { buf[len++] = '\n'; buf[len] = '\0'; }
    LightLock_Lock(&s_log_lock);
    write_str(buf);
    LightLock_Unlock(&s_log_lock);
}

void debug_cleanup(void) {
    if (s_file) { FSFILE_Close(s_file); s_file = 0; }
    fsExit();
}
