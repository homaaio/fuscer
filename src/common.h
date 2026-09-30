#ifndef FS_COMMON_H
#define FS_COMMON_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#define FS_VERSION_STR   "0.2.0"
#define FS_DEFAULT_PORT  "7777"

// Logging (stderr, with timestamp). 
void fs_log(const char *lvl, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#define LOGI(...) fs_log("info", __VA_ARGS__)
#define LOGW(...) fs_log("warn", __VA_ARGS__)
#define LOGE(...) fs_log("error", __VA_ARGS__)

// strncpy that always terminates. 
void fs_copy(char *dst, size_t n, const char *src);

// Set by SIGINT/SIGTERM. Blocking calls are interrupted (no SA_RESTART). 
extern volatile sig_atomic_t fs_stop;
void fs_install_signals(void);

// Monotonic clock, milliseconds.
long long fs_now_ms(void);

// Sleep up to `sec` seconds, wake early when fs_stop is set. 
void fs_sleep(int sec);

#endif
