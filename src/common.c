#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static app_log_level g_log_level = APP_LOG_INFO;

void app_log_set_level(app_log_level level)
{
    g_log_level = level;
}

int app_log_parse_level(const char *text, app_log_level *out)
{
    if (text == NULL || out == NULL) {
        return -1;
    }
    if (strcmp(text, "error") == 0) {
        *out = APP_LOG_ERROR;
    } else if (strcmp(text, "warn") == 0 || strcmp(text, "warning") == 0) {
        *out = APP_LOG_WARN;
    } else if (strcmp(text, "info") == 0) {
        *out = APP_LOG_INFO;
    } else if (strcmp(text, "debug") == 0) {
        *out = APP_LOG_DEBUG;
    } else {
        return -1;
    }
    return 0;
}

void app_log(app_log_level level, const char *fmt, ...)
{
    static const char *const names[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    struct timespec ts;
    struct tm tm_now;
    char timebuf[32];
    va_list args;

    if (level > g_log_level || fmt == NULL) {
        return;
    }

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 &&
        localtime_r(&ts.tv_sec, &tm_now) != NULL) {
        (void)strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_now);
    } else {
        (void)snprintf(timebuf, sizeof(timebuf), "time-unavailable");
    }

    fprintf(stderr, "[%s.%03ld] [%-5s] [pid=%ld] ", timebuf,
            clock_gettime(CLOCK_REALTIME, &ts) == 0 ? ts.tv_nsec / 1000000L : 0L,
            names[(unsigned int)level], (long)getpid());
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static int send_all(int fd, const void *data, size_t length)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(fd, p + sent, length - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
        }
        return -1;
    }
    return 0;
}

/* allow_clean_eof is used only when reading the beginning of a new header */
static int recv_all(int fd, void *data, size_t length, int allow_clean_eof)
{
    unsigned char *p = (unsigned char *)data;
    size_t received = 0;

    while (received < length) {
        ssize_t n = recv(fd, p + received, length - received, 0);
        if (n > 0) {
            received += (size_t)n;
            continue;
        }
        if (n == 0) {
            if (received == 0 && allow_clean_eof) {
                return 0;
            }
            errno = ECONNRESET; /* peer closed in the middle of a frame */
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 1;
}

int app_send_frame(int fd, const void *payload, uint32_t length)
{
    uint32_t network_length;

    if (payload == NULL || length == 0 || length > APP_MAX_FRAME_SIZE) {
        errno = EINVAL;
        return -1;
    }

    network_length = htonl(length);
    if (send_all(fd, &network_length, sizeof(network_length)) < 0) {
        return -1;
    }
    return send_all(fd, payload, length);
}

int app_recv_frame(int fd, char *buffer, size_t capacity, size_t *out_length)
{
    uint32_t network_length;
    uint32_t length;
    int result;

    if (buffer == NULL || out_length == NULL || capacity < 2) {
        errno = EINVAL;
        return -1;
    }
    *out_length = 0;

    result = recv_all(fd, &network_length, sizeof(network_length), 1);
    if (result <= 0) {
        return result;
    }

    length = ntohl(network_length);
    if (length == 0 || length > APP_MAX_FRAME_SIZE || (size_t)length >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    result = recv_all(fd, buffer, (size_t)length, 0);
    if (result < 0) {
        return -1;
    }

    buffer[length] = '\0';
    *out_length = (size_t)length;
    return 1;
}

/* Deliberately incomplete implementation used only for the robustness
 * comparison exercise. It keeps a capacity check to avoid memory corruption,
 * but omits stream reassembly and the protocol's non-empty-frame validation. */
int app_recv_frame_baseline(int fd, char *buffer, size_t capacity, size_t *out_length)
{
    uint32_t network_length;
    uint32_t length;
    ssize_t n;

    if (buffer == NULL || out_length == NULL || capacity < 1) {
        errno = EINVAL;
        return -1;
    }
    *out_length = 0;

    n = recv(fd, &network_length, sizeof(network_length), 0);
    if (n == 0) {
        return 0;
    }
    if (n < 0) {
        return -1;
    }
    if ((size_t)n != sizeof(network_length)) {
        errno = EPROTO; /* baseline wrongly assumes one recv() gets the header */
        return -1;
    }

    length = ntohl(network_length);
    /* Retain memory safety for this classroom demo, but intentionally accept 0. */
    if ((size_t)length >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }
    if (length == 0) {
        buffer[0] = '\0';
        *out_length = 0;
        return 1;
    }

    n = recv(fd, buffer, (size_t)length, 0);
    if (n < 0) {
        return -1;
    }
    if ((size_t)n != (size_t)length) {
        errno = EPROTO; /* baseline wrongly assumes one recv() gets the payload */
        return -1;
    }

    buffer[length] = '\0';
    *out_length = (size_t)length;
    return 1;
}

int app_set_socket_timeouts(int fd, int seconds)
{
    struct timeval timeout;

    if (seconds <= 0) {
        errno = EINVAL;
        return -1;
    }
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    return 0;
}

static void read_memory_values(unsigned long long *total_kb,
                               unsigned long long *available_kb)
{
    FILE *fp = fopen("/proc/meminfo", "r");
    char line[256];

    *total_kb = 0;
    *available_kb = 0;
    if (fp == NULL) {
        return;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, "MemTotal: %llu kB", total_kb) == 1) {
            continue;
        }
        if (sscanf(line, "MemAvailable: %llu kB", available_kb) == 1) {
            continue;
        }
    }
    fclose(fp);
}

int app_collect_system_info(char *buffer, size_t capacity)
{
    struct utsname info;
    char hostname[256] = "unknown";
    long cpu_count;
    unsigned long long total_kb;
    unsigned long long available_kb;
    int written;

    if (buffer == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }
    if (uname(&info) < 0) {
        return -1;
    }
    if (gethostname(hostname, sizeof(hostname) - 1) < 0) {
        (void)snprintf(hostname, sizeof(hostname), "unknown");
    }
    hostname[sizeof(hostname) - 1] = '\0';
    cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    read_memory_values(&total_kb, &available_kb);

    written = snprintf(buffer, capacity,
                       "hostname: %s\n"
                       "system: %s\n"
                       "kernel_release: %s\n"
                       "kernel_version: %s\n"
                       "machine: %s\n"
                       "online_cpus: %ld\n"
                       "memory_total_kb: %llu\n"
                       "memory_available_kb: %llu\n",
                       hostname, info.sysname, info.release, info.version,
                       info.machine, cpu_count > 0 ? cpu_count : -1L,
                       total_kb, available_kb);
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENOSPC;
        return -1;
    }
    return written;
}
