#pragma once

#define CONFIG_PATH     "sdmc:/3ds/navidrome/config.ini"
#define CONFIG_PATH_ALT "/3ds/navidrome/config.ini"
#define MAX_STR     256

typedef struct {
    char host[MAX_STR];
    int  port;
    char username[MAX_STR];
    char password[MAX_STR];
} NaviConfig;

// Load config from SD card (returns 0 on success)
int  config_load(NaviConfig *cfg);

// Save config to SD card
void config_save(const NaviConfig *cfg);

// Fill cfg with defaults
void config_defaults(NaviConfig *cfg);
