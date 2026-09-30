#ifndef FS_FUSCER_H
#define FS_FUSCER_H

#include "config.h"

int coordinator_run(const fs_config *cfg);
int agent_run(const fs_config *cfg);
int status_run(const fs_config *cfg);

// Runs `cmd` (a shell command line) on `node`; returns the exit status to use.
int exec_run(const fs_config *cfg, const char *node, const char *cmd, int timeout);

#endif
