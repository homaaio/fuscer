#define _GNU_SOURCE
#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

void cfg_defaults(fs_config *c)
{
    memset(c, 0, sizeof *c);

    char h[64] = "";
    if (gethostname(h, sizeof h - 1) != 0 || !h[0]) fs_copy(h, sizeof h, "node");
    for (char *p = h; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-')
            *p = '-';
    fs_copy(c->name, sizeof c->name, h);

    fs_copy(c->listen, sizeof c->listen, "127.0.0.1:" FS_DEFAULT_PORT);
    fs_copy(c->coordinator, sizeof c->coordinator, "127.0.0.1:" FS_DEFAULT_PORT);
    c->interval = 5;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

int cfg_load_file(fs_config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    int ln = 0, rc = 0, have_key = 0;

    while (fgets(line, sizeof line, f)) {
        ln++;
        char *s = trim(line);
        if (!*s || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            LOGE("%s:%d: expected key = value", path, ln);
            rc = -2;
            continue;
        }
        *eq = '\0';
        char *k = trim(s), *v = trim(eq + 1);

        if      (!strcmp(k, "name"))        fs_copy(c->name, sizeof c->name, v);
        else if (!strcmp(k, "listen"))      fs_copy(c->listen, sizeof c->listen, v);
        else if (!strcmp(k, "coordinator")) fs_copy(c->coordinator, sizeof c->coordinator, v);
        else if (!strcmp(k, "interval"))    c->interval = atoi(v);
        else if (!strcmp(k, "key")) {
            fs_copy(c->key, sizeof c->key, v);
            have_key = 1;
        } else {
            LOGW("%s:%d: unknown option '%s'", path, ln, k);
        }
    }

    if (have_key) {
        struct stat st;
        if (fstat(fileno(f), &st) == 0 && (st.st_mode & 077))
            LOGW("%s contains the key but is accessible by other users; "
                 "run: chmod 600 %s", path, path);
    }
    fclose(f);
    return rc;
}

void cfg_load_env(fs_config *c)
{
    const char *k = getenv("FUSCER_KEY");
    if (k && *k) fs_copy(c->key, sizeof c->key, k);
}

int cfg_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 63) return 0;
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '_' && *s != '-')
            return 0;
    return 1;
}
