#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "config.h"
#include "fuscer.h"

static void usage(FILE *o)
{
    fprintf(o,
"fuscer " FS_VERSION_STR " - distributed build/test environment for a Linux LAN\n"
"\n"
"usage: fuscer <command> [options]\n"
"\n"
"commands:\n"
"  coordinator        run the coordinator\n"
"  agent              run an agent on this machine\n"
"  status             show nodes known to the coordinator\n"
"  submit, logs       not implemented in v0.1 (roadmap steps 2+)\n"
"\n"
"options:\n"
"  -c, --config FILE      config file (default: ./fuscer.conf, /etc/fuscer.conf)\n"
"      --listen ADDR      coordinator: bind address, host:port\n"
"      --connect ADDR     agent/status: coordinator address, host:port\n"
"      --name NAME        agent: node name (default: hostname)\n"
"      --interval SEC     agent: heartbeat interval (default: 5)\n"
"  -h, --help             this help\n"
"  -V, --version          print version\n"
"\n"
"The shared key is read from the config file ('key = ...') or from the\n"
"FUSCER_KEY environment variable. It is intentionally not a command-line\n"
"option (it would be visible in `ps`).\n");
}

static const struct option LONG_OPTS[] = {
    { "config",   required_argument, NULL, 'c' },
    { "listen",   required_argument, NULL, 'L' },
    { "connect",  required_argument, NULL, 'C' },
    { "name",     required_argument, NULL, 'n' },
    { "interval", required_argument, NULL, 'i' },
    { "help",     no_argument,       NULL, 'h' },
    { "version",  no_argument,       NULL, 'V' },
    { NULL, 0, NULL, 0 }
};

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
        puts("fuscer " FS_VERSION_STR);
        return 0;
    }

    const char *cmd = argv[1];
    if (strcmp(cmd, "coordinator") && strcmp(cmd, "agent") &&
        strcmp(cmd, "status")) {
        if (!strcmp(cmd, "submit") || !strcmp(cmd, "logs")) {
            fprintf(stderr, "fuscer: '%s' is not implemented in v0.1\n", cmd);
            return 2;
        }
        fprintf(stderr, "fuscer: unknown command '%s'\n\n", cmd);
        usage(stderr);
        return 2;
    }

    // Pass 1: find --config. Pass 2 (after file + env): apply flags on top. 
    const char *conf_path = NULL;
    int ac = argc - 1;
    char **av = argv + 1;

    opterr = 0;
    int o;
    while ((o = getopt_long(ac, av, "c:hV", LONG_OPTS, NULL)) != -1)
        if (o == 'c') conf_path = optarg;

    fs_config cfg;
    cfg_defaults(&cfg);

    if (conf_path) {
        int rc = cfg_load_file(&cfg, conf_path);
        if (rc == -1) {
            fprintf(stderr, "fuscer: cannot open config %s\n", conf_path);
            return 2;
        }
        if (rc == -2) return 2;
    } else {
        static const char *const defaults[] = { "./fuscer.conf", "/etc/fuscer.conf", NULL };
        for (int i = 0; defaults[i]; i++) {
            int rc = cfg_load_file(&cfg, defaults[i]);
            if (rc == -2) return 2;
            if (rc == 0) break;
        }
    }
    cfg_load_env(&cfg);

    optind = 1;
    opterr = 1;
    while ((o = getopt_long(ac, av, "c:hV", LONG_OPTS, NULL)) != -1) {
        switch (o) {
        case 'c': break;
        case 'L': fs_copy(cfg.listen, sizeof cfg.listen, optarg); break;
        case 'C': fs_copy(cfg.coordinator, sizeof cfg.coordinator, optarg); break;
        case 'n': fs_copy(cfg.name, sizeof cfg.name, optarg); break;
        case 'i': cfg.interval = atoi(optarg); break;
        case 'h': usage(stdout); return 0;
        case 'V': puts("fuscer " FS_VERSION_STR); return 0;
        default:  usage(stderr); return 2;
        }
    }

    if (!cfg.key[0]) {
        fprintf(stderr, "fuscer: no shared key. Set 'key = ...' in the config "
                        "file or export FUSCER_KEY.\n");
        return 2;
    }
    if (strlen(cfg.key) < 16)
        LOGW("shared key is short (%zu chars); use at least 16 random characters",
             strlen(cfg.key));

    if (!strcmp(cmd, "agent")) {
        if (!cfg_name_ok(cfg.name)) {
            fprintf(stderr, "fuscer: invalid node name '%s' (allowed: A-Z a-z 0-9 . _ -)\n",
                    cfg.name);
            return 2;
        }
        if (cfg.interval < 1 || cfg.interval > 3600) {
            fprintf(stderr, "fuscer: interval must be 1..3600 seconds\n");
            return 2;
        }
    }

    fs_install_signals();

    if (!strcmp(cmd, "coordinator")) return coordinator_run(&cfg);
    if (!strcmp(cmd, "agent"))       return agent_run(&cfg);
    return status_run(&cfg);
}
