/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
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
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/prctl.h>
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
    char rootfs[PATH_MAX];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    FILE *log_file;
    void *stack_ptr;
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
    pthread_t *producer_threads;
    int producer_count;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int fd;
    char id[CONTAINER_ID_LEN];
} pipe_arg_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
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

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * Implement producer-side insertion into the bounded buffer.
 */
/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count >= LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Implement consumer-side removal from the bounded buffer.
 */
/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/*
 * Implement the logging consumer thread.
 */
/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    container_record_t *container;

    while (1) {
        int pop_rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (pop_rc != 0) {
            /* Drain any remaining items during shutdown */
            if (ctx->log_buffer.shutting_down && ctx->log_buffer.count > 0) {
                continue;
            }
            break;
        }

        /* Find matching container and use cached file handle */
        pthread_mutex_lock(&ctx->metadata_lock);
        for (container = ctx->containers; container; container = container->next) {
            if (strcmp(container->id, item.container_id) == 0) {
                if (container->log_file) {
                    size_t written = fwrite(item.data, 1, item.length, container->log_file);
                    if (written != item.length) {
                        fprintf(stderr, "Warning: short write to log for %s\n", item.container_id);
                    }
                    if (fflush(container->log_file) < 0) {
                        fprintf(stderr, "Warning: flush error on log for %s\n", item.container_id);
                    }
                }
                break;
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    return NULL;
}

/*
 * Implement the clone child entrypoint.
 */
/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;
    setpgid(0, 0);
    char *shell_argv[] = {(char *)"/bin/sh", (char *)"-c", cfg->command, NULL};
    int rc;

    /* Redirect stdout/stderr to logging pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        return 1;
    }
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Isolate mount propagation to prevent leaks to host */
    rc = mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    if (rc < 0) {
        perror("mount MS_PRIVATE");
        return 1;
    }

    /* Change root filesystem */
    if (chroot(cfg->rootfs)!=0){
    	perror("chroot failed");
    	exit(1);
    }
    
    if (chdir("/") !=0) {
    	perror("chdir failed");
    	exit(1);
    }

    /* Create /proc directory and mount proc filesystem */
    if (mkdir("/proc", 0555) < 0 && errno != EEXIST) {
        perror("mkdir /proc");
        return 1;
    }
    rc = mount("proc", "/proc", "proc", 0, NULL);
    if (rc < 0) {
        perror("mount /proc");
        return 1;
    }

    /* Set hostname to container ID for UTS namespace */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0) {
        perror("sethostname");
        return 1;
    }

    /* Set nice level if specified */
    if (cfg->nice_value != 0) {
        errno = 0;
        if (nice(cfg->nice_value) < 0 && errno != 0) {
            perror("nice");
        }
    }

    /* Set process name for debugging */
    prctl(PR_SET_NAME, (unsigned long)cfg->id);

    /* Execute the command */
    extern char **environ;
    execve("/bin/sh", shell_argv, environ);
    perror("execve");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static supervisor_ctx_t *g_ctx = NULL;

static void sigchld_handler(int sig)
{
    pid_t pid;
    int status;
    container_record_t *c;

    (void)sig;

    /* Reap all exited children */
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_ctx->metadata_lock);
        for (c = g_ctx->containers; c; c = c->next) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (c->exit_signal == SIGKILL && !c->stop_requested) {
                        c->state = CONTAINER_KILLED;
                    } else if (c->stop_requested) {
                        c->state = CONTAINER_STOPPED;
                    } else {
                        c->state = CONTAINER_KILLED;
                    }
                }
                break;
            }
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigint_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

