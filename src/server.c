#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT 9000
#define DEFAULT_MAX_CHILDREN 64
#define CLIENT_IO_TIMEOUT_SEC 15

static volatile sig_atomic_t g_signal_pipe_write = -1;

// static void signal_handler(int signo)
// {
//     unsigned char code = (signo == SIGCHLD) ? 'C' : 'T';
//     if (g_signal_pipe_write >= 0) {
//         /* write() is async-signal-safe. A full pipe is already readable. */
//         (void)write(g_signal_pipe_write, &code, sizeof(code));
//     }
// }

static void signal_handler(int signo)
{
    unsigned char code = (signo == SIGCHLD) ? 'C' : 'T';

    if (g_signal_pipe_write >= 0) {
        /* write() is async-signal-safe. A full pipe is already readable. */
        ssize_t ret = write(g_signal_pipe_write, &code, sizeof(code));
        if (ret == -1) {
            /* Ignore errors such as EAGAIN when the pipe is full. */
        }
    }
}

static int set_nonblocking_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    int fdflags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fdflags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFD, fdflags | FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}

static int make_signal_pipe(int pipefd[2])
{
    if (pipe(pipefd) < 0) {
        return -1;
    }
    if (set_nonblocking_cloexec(pipefd[0]) < 0 ||
        set_nonblocking_cloexec(pipefd[1]) < 0) {
        int saved = errno;
        close(pipefd[0]);
        close(pipefd[1]);
        errno = saved;
        return -1;
    }
    return 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* let poll() return EINTR */

    if (sigaction(SIGCHLD, &action, NULL) < 0 ||
        sigaction(SIGINT, &action, NULL) < 0 ||
        sigaction(SIGTERM, &action, NULL) < 0) {
        return -1;
    }
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

static void reap_children(int *active_children)
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (*active_children > 0) {
            --(*active_children);
        }
        LOGI("child reaped pid=%ld active_children=%d", (long)pid, *active_children);
    }
}

static int parse_hold_seconds(const char *text, unsigned int *seconds)
{
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 10) {
        return -1;
    }
    *seconds = (unsigned int)value;
    return 0;
}

static void sleep_seconds(unsigned int seconds)
{
    struct timespec remaining = {(time_t)seconds, 0};
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {
        /* Continue sleeping for the remaining duration. */
    }
}

static int process_request(const char *request, char *response, size_t response_cap)
{
    if (strcmp(request, "INFO") == 0) {
        return app_collect_system_info(response, response_cap);
    }

    if (strncmp(request, "ECHO ", 5) == 0) {
        int n = snprintf(response, response_cap, "%s", request + 5);
        if (n < 0 || (size_t)n >= response_cap) {
            errno = ENOSPC;
            return -1;
        }
        return n;
    }

    if (strncmp(request, "HOLD ", 5) == 0) {
        unsigned int seconds;
        int n;
        if (parse_hold_seconds(request + 5, &seconds) < 0) {
            n = snprintf(response, response_cap,
                         "ERR: HOLD duration must be an integer from 1 to 10 seconds");
            return n;
        }
        LOGD("test workload started: seconds=%u", seconds);
        sleep_seconds(seconds);
        n = snprintf(response, response_cap,
                     "HOLD completed: %u second(s), worker_pid=%ld",
                     seconds, (long)getpid());
        return n;
    }

    {
        int n = snprintf(response, response_cap,
                         "ERR: unknown command. Supported commands: INFO, ECHO <text>, HOLD <1-10>");
        return n;
    }
}

