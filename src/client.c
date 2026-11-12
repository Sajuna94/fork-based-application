#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CLIENT_TIMEOUT_SEC 20

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--log-level error|warn|info|debug] HOST PORT COMMAND [ARG ...]\n"
            "Commands: INFO | ECHO <text> | HOLD <1-10>\n",
            program);
}

static int parse_port(const char *text, char *out, size_t capacity)
{
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }
    return snprintf(out, capacity, "%ld", value) < (int)capacity ? 0 : -1;
}

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *entry;
    int fd = -1;
    int gai_result;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    gai_result = getaddrinfo(host, port, &hints, &result);
    if (gai_result != 0) {
        LOGE("getaddrinfo: %s", gai_strerror(gai_result));
        return -1;
    }

    for (entry = result; entry != NULL; entry = entry->ai_next) {
        fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) {
            break;
        }
        LOGD("connect attempt failed: %s", strerror(errno));
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);
    return fd;
}

int main(int argc, char **argv)
{
    int index = 1;
    app_log_level log_level = APP_LOG_INFO;
    char port_text[16];
    char request[APP_MAX_FRAME_SIZE + 1];
    char response[APP_MAX_FRAME_SIZE + 1];
    size_t request_length = 0;
    size_t response_length = 0;
    int fd;
    int i;
    int written;

    while (index < argc && strcmp(argv[index], "--log-level") == 0) {
        if (index + 1 >= argc || app_log_parse_level(argv[index + 1], &log_level) < 0) {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
        index += 2;
    }
    app_log_set_level(log_level);

    if (argc - index < 3 || parse_port(argv[index + 1], port_text, sizeof(port_text)) < 0) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    written = snprintf(request, sizeof(request), "%s", argv[index + 2]);
    if (written < 0 || (size_t)written >= sizeof(request)) {
        LOGE("request too long");
        return EXIT_FAILURE;
    }
    request_length = (size_t)written;
    for (i = index + 3; i < argc; ++i) {
        size_t used = request_length;
        written = snprintf(request + used, sizeof(request) - used, " %s", argv[i]);
        if (written < 0 || (size_t)written >= sizeof(request) - used) {
            LOGE("request too long (maximum payload is %u bytes)", APP_MAX_FRAME_SIZE);
            return EXIT_FAILURE;
        }
        request_length += (size_t)written;
    }
    if (request_length == 0 || request_length > APP_MAX_FRAME_SIZE) {
        LOGE("invalid request length");
        return EXIT_FAILURE;
    }

    fd = connect_to_server(argv[index], port_text);
    if (fd < 0) {
        LOGE("cannot connect to %s:%s: %s", argv[index], port_text, strerror(errno));
        return EXIT_FAILURE;
    }
    if (app_set_socket_timeouts(fd, CLIENT_TIMEOUT_SEC) < 0) {
        LOGW("cannot set socket timeout: %s", strerror(errno));
    }

    LOGD("connected to %s:%s; sending %zu-byte request", argv[index], port_text, request_length);
    if (app_send_frame(fd, request, (uint32_t)request_length) < 0) {
        LOGE("send request: %s", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }
    if (app_recv_frame(fd, response, sizeof(response), &response_length) != 1) {
        LOGE("receive response failed: %s", strerror(errno));
        close(fd);
        return EXIT_FAILURE;
    }

    printf("%.*s", (int)response_length, response);
    if (response_length == 0 || response[response_length - 1] != '\n') {
        putchar('\n');
    }
    close(fd);
    return strncmp(response, "ERR:", 4) == 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
