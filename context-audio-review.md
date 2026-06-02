# Code Context

## Files Retrieved
1. `source/audio.c` - Audio thread stack configuration and overflow prevention measures

## Key Code

### Stack Configuration
```c
#define AUDIO_THREAD_STACK_SIZE (1024 * 1024)
#define STACK_CANARY_VALUE  0xDEADBEEFu
#define STACK_CANARY_WORDS  16

static u8 *s_thread_stack = NULL;
```

### Stack Canary Functions
```c
static void plant_stack_canary(void) {
    debug_log("[AUDIO] plant_stack_canary called");
    if (!s_thread_stack) {
        debug_log("[AUDIO] s_thread_stack is NULL in plant_stack_canary");
        return;
    }
    u32 *p = (u32 *)s_thread_stack;
    for (int i = 0; i < STACK_CANARY_WORDS; i++)
        p[i] = STACK_CANARY_VALUE;
    debug_log("[AUDIO] Stack canary planted");
}

static void check_stack_canary(void) {
    debug_log("[AUDIO] check_stack_canary called");
    if (!s_thread_stack) {
        debug_log("[AUDIO] s_thread_stack is NULL in check_stack_canary");
        return;
    }
    int first_bad = -1;
    u32 *p = (u32 *)s_thread_stack;
    for (int i = 0; i < STACK_CANARY_WORDS; i++) {
        if (p[i] != STACK_CANARY_VALUE) { first_bad = i; break; }
    }
    if (first_bad >= 0)
        debug_log("audio: STACK OVERFLOW DETECTED! canary[%d] = 0x%08lX (expected 0x%08lX)",
                  first_bad, (unsigned long)p[first_bad],
                  (unsigned long)STACK_CANARY_VALUE);
    else
        debug_log("audio: stack canary OK (bottom %d words intact, ~%u KB margin)",
                  STACK_CANARY_WORDS,
                  AUDIO_THREAD_STACK_SIZE / 1024);
    debug_log("[AUDIO] check_stack_canary complete");
}
```

### Thread Creation and Cleanup
```c
int audio_play_url(const char *url) {
    // Allocate the thread stack explicitly so we can plant/check the canary.
    s_thread_stack = (u8*)malloc(AUDIO_THREAD_STACK_SIZE);
    if (!s_thread_stack) {
        debug_log("audio: failed to alloc %u KB thread stack",
                  AUDIO_THREAD_STACK_SIZE / 1024);
        return -3;
    }
    plant_stack_canary();
    debug_log("audio: stack alloc OK at %p (%u KB)", s_thread_stack,
              AUDIO_THREAD_STACK_SIZE / 1024);

    LightEvent_Clear(&s_exit_event);
    s_thread = threadCreate(audio_thread, NULL, AUDIO_THREAD_STACK_SIZE, 0x31, 0, false);
    debug_log("audio: thread %s", s_thread ? "OK" : "FAIL");
    if (!s_thread) {
        free(s_thread_stack);
        s_thread_stack = NULL;
        return -1;
    }
    return 0;
}

void audio_stop(void) {
    if (!s_thread) return;
    debug_log("audio_stop: start");
    s_stop_req = true;

    // Unblock httpcDownloadData if the thread is still downloading
    LightLock_Lock(&s_ctx_lock);
    if (s_ctx) { httpcCloseContext(s_ctx); s_ctx = NULL; }
    LightLock_Unlock(&s_ctx_lock);

    // Wait for thread to finish all memory operations
    LightEvent_Wait(&s_exit_event);

    // Check the canary while the stack is still allocated
    check_stack_canary();

    // Safe to reset the channel now: thread is done, stack still alive.
    // Any DSP interrupt from this reset fires here, not after threadFree.
    ndspChnReset(0);

    threadJoin(s_thread, U64_MAX);
    threadFree(s_thread);
    s_thread = NULL;

    if (s_thread_stack) { free(s_thread_stack); s_thread_stack = NULL; }

    s_playing  = false;
    s_paused   = false;
    s_stop_req = false;
    debug_log("audio_stop: done");
}
```

## Architecture

The audio thread stack configuration and overflow prevention measures are implemented as follows:

1. **Stack Size**: The audio thread stack size is set to 1MB to ensure sufficient space for the thread's operations.
2. **Stack Canary**: A stack canary is used to detect stack overflows. The canary is planted at the bottom of the stack before the thread starts and checked after the thread exits.
3. **Stack Allocation**: The stack is explicitly allocated using `malloc` to ensure proper management and checking for overflows.
4. **Thread Creation and Cleanup**: The audio thread is created with the explicitly allocated stack and the correct stack size. The `audio_stop` function ensures proper cleanup, including checking the stack canary, resetting the DSP channel, and freeing the stack memory.
5. **Debug Logging**: Extensive debug logging is included to track the state of the audio thread, stack canary, and other critical operations.

## Start Here

Begin with `source/audio.c` to understand the audio thread stack configuration and overflow prevention measures. This file contains the key implementations and functions related to the audio thread stack management.