static void handle_client(int client_fd, int listen_fd,
                          int signal_pipe_read, int signal_pipe_write,
                          const struct sockaddr_in *peer,
                          int robustness_enabled, int io_timeout_sec)
{
    char request[APP_MAX_FRAME_SIZE + 1];
    char response[APP_MAX_FRAME_SIZE + 1];
    size_t request_length = 0;
    char peer_ip[INET_ADDRSTRLEN] = "?";
    int rc;

    close(listen_fd);
    close(signal_pipe_read);
    close(signal_pipe_write);
    (void)inet_ntop(AF_INET, &peer->sin_addr, peer_ip, sizeof(peer_ip));

    if (robustness_enabled && app_set_socket_timeouts(client_fd, io_timeout_sec) < 0) {
        LOGW("pid=%ld cannot set socket timeout: %s", (long)getpid(), strerror(errno));
    }

    LOGI("worker started peer=%s:%u mode=%s", peer_ip,
         (unsigned int)ntohs(peer->sin_port), robustness_enabled ? "robust" : "baseline");
    rc = robustness_enabled
        ? app_recv_frame(client_fd, request, sizeof(request), &request_length)
        : app_recv_frame_baseline(client_fd, request, sizeof(request), &request_length);
    if (rc == 0) {
        LOGI("peer closed without sending a request");
        close(client_fd);
        _exit(EXIT_SUCCESS);
    }
    if (rc < 0) {
        LOGW("invalid/incomplete request frame: %s", strerror(errno));
        close(client_fd);
        _exit(EXIT_FAILURE);
    }

    LOGD("received request (%zu bytes): %.*s", request_length,
         (int)request_length, request);
    rc = process_request(request, response, sizeof(response));
    if (rc < 0) {
        (void)snprintf(response, sizeof(response), "ERR: failed to collect/process request");
        rc = (int)strlen(response);
    }
    if (app_send_frame(client_fd, response, (uint32_t)rc) < 0) {
        LOGW("send response failed: %s", strerror(errno));
    } else {
        LOGD("response sent (%d bytes)", rc);
    }

    close(client_fd);
    LOGI("worker finished");
    _exit(EXIT_SUCCESS);
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--bind IPv4] [--port PORT] [--max-clients N] "
            "[--log-level error|warn|info|debug] [--robustness robust|baseline] "
            "[--io-timeout SEC]\n",
            program);
}

