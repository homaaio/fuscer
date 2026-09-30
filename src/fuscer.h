#ifndef FS_FUSCER_H
#define FS_FUSCER_H

#include "config.h"

int coordinator_run(const fs_config *cfg);
int agent_run(const fs_config *cfg);
int status_run(const fs_config *cfg);

#endif
