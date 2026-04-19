/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

/* Safe string copy */
#define safe_copy(dst, src, size) \
    do { snprintf((dst), (size), "%s", (src)); } while (0)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/* Usage */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ------------------------------------------------------------------ */
/* Argument parsing */
/* ------------------------------------------------------------------ */

static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nv;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nv = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nv < -20 || nv > 19) {
                fprintf(stderr, "Invalid value for --nice: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nv;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft > hard\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* State string */
/* ------------------------------------------------------------------ */

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* Bounded buffer */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY) {
        if (b->shutting_down) { pthread_mutex_unlock(&b->mutex); return -1; }
        pthread_cond_wait(&b->not_full, &b->mutex);
    }
    if (b->shutting_down) { pthread_mutex_unlock(&b->mutex); return -1; }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0) {
        if (b->shutting_down) { pthread_mutex_unlock(&b->mutex); return -1; }
        pthread_cond_wait(&b->not_empty, &b->mutex);
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Logging thread */
/* ------------------------------------------------------------------ */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char path[PATH_MAX];
    int fd;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) { perror("logging_thread: open"); continue; }
        if (write(fd, item.data, item.length) < 0)
            perror("logging_thread: write");
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Container child entrypoint */
/* ------------------------------------------------------------------ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("child_fn: sethostname");

    if (mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) {
        perror("child_fn: mount private");
        return 1;
    }
    if (chroot(cfg->rootfs) < 0) {
        perror("child_fn: chroot");
        return 1;
    }
    if (chdir("/") < 0) {
        perror("child_fn: chdir");
        return 1;
    }

    mount("proc", "/proc", "proc", 0, NULL);

    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) == -1 && errno != 0)
            perror("child_fn: nice");
    }

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);
    perror("child_fn: execv");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Monitor registration */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd, const char *container_id,
                          pid_t host_pid, unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    safe_copy(req.container_id, container_id, sizeof(req.container_id));
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0) return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    safe_copy(req.container_id, container_id, sizeof(req.container_id));
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0) return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Signal handlers */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c = g_ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->state = CONTAINER_EXITED;
                    c->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    c->state = CONTAINER_KILLED;
                    c->exit_signal = WTERMSIG(status);
                }
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
    errno = saved_errno;
}

static void shutdown_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/* Pipe reader thread */
/* ------------------------------------------------------------------ */

typedef struct {
    int fd;
    char id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} pipe_reader_arg_t;

