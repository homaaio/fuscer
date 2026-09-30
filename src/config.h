#ifndef FS_CONFIG_H
#define FS_CONFIG_H

typedef struct {
    char name[64];          // node name (agent), default: hostname 
    char listen[128];       // coordinator: bind address, host:port 
    char coordinator[128];  // agent/status: coordinator address, host:port 
    char key[256];          // shared secret (HMAC) 
    int  interval;          // agent: heartbeat interval, seconds 
    int  allow_exec;        // agent: accept remote exec (default: off) 
} fs_config;

void cfg_defaults(fs_config *c);

// Returns 0 ok, -1 cannot open, -2 syntax error. 
int  cfg_load_file(fs_config *c, const char *path);

// FUSCER_KEY overrides the file. 
void cfg_load_env(fs_config *c);

// Valid node name: [A-Za-z0-9._-], 1..63 chars. 
int  cfg_name_ok(const char *s);
#endif
