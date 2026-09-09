#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    char *shared_dir; /* NULL when absent — callers apply their own default */
    char *peer_host;
    int peer_port;    /* 0 when absent */
    char *token;
} config;

/* Reads config.json and exits loudly if it isn't there, exactly like the
   Python load_config(). Searches $LAN_FOLDER_SYNC_CONFIG, then ./config.json,
   then next to the executable (and its parent, for bin/ builds). */
config *load_config(const char *argv0);
void config_free(config *cfg);

#endif