int main(int argc, char **argv)
{
    const char *bind_ip = "127.0.0.1";
    unsigned int port = DEFAULT_PORT;
    int max_children = DEFAULT_MAX_CHILDREN;
    int robustness_enabled = 1;
    int io_timeout_sec = CLIENT_IO_TIMEOUT_SEC;
    app_log_level log_level = APP_LOG_INFO;
    int i;
    int listen_fd = -1;
    int signal_pipe[2] = {-1, -1};
    int active_children = 0;
    int peak_children = 0;
    int stop_requested = 0;
    struct sockaddr_in address;
    int reuse = 1;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bind") == 0 && i + 1 < argc) {
            bind_ip = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            char *end = NULL;
            unsigned long value = strtoul(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 1 || value > 65535) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            port = (unsigned int)value;
        } else if (strcmp(argv[i], "--max-clients") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 1 || value > 1024) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            max_children = (int)value;
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            if (app_log_parse_level(argv[++i], &log_level) < 0) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--robustness") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            if (strcmp(mode, "robust") == 0) {
                robustness_enabled = 1;
            } else if (strcmp(mode, "baseline") == 0) {
                robustness_enabled = 0;
            } else {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--io-timeout") == 0 && i + 1 < argc) {
            char *end = NULL;
            long value = strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || value < 1 || value > 300) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            io_timeout_sec = (int)value;
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    app_log_set_level(log_level);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        LOGE("socket: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        LOGE("setsockopt(SO_REUSEADDR): %s", strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_ip, &address.sin_addr) != 1) {
        LOGE("invalid IPv4 bind address: %s", bind_ip);
        close(listen_fd);
        return EXIT_FAILURE;
    }
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        LOGE("bind %s:%u: %s", bind_ip, port, strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }
    if (listen(listen_fd, 128) < 0) {
        LOGE("listen: %s", strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (make_signal_pipe(signal_pipe) < 0) {
        LOGE("signal pipe: %s", strerror(errno));
        close(listen_fd);
        return EXIT_FAILURE;
    }
    g_signal_pipe_write = signal_pipe[1];
    if (install_signal_handlers() < 0) {
        LOGE("install signal handlers: %s", strerror(errno));
        close(signal_pipe[0]);
        close(signal_pipe[1]);
        close(listen_fd);
        return EXIT_FAILURE;
    }

    LOGI("server ready bind=%s port=%u max_clients=%d pid=%ld mode=%s io_timeout=%ds",
         bind_ip, port, max_children, (long)getpid(),
         robustness_enabled ? "robust" : "baseline", io_timeout_sec);
    if (!robustness_enabled) {
        LOGW("BASELINE DEMO MODE: selected robustness protections are intentionally disabled; do not use in production");
    }
#if DEBUG_BUILD
    LOGD("compile-time debug logging is ENABLED; runtime level is %d", (int)log_level);
#else
    LOGI("compile-time debug logging is DISABLED (build with make DEBUG=1 to enable)");
#endif

    while (!stop_requested) {
        struct pollfd fds[2];
        int poll_result;

        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = signal_pipe[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        poll_result = poll(fds, 2, -1);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("poll: %s", strerror(errno));
            break;
        }

        if (fds[1].revents & POLLIN) {
            unsigned char signals[128];
            ssize_t n;
            int saw_terminate = 0;
            while ((n = read(signal_pipe[0], signals, sizeof(signals))) > 0) {
                ssize_t j;
                for (j = 0; j < n; ++j) {
                    if (signals[j] == 'T') {
                        saw_terminate = 1;
                    }
                }
            }
            reap_children(&active_children);
            if (saw_terminate) {
                stop_requested = 1;
            }
        }

        if (!stop_requested && (fds[0].revents & POLLIN)) {
            struct sockaddr_in peer;
            socklen_t peer_length = sizeof(peer);
            int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_length);
            if (client_fd < 0) {
                if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOGW("accept: %s", strerror(errno));
                }
                continue;
            }

            /* Reap once more before enforcing the child limit. */
            reap_children(&active_children);
            if (active_children >= max_children) {
                static const char busy[] = "ERR: server is busy; try again later";
                LOGW("connection rejected: max_clients=%d reached", max_children);
                (void)app_send_frame(client_fd, busy, (uint32_t)(sizeof(busy) - 1));
                close(client_fd);
                continue;
            }

            {
                pid_t child = fork();
                if (child < 0) {
                    static const char failure[] = "ERR: server could not create worker";
                    LOGE("fork: %s", strerror(errno));
                    (void)app_send_frame(client_fd, failure, (uint32_t)(sizeof(failure) - 1));
                    close(client_fd);
                    continue;
                }
                if (child == 0) {
                    /* Parent-only handlers must not swallow termination in workers. */
                    (void)signal(SIGINT, SIG_DFL);
                    (void)signal(SIGTERM, SIG_DFL);
                    (void)signal(SIGCHLD, SIG_DFL);
                    g_signal_pipe_write = -1;
                    /* The worker closes inherited listener/signal-pipe descriptors. */
                    handle_client(client_fd, listen_fd, signal_pipe[0], signal_pipe[1],
                                  &peer, robustness_enabled, io_timeout_sec);
                }
                ++active_children;
                if (active_children > peak_children) {
                    peak_children = active_children;
                }
                LOGI("accepted pid=%ld active_children=%d peak_children=%d",
                     (long)child, active_children, peak_children);
                close(client_fd);
            }
        }
    }

    LOGI("server shutting down; peak_children=%d", peak_children);
    close(listen_fd);
    for (;;) {
        pid_t child = waitpid(-1, NULL, 0);
        if (child > 0) {
            continue;
        }
        if (child < 0 && errno == EINTR) {
            continue;
        }
        break; /* ECHILD: no workers remain. */
    }
    g_signal_pipe_write = -1;
    close(signal_pipe[0]);
    close(signal_pipe[1]);
    return EXIT_SUCCESS;
}
