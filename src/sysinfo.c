#define _GNU_SOURCE
#include "sysinfo.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "common.h"

static const char *const TOOL_LIST[] = {
    "gcc", "g++", "clang", "clang-tidy", "cmake", "make", "git",
    "cppcheck", "valgrind", "gdb", "llama-server", "llama-cli", NULL
};

static int in_path(const char *name)
{
    const char *path = getenv("PATH");
    if (!path || !*path) return 0;

    char *copy = strdup(path);
    if (!copy) return 0;

    int found = 0;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found;
         dir = strtok_r(NULL, ":", &save)) {
        char full[1024];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, name);
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
            found = 1;
    }
    free(copy);
    return found;
}

static void read_distro(char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen("/etc/os-release", "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "PRETTY_NAME=", 12) != 0) continue;
        char *v = line + 12;
        v[strcspn(v, "\r\n")] = '\0';
        size_t l = strlen(v);
        if (l >= 2 && v[0] == '"' && v[l - 1] == '"') {
            v[l - 1] = '\0';
            v++;
        }
        fs_copy(out, n, v);
        break;
    }
    fclose(f);
}

static long read_ram_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char line[128];
    long kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemTotal: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb / 1024;
}

static void detect_gpu(char *out, size_t n)
{
    if (access("/proc/driver/nvidia", F_OK) == 0) {
        fs_copy(out, n, "nvidia");
        return;
    }
    DIR *d = opendir("/dev/dri");
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "renderD", 7) == 0) {
                closedir(d);
                fs_copy(out, n, "dri");
                return;
            }
        }
        closedir(d);
    }
    fs_copy(out, n, "none");
}

void sys_collect_caps(fs_caps *c)
{
    memset(c, 0, sizeof *c);
    fs_copy(c->os, sizeof c->os, "linux");

    struct utsname u;
    if (uname(&u) == 0) {
        fs_copy(c->kernel, sizeof c->kernel, u.release);
        fs_copy(c->arch, sizeof c->arch, u.machine);
    }
    read_distro(c->distro, sizeof c->distro);

    long nc = sysconf(_SC_NPROCESSORS_ONLN);
    c->cores = nc > 0 ? (int)nc : 1;
    c->ram_mb = read_ram_mb();
    detect_gpu(c->gpu, sizeof c->gpu);

    size_t len = 0;
    for (int i = 0; TOOL_LIST[i]; i++) {
        if (!in_path(TOOL_LIST[i])) continue;
        int w = snprintf(c->tools + len, sizeof c->tools - len, "%s%s",
                         len ? "," : "", TOOL_LIST[i]);
        if (w < 0 || (size_t)w >= sizeof c->tools - len) break;
        len += (size_t)w;
    }
}

double sys_load1(void)
{
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return 0.0;
    double l = 0.0;
    if (fscanf(f, "%lf", &l) != 1) l = 0.0;
    fclose(f);
    return l;
}
