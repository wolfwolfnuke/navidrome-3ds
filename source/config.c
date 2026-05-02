#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include "debug.h"

#define CONFIG_DIR  "sdmc:/3ds/navidrome"

void config_defaults(NaviConfig *cfg) {
    debug_log("[CONFIG] config_defaults called");
    strncpy(cfg->host,     "192.168.1.100", MAX_STR);
    cfg->port = 4533;
    strncpy(cfg->username, "admin",         MAX_STR);
    strncpy(cfg->password, "password",      MAX_STR);
    debug_log("[CONFIG] config_defaults set host=%s, port=%d, user=%s", cfg->host, cfg->port, cfg->username);
}

int config_load(NaviConfig *cfg) {
    debug_log("[CONFIG] config_load called");
    config_defaults(cfg);

    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        debug_log("[CONFIG] config_load: missing or unreadable %s, trying alternate path", CONFIG_PATH);
        f = fopen(CONFIG_PATH_ALT, "r");
        if (!f) {
            debug_log("[CONFIG] config_load: missing or unreadable %s", CONFIG_PATH_ALT);
            return -1;
        }
    }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        // Strip newline
        line[strcspn(line, "\r\n")] = 0;

        // Skip comments and section headers
        if (line[0] == '#' || line[0] == '[' || line[0] == '\0') continue;

        char key[128], val[256];
        if (sscanf(line, "%127[^=]=%255s", key, val) == 2) {
            debug_log("[CONFIG] config_load: parsed key=%s, val=%s", key, val);
            if (strcmp(key, "host")     == 0) { strncpy(cfg->host,     val, MAX_STR); debug_log("[CONFIG] config_load: set host=%s", val); }
            if (strcmp(key, "port")     == 0) { cfg->port = atoi(val); debug_log("[CONFIG] config_load: set port=%d", cfg->port); }
            if (strcmp(key, "username") == 0) { strncpy(cfg->username, val, MAX_STR); debug_log("[CONFIG] config_load: set username=%s", val); }
            if (strcmp(key, "password") == 0) { strncpy(cfg->password, val, MAX_STR); debug_log("[CONFIG] config_load: set password=***"); }
        }
    }

    fclose(f);
    debug_log("[CONFIG] config_load complete: host=%s, port=%d, user=%s", cfg->host, cfg->port, cfg->username);
    return 0;
}

void config_save(const NaviConfig *cfg) {
    debug_log("[CONFIG] config_save called: host=%s, port=%d, user=%s", cfg->host, cfg->port, cfg->username);
    // Ensure directory exists
    // Recursively create parent directories if needed
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", CONFIG_DIR);
    for (char *p = tmp + 6; *p; p++) { // skip "sdmc:/"
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0777);
            *p = '/';
        }
    }
    int mkres = mkdir(CONFIG_DIR, 0777);
    if (mkres != 0 && errno != EEXIST) {
        debug_log("[CONFIG] config_save: failed to create %s (errno=%d)", CONFIG_DIR, errno);
        // Try alternate path (root)
        FILE *f_alt = fopen(CONFIG_PATH_ALT, "w");
        if (!f_alt) {
            debug_log("[CONFIG] config_save: failed to open %s for writing", CONFIG_PATH_ALT);
            return;
        }
        fprintf(f_alt, "[server]\n");
        fprintf(f_alt, "host=%s\n", cfg->host);
        fprintf(f_alt, "port=%d\n", cfg->port);
        fprintf(f_alt, "username=%s\n", cfg->username);
        fprintf(f_alt, "password=%s\n", cfg->password);
        fclose(f_alt);
        debug_log("[CONFIG] config_save: wrote config to alternate path %s", CONFIG_PATH_ALT);
        return;
    }

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        debug_log("[CONFIG] config_save: failed to open %s for writing, trying alternate path", CONFIG_PATH);
        FILE *f_alt = fopen(CONFIG_PATH_ALT, "w");
        if (!f_alt) {
            debug_log("[CONFIG] config_save: failed to open %s for writing", CONFIG_PATH_ALT);
            return;
        }
        fprintf(f_alt, "[server]\n");
        fprintf(f_alt, "host=%s\n", cfg->host);
        fprintf(f_alt, "port=%d\n", cfg->port);
        fprintf(f_alt, "username=%s\n", cfg->username);
        fprintf(f_alt, "password=%s\n", cfg->password);
        fclose(f_alt);
        debug_log("[CONFIG] config_save: wrote config to alternate path %s", CONFIG_PATH_ALT);
        return;
    }

    fprintf(f, "[server]\n");
    fprintf(f, "host=%s\n",     cfg->host);
    fprintf(f, "port=%d\n",     cfg->port);
    fprintf(f, "username=%s\n", cfg->username);
    fprintf(f, "password=%s\n", cfg->password);

    fclose(f);
}
