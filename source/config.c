#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define CONFIG_DIR  "sdmc:/3ds/navidrome"

void config_defaults(NaviConfig *cfg) {
    strncpy(cfg->host,     "192.168.1.100", MAX_STR);
    cfg->port = 4533;
    strncpy(cfg->username, "admin",         MAX_STR);
    strncpy(cfg->password, "password",      MAX_STR);
}

int config_load(NaviConfig *cfg) {
    config_defaults(cfg);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        line[strcspn(line, "\r\n")] = 0;

        // Skip comments and section headers
        if (line[0] == '#' || line[0] == '[' || line[0] == '\0') continue;

        char key[128], val[256];
        if (sscanf(line, "%127[^=]=%255s", key, val) == 2) {
            if (strcmp(key, "host")     == 0) strncpy(cfg->host,     val, MAX_STR);
            if (strcmp(key, "port")     == 0) cfg->port = atoi(val);
            if (strcmp(key, "username") == 0) strncpy(cfg->username, val, MAX_STR);
            if (strcmp(key, "password") == 0) strncpy(cfg->password, val, MAX_STR);
        }
    }

    fclose(f);
    return 0;
}

void config_save(const NaviConfig *cfg) {
    // Ensure directory exists
    mkdir(CONFIG_DIR, 0777);

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) return;

    fprintf(f, "[server]\n");
    fprintf(f, "host=%s\n",     cfg->host);
    fprintf(f, "port=%d\n",     cfg->port);
    fprintf(f, "username=%s\n", cfg->username);
    fprintf(f, "password=%s\n", cfg->password);

    fclose(f);
}
