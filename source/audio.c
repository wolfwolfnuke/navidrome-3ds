/*
 * audio.c - clean version with proper synchronization
 */

#include "audio.h"
#include "debug.h"
#include <3ds.h>
#include <string.h>
#include <stdlib.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define SAMPLE_RATE    44100
#define CHANNEL_COUNT  2
#define BUF_SAMPLES    4096
#define BUF_SIZE       (BUF_SAMPLES * CHANNEL_COUNT * sizeof(s16))
#define NUM_BUFS       2
#define MAX_DL_SIZE    (5 * 1024 * 1024)

static ndspWaveBuf   s_wave_bufs[NUM_BUFS];
static s16          *s_pcm_buf   = NULL;
static Thread        s_thread    = NULL;
static LightLock     s_ctx_lock;
static httpcContext *s_ctx       = NULL;
static LightEvent    s_exit_event;  // thread signals this just before returning

static volatile bool s_playing  = false;
static volatile bool s_paused   = false;
static volatile bool s_stop_req = false;
static float         s_volume   = 1.0f;
static char          s_url[1024] = {0};

static void audio_thread(void *arg) {
    (void)arg;
    u8   *dl_buf = NULL;
    drmp3 *mp3   = NULL;

    // ---- Download ----
    httpcContext *ctx = (httpcContext*)malloc(sizeof(httpcContext));
    if (!ctx) { debug_log("audio: malloc ctx failed"); goto done; }

    if (R_FAILED(httpcOpenContext(ctx, HTTPC_METHOD_GET, s_url, 1))) {
        debug_log("audio: httpcOpenContext failed");
        free(ctx); goto done;
    }
    httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
    httpcAddRequestHeaderField(ctx, "Connection", "close");

    if (R_FAILED(httpcBeginRequest(ctx))) {
        debug_log("audio: httpcBeginRequest failed");
        httpcCloseContext(ctx); free(ctx); goto done;
    }

    LightLock_Lock(&s_ctx_lock);
    s_ctx = ctx;
    LightLock_Unlock(&s_ctx_lock);

    {
        u32 http_status = 0;
        httpcGetResponseStatusCode(ctx, &http_status);
        debug_log("audio: HTTP %lu", (unsigned long)http_status);
        if (http_status != 200) goto close_ctx;

        // Allocate fixed buffer — never realloc so drmp3's internal
        // pointer stays valid for the entire decode phase
        dl_buf = (u8*)malloc(MAX_DL_SIZE);
        if (!dl_buf) { debug_log("audio: malloc failed"); goto close_ctx; }

        u32 offset = 0;
        while (!s_stop_req) {
            u32 want = 32768;
            if (offset + want > MAX_DL_SIZE) want = MAX_DL_SIZE - offset;
            if (want == 0) break;
            u32 got = 0;
            Result rc = httpcDownloadData(ctx, dl_buf + offset, want, &got);
            offset += got;
            if (rc == HTTPC_STATUS_DOWNLOAD_READY) break;
            if ((u32)rc == 0xD840A02B) continue;
            if (R_FAILED(rc)) break;
        }
        debug_log("audio: downloaded %lu bytes", (unsigned long)offset);

        if (s_stop_req || offset < 128) goto close_ctx;

        // ---- Decode ----
        mp3 = (drmp3*)malloc(sizeof(drmp3));
        if (!mp3 || !drmp3_init_memory(mp3, dl_buf, offset, NULL)) {
            debug_log("audio: drmp3 init failed");
            goto close_ctx;
        }
        debug_log("audio: %luHz %luch", (unsigned long)mp3->sampleRate, (unsigned long)mp3->channels);

        // ---- ndsp ----
        ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
        ndspChnSetRate(0, (float)mp3->sampleRate);
        ndspChnSetFormat(0, mp3->channels == 2
            ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
        {
            float mix[12] = {0};
            mix[0] = mix[1] = s_volume;
            ndspChnSetMix(0, mix);
        }

        memset(s_wave_bufs, 0, sizeof(s_wave_bufs));
        for (int i = 0; i < NUM_BUFS; i++) {
            s_wave_bufs[i].data_vaddr = s_pcm_buf + i * BUF_SAMPLES * CHANNEL_COUNT;
            s_wave_bufs[i].nsamples   = BUF_SAMPLES;
            s_wave_bufs[i].status     = NDSP_WBUF_DONE;
        }

        s_playing = true;
        int cur = 0;

        while (!s_stop_req) {
            if (s_paused) { svcSleepThread(8000000LL); continue; }
            ndspWaveBuf *wb = &s_wave_bufs[cur];
            if (wb->status != NDSP_WBUF_DONE) { svcSleepThread(2000000LL); continue; }

            drmp3_uint64 n = drmp3_read_pcm_frames_s16(
                mp3, BUF_SAMPLES, (drmp3_int16*)wb->data_vaddr);
            if (n == 0) break;

            wb->nsamples = (u32)n;
            DSP_FlushDataCache(wb->data_vaddr, n * CHANNEL_COUNT * sizeof(s16));
            ndspChnWaveBufAdd(0, wb);
            cur ^= 1;
        }

        s_playing = false;

        // Drain if natural end (not stopped)
        if (!s_stop_req)
            while (ndspChnIsPlaying(0)) svcSleepThread(4000000LL);

        // Keep NDSP teardown on the playback thread so reset/queue cleanup
        // cannot race with wave-buffer submission from another thread.
        ndspChnReset(0);

        // Clean up decode state while dl_buf is still alive
        drmp3_uninit(mp3);
        free(mp3);
        mp3 = NULL;
    }

close_ctx:
    LightLock_Lock(&s_ctx_lock);
    s_ctx = NULL;
    LightLock_Unlock(&s_ctx_lock);
    if (ctx) { httpcCloseContext(ctx); free(ctx); }

done:
    // Free download buffer last — drmp3 referenced it
    if (dl_buf) { free(dl_buf); dl_buf = NULL; }
    if (mp3)    { drmp3_uninit(mp3); free(mp3); mp3 = NULL; }
    s_playing = false;
    debug_log("audio: thread exit");
    // Signal that we are completely done with all memory
    LightEvent_Signal(&s_exit_event);
}

int audio_init(void) {
    LightLock_Init(&s_ctx_lock);
    LightEvent_Init(&s_exit_event, RESET_ONESHOT);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetOutputCount(1);
    ndspSetMasterVol(1.0f);
    s_pcm_buf = (s16*)linearAlloc(BUF_SIZE * NUM_BUFS);
    debug_log("audio_init: pcm_buf=%p", s_pcm_buf);
    return s_pcm_buf ? 0 : -1;
}

void audio_cleanup(void) {
    audio_stop();
    if (s_pcm_buf) { linearFree(s_pcm_buf); s_pcm_buf = NULL; }
}

int audio_play_url(const char *url) {
    if (!s_pcm_buf) return -2;
    audio_stop();
    strncpy(s_url, url, sizeof(s_url)-1);
    s_url[sizeof(s_url)-1] = '\0';
    s_stop_req = false;
    s_paused   = false;
    s_playing  = false;
    LightEvent_Clear(&s_exit_event);
    s_thread = threadCreate(audio_thread, NULL, 512*1024, 0x31, 0, true);
    debug_log("audio: thread %s", s_thread ? "OK" : "FAIL");
    return s_thread ? 0 : -1;
}

void audio_stop(void) {
    if (!s_thread) return;
    debug_log("audio_stop: start");
    s_stop_req = true;

    // Unblock httpcDownloadData
    LightLock_Lock(&s_ctx_lock);
    if (s_ctx) { httpcCloseContext(s_ctx); s_ctx = NULL; }
    LightLock_Unlock(&s_ctx_lock);

    // Wait for thread to signal it's done with ALL memory before we return
    LightEvent_Wait(&s_exit_event);

    threadJoin(s_thread, U64_MAX);
    threadFree(s_thread);
    s_thread   = NULL;
    s_playing  = false;
    s_paused   = false;
    s_stop_req = false;
    debug_log("audio_stop: done");
}

void audio_toggle_pause(void) {
    if (!s_playing) return;
    s_paused = !s_paused;
    ndspChnSetPaused(0, s_paused);
}

bool audio_is_playing(void) { return s_playing; }
bool audio_is_paused(void)  { return s_paused;  }

void audio_set_volume(float vol) {
    s_volume = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
    if (s_playing) {
        float mix[12] = {0};
        mix[0] = mix[1] = s_volume;
        ndspChnSetMix(0, mix);
    }
}

float audio_get_volume(void) { return s_volume; }