static void *pipe_reader(void *arg)
{
    pipe_reader_arg_t *p = (pipe_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(item.container_id, 0, sizeof(item.container_id));
    safe_copy(item.container_id, p->id, sizeof(item.container_id));

    while ((n = read(p->fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(p->buf, &item);
    }
    close(p->fd);
    free(p);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Launch a container */
/* ------------------------------------------------------------------ */

static int launch_container(supervisor_ctx_t *ctx,
                            const control_request_t *req,
                            control_response_t *resp)
{
    char *stack;
    pid_t pid;
    container_record_t *rec;
    int log_fds[2];
    pipe_reader_arg_t *pra;
    pthread_t reader_tid;
    pthread_attr_t attr;

    if (pipe(log_fds) < 0) {
        snprintf(resp->message, sizeof(resp->message),
                 "pipe failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }

    stack = malloc(STACK_SIZE);
    if (!stack) {
        close(log_fds[0]); close(log_fds[1]);
        snprintf(resp->message, sizeof(resp->message), "malloc stack failed");
        resp->status = -1;
        return -1;
    }

    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        free(stack);
        close(log_fds[0]); close(log_fds[1]);
        snprintf(resp->message, sizeof(resp->message), "malloc cfg failed");
        resp->status = -1;
        return -1;
    }

    safe_copy(cfg->id,      req->container_id, sizeof(cfg->id));
    safe_copy(cfg->rootfs,  req->rootfs,        sizeof(cfg->rootfs));
    safe_copy(cfg->command, req->command,       sizeof(cfg->command));
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = log_fds[1];

    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    close(log_fds[1]);

    if (pid < 0) {
        free(stack); free(cfg);
        close(log_fds[0]);
        snprintf(resp->message, sizeof(resp->message),
                 "clone failed: %s", strerror(errno));
        resp->status = -1;
        return -1;
    }

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    rec = calloc(1, sizeof(*rec));
    if (rec) {
        safe_copy(rec->id, req->container_id, sizeof(rec->id));
        rec->host_pid         = pid;
        rec->started_at       = time(NULL);
        rec->state            = CONTAINER_RUNNING;
        rec->soft_limit_bytes = req->soft_limit_bytes;
        rec->hard_limit_bytes = req->hard_limit_bytes;
        snprintf(rec->log_path, PATH_MAX, "%s/%s.log",
                 LOG_DIR, req->container_id);

        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next       = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    pra = malloc(sizeof(*pra));
    if (pra) {
        pra->fd  = log_fds[0];
        pra->buf = &ctx->log_buffer;
        memset(pra->id, 0, sizeof(pra->id));
        safe_copy(pra->id, req->container_id, sizeof(pra->id));

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&reader_tid, &attr, pipe_reader, pra);
        pthread_attr_destroy(&attr);
    } else {
        close(log_fds[0]);
    }

    snprintf(resp->message, sizeof(resp->message),
             "started container %s pid=%d", req->container_id, pid);
    resp->status = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Supervisor event loop */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    struct sigaction sa;
    int rc, client_fd;
    control_request_t req;
    control_response_t resp;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        perror("warning: cannot open /dev/container_monitor");

    /* UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path));

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) { perror("listen"); return 1; }

    /* Signal handlers */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = shutdown_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Log dir + logger thread */
    mkdir(LOG_DIR, 0755);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    fprintf(stderr, "supervisor ready on %s\n", CONTROL_PATH);

    /* Event loop */
    while (!ctx.should_stop) {
        client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        memset(&req,  0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        if (recv(client_fd, &req, sizeof(req), MSG_WAITALL) != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            launch_container(&ctx, &req, &resp);
            break;

        case CMD_PS: {
            int offset = 0;
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            while (c && offset < CONTROL_MESSAGE_LEN - 60) {
                offset += snprintf(resp.message + offset,
                                   CONTROL_MESSAGE_LEN - offset,
                                   "%s %s pid=%d\n",
                                   c->id, state_to_string(c->state),
                                   c->host_pid);
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            if (offset == 0)
                snprintf(resp.message, sizeof(resp.message), "(no containers)");
            resp.status = 0;
            break;
        }

        case CMD_STOP: {
            int found = 0;
            pthread_mutex_lock(&ctx.metadata_lock);
            container_record_t *c = ctx.containers;
            while (c) {
                if (strcmp(c->id, req.container_id) == 0) {
                    kill(c->host_pid, SIGTERM);
                    c->state = CONTAINER_STOPPED;
                    found = 1;
                    break;
                }
                c = c->next;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            resp.status = found ? 0 : -1;
            snprintf(resp.message, sizeof(resp.message),
                     found ? "stopped %s" : "not found: %s",
                     req.container_id);
            break;
        }

        case CMD_LOGS: {
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s.log",
                     LOG_DIR, req.container_id);
            int lfd = open(path, O_RDONLY);
            if (lfd < 0) {
                snprintf(resp.message, sizeof(resp.message),
                         "no log for %s", req.container_id);
                resp.status = -1;
            } else {
                char buf[4096];
                ssize_t n;
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "log follows");
                send(client_fd, &resp, sizeof(resp), 0);
                while ((n = read(lfd, buf, sizeof(buf))) > 0)
                    send(client_fd, buf, (size_t)n, 0);
                close(lfd);
                close(client_fd);
                continue;
            }
            break;
        }

        default:
            snprintf(resp.message, sizeof(resp.message), "unknown command");
            resp.status = -1;
        }

        send(client_fd, &resp, sizeof(resp), 0);
        close(client_fd);
    }

    fprintf(stderr, "supervisor shutting down\n");
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    pthread_mutex_destroy(&ctx.metadata_lock);
    g_ctx = NULL;
    return 0;
}

/* ------------------------------------------------------------------ */
/* CLI client */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    safe_copy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path));

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s\n"
                        "Is the supervisor running?\n", CONTROL_PATH);
        close(fd);
        return 1;
    }

    send(fd, req, sizeof(*req), 0);

    memset(&resp, 0, sizeof(resp));
    if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp))
        printf("%s\n", resp.message);

    if (req->kind == CMD_LOGS && resp.status == 0) {
        char buf[4096];
        ssize_t n;
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, (size_t)n, stdout);
    }

    close(fd);
    return resp.status == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* CLI command handlers */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <command> [...]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    safe_copy(req.container_id, argv[2], sizeof(req.container_id));
    safe_copy(req.rootfs,       argv[3], sizeof(req.rootfs));
    safe_copy(req.command,      argv[4], sizeof(req.command));
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <command> [...]\n", argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    safe_copy(req.container_id, argv[2], sizeof(req.container_id));
    safe_copy(req.rootfs,       argv[3], sizeof(req.rootfs));
    safe_copy(req.command,      argv[4], sizeof(req.command));
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    safe_copy(req.container_id, argv[2], sizeof(req.container_id));
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    safe_copy(req.container_id, argv[2], sizeof(req.container_id));
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/* main */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