void *pipe_reader_thread(void *arg)
{
    pipe_arg_t *p = (pipe_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while ((n = read(p->fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = n;
        strncpy(item.container_id, p->id, CONTAINER_ID_LEN);

        bounded_buffer_push(&g_ctx->log_buffer, &item);
    }

    close(p->fd);
    free(p);
    return NULL;
}

/*
 * Implement the long-running supervisor process
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc, sock_fd, client_fd;
    struct sockaddr_un addr;
    control_request_t req;
    control_response_t resp;
    struct sigaction sa;
    socklen_t addrlen;
    pid_t pid;
    void *stack;
    child_config_t child_cfg;
    int pipes[2];
    container_record_t *container, *tmp;
    int log_fd;
    int list_remove = 0;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Create logs directory */
    mkdir(LOG_DIR, 0755);

    /* Open monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        fprintf(stderr, "Warning: Failed to open /dev/container_monitor\n");
    }

    /* Setup UNIX socket for control requests */
    unlink(CONTROL_PATH);
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock_fd);
        goto cleanup;
    }

    if (listen(sock_fd, 5) < 0) {
        perror("listen");
        close(sock_fd);
        goto cleanup;
    }

    ctx.server_fd = sock_fd;

    /* Start logger thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create");
        close(sock_fd);
        goto cleanup;
    }

    /* Setup signal handlers */
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = sigchld_handler;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    printf("Supervisor started. Base rootfs: %s\n", rootfs);

    /* Main event loop */
    while (!ctx.should_stop) {
        /* Handle completed reads with timeout */
        fd_set readfds;
        struct timeval tv;
        FD_ZERO(&readfds);
        FD_SET(sock_fd, &readfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(sock_fd + 1, &readfds, NULL, NULL, &tv);
        if (sel <= 0)
            continue;

        addrlen = sizeof(addr);
        client_fd = accept(sock_fd, (struct sockaddr *)&addr, &addrlen);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        if (recv(client_fd, &req, sizeof(req), 0) <= 0) {
            close(client_fd);
            continue;
        }

        memset(&resp, 0, sizeof(resp));
        resp.status = 0;

        if (req.kind == CMD_START) {
            /* Check for duplicate container ID - allow reuse if not RUNNING */
            pthread_mutex_lock(&ctx.metadata_lock);
            for (container = ctx.containers; container; container = container->next) {
                if (strcmp(container->id, req.container_id) == 0) {
                    if (container->state == CONTAINER_RUNNING) {
                        snprintf(resp.message, sizeof(resp.message), "Container %s is still running", req.container_id);
                        resp.status = 1;
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                            perror("send");
                        close(client_fd);
                        goto next_request;
                    } else {
                        /* Remove old container record to allow reuse */
                        list_remove = 1;
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            
            /* If old container found, remove it */
            if (list_remove) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *prev = NULL;
                for (container = ctx.containers; container; prev = container, container = container->next) {
                    if (strcmp(container->id, req.container_id) == 0) {
                        if (prev)
                            prev->next = container->next;
                        else
                            ctx.containers = container->next;
                        if (container->log_file)
                            fclose(container->log_file);
                        if (container->stack_ptr)
                            free(container->stack_ptr);
                        free(container);
                        break;
                    }
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
                list_remove = 0;
            }

            /* Check for rootfs uniqueness (correct comparison) */
            pthread_mutex_lock(&ctx.metadata_lock);
            for (container = ctx.containers; container; container = container->next) {
                if (container->state == CONTAINER_RUNNING && strcmp(container->rootfs, req.rootfs) == 0) {
                    snprintf(resp.message, sizeof(resp.message), "Rootfs %s already in use", req.rootfs);
                    resp.status = 1;
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                        perror("send");
                    close(client_fd);
                    goto next_request;
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
            /* Create pipe for container output */
            if (pipe(pipes) < 0) {
                strncpy(resp.message, "Failed to create pipe", sizeof(resp.message) - 1);
                resp.status = 1;
                send(client_fd, &resp, sizeof(resp), 0);
                close(client_fd);
                continue;
            }

            /* Allocate container metadata */
            container = (container_record_t *)malloc(sizeof(*container));
            if (!container) {
                strncpy(resp.message, "Memory allocation failed", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                close(pipes[1]);
                send(client_fd, &resp, sizeof(resp), 0);
                close(client_fd);
                continue;
            }

            memset(container, 0, sizeof(*container));
            strncpy(container->id, req.container_id, sizeof(container->id) - 1);
            strncpy(container->rootfs, req.rootfs, sizeof(container->rootfs) - 1);
            container->soft_limit_bytes = req.soft_limit_bytes;
            container->hard_limit_bytes = req.hard_limit_bytes;
            container->started_at = time(NULL);
            container->state = CONTAINER_STARTING;

            /* Setup log file and cache handle */
            snprintf(container->log_path, sizeof(container->log_path), "%s/%s.log", LOG_DIR, req.container_id);
            container->log_file = fopen(container->log_path, "a");
            if (!container->log_file) {
                strncpy(resp.message, "Failed to create log file", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                close(pipes[1]);
                free(container);
                if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                    perror("send");
                close(client_fd);
                goto next_request;
            }

            /* Allocate per-container stack to avoid reuse */
            stack = malloc(STACK_SIZE);
            if (!stack) {
                strncpy(resp.message, "Failed to allocate stack", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                close(pipes[1]);
                free(container);
                if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                    perror("send");
                close(client_fd);
                goto next_request;
            }

            /* Store stack pointer in container for later cleanup */
            container->stack_ptr = stack;

            /* Prepare child config */
            memset(&child_cfg, 0, sizeof(child_cfg));
            strncpy(child_cfg.id, req.container_id, sizeof(child_cfg.id) - 1);
            strncpy(child_cfg.rootfs, req.rootfs, sizeof(child_cfg.rootfs) - 1);
            strncpy(child_cfg.command, req.command, sizeof(child_cfg.command) - 1);
            child_cfg.nice_value = req.nice_value;
            child_cfg.log_write_fd = pipes[1];

            /* Clone with namespaces (PID, UTS, mount, IPC, network, user isolation) */
            pid = clone(child_fn, (char *)stack + STACK_SIZE,
                       CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD,
                       &child_cfg);

            close(pipes[1]);

            if (pid < 0) {
                strncpy(resp.message, "Failed to clone container", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                free(container);
                if (stack)
                    free(stack);
            } else {
                container->host_pid = pid;
                container->state = CONTAINER_RUNNING;

                /* Register with kernel monitor */
                if (ctx.monitor_fd >= 0) {
                    if (register_with_monitor(ctx.monitor_fd, req.container_id, pid,
                                            req.soft_limit_bytes, req.hard_limit_bytes) < 0) {
                        fprintf(stderr, "Warning: Failed to register container %s with monitor\n", req.container_id);
                    }
                }

                /* Add to tracking list */
                pthread_mutex_lock(&ctx.metadata_lock);
                container->next = ctx.containers;
                ctx.containers = container;
                pthread_mutex_unlock(&ctx.metadata_lock);

                /* Start pipe reader thread for logging */
                pthread_t reader_tid;
                pipe_arg_t *p = malloc(sizeof(pipe_arg_t));
                if (p) {
                    p->fd = pipes[0];
                    strncpy(p->id, req.container_id, CONTAINER_ID_LEN);
                    if (pthread_create(&reader_tid, NULL, pipe_reader_thread, p) == 0) {
                        /* Track producer thread for later joining */
                        ctx.producer_threads = realloc(ctx.producer_threads,
                                                      (ctx.producer_count + 1) * sizeof(pthread_t));
                        ctx.producer_threads[ctx.producer_count++] = reader_tid;
                    } else {
                        close(pipes[0]);
                        free(p);
                    }
                } else {
                    close(pipes[0]);
                }

                snprintf(resp.message, sizeof(resp.message), "Container %s started with PID %d",
                        req.container_id, pid);
            }
        } else if (req.kind == CMD_PS) {
            snprintf(resp.message, sizeof(resp.message),
                    "Container ID          PID    State       StartTime\n");
            pthread_mutex_lock(&ctx.metadata_lock);
            for (container = ctx.containers; container; container = container->next) {
                char time_str[32];
                struct tm *tm_info = localtime(&container->started_at);
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

                size_t current_len = strlen(resp.message);
                size_t remaining = sizeof(resp.message) - current_len;
                
                /* Stop if not enough space (conservative: 64 bytes per row) */
                if (remaining < 80) {
                    strncat(resp.message, "... (truncated)\n", remaining - 1);
                    break;
                }

                snprintf(resp.message + current_len,
                        remaining,
                        "%-20s %5d  %-10s %s\n",
                        container->id, container->host_pid,
                        state_to_string(container->state), time_str);
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        } else if (req.kind == CMD_LOGS) {
            /* Read log file contents and send back */
            char log_path[PATH_MAX];
            snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req.container_id);
            FILE *logfile = fopen(log_path, "r");
            if (!logfile) {
                snprintf(resp.message, sizeof(resp.message), "Log file not found for container %s", req.container_id);
                resp.status = 1;
            } else {
                /* Read file and append to response (up to buffer limit) */
                memset(resp.message, 0, sizeof(resp.message));
                size_t bytes_read = fread(resp.message, 1, sizeof(resp.message) - 1, logfile);
                if (bytes_read > 0) {
                    resp.message[bytes_read] = '\n';
                } else if (ferror(logfile)) {
                    snprintf(resp.message, sizeof(resp.message), "Error reading log file");
                    resp.status = 1;
                } else {
                    snprintf(resp.message, sizeof(resp.message), "(empty log)");
                }
                fclose(logfile);
            }
        } else if (req.kind == CMD_RUN) {
            /* RUN is like START but we block until exit */
            /* Check for duplicate container ID - allow reuse if not RUNNING */
            pthread_mutex_lock(&ctx.metadata_lock);
            for (container = ctx.containers; container; container = container->next) {
                if (strcmp(container->id, req.container_id) == 0) {
                    if (container->state == CONTAINER_RUNNING) {
                        snprintf(resp.message, sizeof(resp.message), "Container %s is still running", req.container_id);
                        resp.status = 1;
                        pthread_mutex_unlock(&ctx.metadata_lock);
                        if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                            perror("send");
                        close(client_fd);
                        goto next_request;
                    } else {
                        /* Remove old container record to allow reuse */
                        list_remove = 1;
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            /* If old container found, remove it */
            if (list_remove) {
                pthread_mutex_lock(&ctx.metadata_lock);
                container_record_t *prev = NULL;
                for (container = ctx.containers; container; prev = container, container = container->next) {
                    if (strcmp(container->id, req.container_id) == 0) {
                        if (prev)
                            prev->next = container->next;
                        else
                            ctx.containers = container->next;
                        if (container->log_file)
                            fclose(container->log_file);
                        if (container->stack_ptr)
                            free(container->stack_ptr);
                        free(container);
                        break;
                    }
                }
                pthread_mutex_unlock(&ctx.metadata_lock);
                list_remove = 0;
            }

            /* Check for rootfs uniqueness (correct comparison) */
            pthread_mutex_lock(&ctx.metadata_lock);
            for (container = ctx.containers; container; container = container->next) {
                if (container->state == CONTAINER_RUNNING && strcmp(container->rootfs, req.rootfs) == 0) {
                    snprintf(resp.message, sizeof(resp.message), "Rootfs %s already in use", req.rootfs);
                    resp.status = 1;
                    pthread_mutex_unlock(&ctx.metadata_lock);
                    if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                        perror("send");
                    close(client_fd);
                    goto next_request;
                }
            }
            pthread_mutex_unlock(&ctx.metadata_lock);

            if (pipe(pipes) < 0) {
                strncpy(resp.message, "Failed to create pipe", sizeof(resp.message) - 1);
                resp.status = 1;
                if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                    perror("send");
                close(client_fd);
                goto next_request;
            }

            container = (container_record_t *)malloc(sizeof(*container));
            if (!container) {
                strncpy(resp.message, "Memory allocation failed", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                close(pipes[1]);
                if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                    perror("send");
                close(client_fd);
                goto next_request;
            }

            memset(container, 0, sizeof(*container));
            strncpy(container->id, req.container_id, sizeof(container->id) - 1);
            strncpy(container->rootfs, req.rootfs, sizeof(container->rootfs) - 1);
            container->soft_limit_bytes = req.soft_limit_bytes;
            container->hard_limit_bytes = req.hard_limit_bytes;
            container->started_at = time(NULL);
            container->state = CONTAINER_STARTING;

            snprintf(container->log_path, sizeof(container->log_path), "%s/%s.log", LOG_DIR, req.container_id);
            container->log_file = fopen(container->log_path, "a");
            if (!container->log_file) {
                strncpy(resp.message, "Failed to create log file", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                close(pipes[1]);
                free(container);
                if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                    perror("send");
                close(client_fd);
                goto next_request;
            }

            memset(&child_cfg, 0, sizeof(child_cfg));
            strncpy(child_cfg.id, req.container_id, sizeof(child_cfg.id) - 1);
            strncpy(child_cfg.rootfs, req.rootfs, sizeof(child_cfg.rootfs) - 1);
            strncpy(child_cfg.command, req.command, sizeof(child_cfg.command) - 1);
            child_cfg.nice_value = req.nice_value;
            child_cfg.log_write_fd = pipes[1];

            /* Allocate per-container stack to avoid reuse */
            stack = malloc(STACK_SIZE);
            if (!stack) {
                strncpy(resp.message, "Failed to allocate stack", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                close(pipes[1]);
                free(container);
                if (send(client_fd, &resp, sizeof(resp), 0) < 0)
                    perror("send");
                close(client_fd);
                goto next_request;
            }

            /* Store stack pointer in container for later cleanup */
            container->stack_ptr = stack;

            /* Clone with namespaces (PID, UTS, mount, IPC, network, user isolation) */
            pid = clone(child_fn, (char *)stack + STACK_SIZE,
                       CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | SIGCHLD,
                       &child_cfg);

            close(pipes[1]);

            if (pid < 0) {
                strncpy(resp.message, "Failed to clone container", sizeof(resp.message) - 1);
                resp.status = 1;
                close(pipes[0]);
                free(container);
                free(stack);
            } else {
                container->host_pid = pid;
                container->state = CONTAINER_RUNNING;

                if (ctx.monitor_fd >= 0) {
                    if (register_with_monitor(ctx.monitor_fd, req.container_id, pid,
                                            req.soft_limit_bytes, req.hard_limit_bytes) < 0) {
                        fprintf(stderr, "Warning: Failed to register container %s with monitor\n", req.container_id);
                    }
                }

                pthread_mutex_lock(&ctx.metadata_lock);
                container->next = ctx.containers;
                ctx.containers = container;
                pthread_mutex_unlock(&ctx.metadata_lock);

                pthread_t reader_tid;
                pipe_arg_t *p = malloc(sizeof(pipe_arg_t));
                if (p) {
                    p->fd = pipes[0];
                    strncpy(p->id, req.container_id, CONTAINER_ID_LEN);
                    if (pthread_create(&reader_tid, NULL, pipe_reader_thread, p) == 0) {
                        /* Track producer thread for later joining */
                        ctx.producer_threads = realloc(ctx.producer_threads,
                                                      (ctx.producer_count + 1) * sizeof(pthread_t));
                        ctx.producer_threads[ctx.producer_count++] = reader_tid;
                    } else {
                        close(pipes[0]);
                        free(p);
                    }
                } else {
                    close(pipes[0]);
                }

                /* Block until container exits and capture status directly */
                /* Block SIGCHLD to prevent race with handler reaping child */
                sigset_t mask, oldmask;
                sigemptyset(&mask);
                sigaddset(&mask, SIGCHLD);
                pthread_sigmask(SIG_BLOCK, &mask, &oldmask);

                int run_status;
                waitpid(pid, &run_status, 0);

                /* Unblock SIGCHLD */
                pthread_sigmask(SIG_SETMASK, &oldmask, NULL);

                /* Get final status directly from waitpid result (no race condition) */
                if (WIFEXITED(run_status)) {
                    resp.status = WEXITSTATUS(run_status);
                } else if (WIFSIGNALED(run_status)) {
                    resp.status = 128 + WTERMSIG(run_status);
                } else {
                    resp.status = 1;
                }
                snprintf(resp.message, sizeof(resp.message), "Container %s exited", req.container_id);
            }
        } else if (req.kind == CMD_STOP) {
            pthread_mutex_lock(&ctx.metadata_lock);
            for (container = ctx.containers; container; container = container->next) {
                if (strcmp(container->id, req.container_id) == 0) {
                    if (container->state == CONTAINER_RUNNING) {
                        container->stop_requested = 1;
                        kill(container->host_pid, SIGTERM);
                        sleep(1);
                        
                        if(kill(container->host_pid, 0) ==0){
                        	kill(container->host_pid, SIGKILL);
                        }
                        snprintf(resp.message, sizeof(resp.message), "Stopped container %s",
                                req.container_id);
                    } else {
                        snprintf(resp.message, sizeof(resp.message), "Container not running");
                        resp.status = 1;
                    }
                    break;
                }
            }
            if (!container) {
                snprintf(resp.message, sizeof(resp.message), "Container not found");
                resp.status = 1;
            }
            pthread_mutex_unlock(&ctx.metadata_lock);
        }

        if (send(client_fd, &resp, sizeof(resp), 0) < 0)
            perror("send");
        close(client_fd);

next_request:
        ;
    }

    printf("Supervisor shutting down...\n");

    /* Kill all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container = ctx.containers; container; container = container->next) {
        if (container->state == CONTAINER_RUNNING) {
            container->stop_requested =1;
            kill(container->host_pid, SIGTERM);
            sleep(1);
            
            if(kill(container->host_pid, 0) ==0) {
            	kill(container->host_pid, SIGKILL);
        }
            container->state =CONTAINER_STOPPED;
            }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for all containers to exit (with timeout) */
    for (int wait_count = 0; wait_count < 30; wait_count++) {
        pthread_mutex_lock(&ctx.metadata_lock);
        int still_running = 0;
        for (container = ctx.containers; container; container = container->next) {
            if (container->state == CONTAINER_RUNNING)
                still_running++;
        }
        pthread_mutex_unlock(&ctx.metadata_lock);
        if (still_running == 0)
            break;
        sleep(1);
    }

    /* Clean up */
cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    
    /* Join all producer threads */
    for (int i = 0; i < ctx.producer_count; i++) {
        pthread_join(ctx.producer_threads[i], NULL);
    }
    if (ctx.producer_threads)
        free(ctx.producer_threads);
    
    /* Join logger consumer thread */
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    pthread_mutex_lock(&ctx.metadata_lock);
    for (container = ctx.containers; container;) {
        tmp = container;
        container = container->next;
        /* Close cached log file */
        if (tmp->log_file) {
            fclose(tmp->log_file);
        }
        /* Free stack allocated for this container */
        if (tmp->stack_ptr) {
            free(tmp->stack_ptr);
        }
        if (ctx.monitor_fd >= 0)
            unregister_from_monitor(ctx.monitor_fd, tmp->id, tmp->host_pid);
        free(tmp);
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    if (ctx.server_fd >= 0) {
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
    }

    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/*
 * Implement the client-side control request path.
 */
/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    if (send(sock, (void *)req, sizeof(*req), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    n = recv(sock, (void *)&resp, sizeof(resp), 0);
    if (n < 0) {
        perror("recv");
        close(sock);
        return 1;
    }
    if (n > 0) {
        printf("%s\n", resp.message);
    }

    close(sock);
    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    int sock;
    struct sockaddr_un addr;
    control_response_t resp;
    ssize_t n;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    /* Connect to supervisor */
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }

    /* Send RUN request */
    if (send(sock, (void *)&req, sizeof(req), 0) < 0) {
        perror("send");
        close(sock);
        return 1;
    }

    printf("Waiting for container %s to complete...\n", argv[2]);
    printf("(Press Ctrl+C to stop)\n");

    /* Setup signal handler to gracefully stop on SIGINT */
    signal(SIGINT, SIG_IGN);  /* Temporarily ignore SIGINT */

    /* Wait for response from supervisor (blocking until container exits) */
    n = recv(sock, (void *)&resp, sizeof(resp), 0);
    
    /* Re-enable SIGINT handling */
    signal(SIGINT, SIG_DFL);

    if (n < 0) {
        perror("recv");
        close(sock);
        return 1;
    }
    if (n > 0) {
        printf("%s\n", resp.message);
    }

    close(sock);
    return resp.status;
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

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}  

