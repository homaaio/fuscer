#ifndef FS_SYSINFO_H
#define FS_SYSINFO_H

typedef struct {
    char os[32];       // always "linux" 
    char kernel[64];
    char distro[96];
    char arch[32];
    int  cores;
    long ram_mb;
    char gpu[16];      // nvidia | dri | none 
    char tools[256];   // comma separated, found in PATH 
} fs_caps;

void   sys_collect_caps(fs_caps *c);
double sys_load1(void);

#endif
