#pragma once
#include <stdbool.h>

// Initialize the DSP audio subsystem
int  audio_init(void);

// Cleanup
void audio_cleanup(void);

// Start streaming from a URL (non-blocking; spawns a background thread)
int  audio_play_url(const char *url);

// Stop playback
void audio_stop(void);

// Pause / resume toggle
void audio_toggle_pause(void);

bool audio_is_playing(void);
bool audio_is_paused(void);

// Volume: 0.0 – 1.0
void  audio_set_volume(float vol);
float audio_get_volume(void);
