#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

#ifndef DEBUG_BUILD
#define DEBUG_BUILD 0
#endif

#define APP_MAX_FRAME_SIZE 4096U

typedef enum {
    APP_LOG_ERROR = 0,
    APP_LOG_WARN  = 1,
    APP_LOG_INFO  = 2,
    APP_LOG_DEBUG = 3
} app_log_level;

void app_log_set_level(app_log_level level);
int app_log_parse_level(const char *text, app_log_level *out);
void app_log(app_log_level level, const char *fmt, ...);

#define LOGE(...) app_log(APP_LOG_ERROR, __VA_ARGS__)
#define LOGW(...) app_log(APP_LOG_WARN,  __VA_ARGS__)
#define LOGI(...) app_log(APP_LOG_INFO,  __VA_ARGS__)
#if DEBUG_BUILD
#define LOGD(...) app_log(APP_LOG_DEBUG, __VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif

/* Framing protocol: 4-byte network-byte-order length followed by payload. */
int app_send_frame(int fd, const void *payload, uint32_t length);
/* Returns 1 for a frame, 0 for clean EOF before a frame, and -1 on error. */
int app_recv_frame(int fd, char *buffer, size_t capacity, size_t *out_length);
/* Educational comparison only: intentionally performs one recv() per section
 * and accepts a zero-length frame. Do not use this in production. */
int app_recv_frame_baseline(int fd, char *buffer, size_t capacity, size_t *out_length);

int app_set_socket_timeouts(int fd, int seconds);
int app_collect_system_info(char *buffer, size_t capacity);

#endif
