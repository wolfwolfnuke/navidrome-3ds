# Code Context

## Files Retrieved
1. `source/audio.c` - Audio thread stack configuration and overflow prevention measures

## Key Code
```c
#define AUDIO_THREAD_STACK_SIZE (1024 * 1024)

static void plant_stack_canary(void) {
    u32 *p = (u32 *)s_thread_stack;
    for (int i = 0; i < STACK_CANARY_WORDS; i++)
        p[i] = STACK_CANARY_VALUE;
}

static void check_stack_canary(void) {
    int first_bad = -1;
    u32 *p = (u32 *)s_thread_stack;
    for (int i = 0; i < STACK_CANARY_WORDS; i++) {
        if (p[i] != STACK_CANARY_VALUE) { first_bad = i; break; }
    }
    if (first_bad >= 0)
        debug_log("audio: STACK OVERFLOW DETECTED! canary[%d] = 0x%08lX (expected 0x%08lX)",
                  first_bad, (unsigned long)p[first_bad],
                  (unsigned long)STACK_CANARY_VALUE);
}
```

## Architecture
The audio thread stack is configured to be 1MB to prevent stack overflow issues. A stack canary mechanism is used to detect overflows by writing known values at the bottom of the stack and checking these values after the thread exits. The stack is explicitly allocated and managed with careful synchronization.

## Start Here
Start with `source/audio.c` to understand the stack size configuration and the stack canary mechanism used to prevent overflow issues.