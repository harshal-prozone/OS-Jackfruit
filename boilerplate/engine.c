/* engine.c - Multi-Container Runtime Supervisor */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <stdarg.h>

#include "monitor_ioctl.h"

/* ── constants ─────────────────────────────────────────────── */
#define MAX_CONTAINERS   32
#define LOG_BUF_SLOTS    512
#define LOG_LINE_MAX     1024
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine-logs"
#define STACK_SIZE       (1 << 20)   /* 1 MiB clone stack */
#define DEFAULT_SOFT_MIB 40
#define DEFAULT_HARD_MIB 64

/* ── container states ──────────────────────────────────────── */
typedef enum {
    STATE_STARTING = 0,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
    STATE_EXITED,
} ContainerState;

static const char *state_str[] = {
    "starting", "running", "stopped", "killed(hard-limit)", "exited"
};

/* ── container metadata ────────────────────────────────────── */
typedef struct {
    char           id[64];
    pid_t          pid;
    time_t         start_time;
    ContainerState state;
    long           soft_mib;
    long           hard_mib;
    char           log_path[256];
    int            exit_code;
    int            exit_signal;
    int            stop_requested;
    int            in_use;
    int            pipe_stdout[2];
    int            pipe_stderr[2];
} Container;

static Container       containers[MAX_CONTAINERS];
static pthread_mutex_t containers_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── bounded log buffer ────────────────────────────────────── */
typedef struct {
    char container_id[64];
    char line[LOG_LINE_MAX];
} LogEntry;

static LogEntry        log_buf[LOG_BUF_SLOTS];
static int             log_head     = 0;
static int             log_tail     = 0;
static int             log_count    = 0;
static pthread_mutex_t log_mutex    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  log_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  log_not_full  = PTHREAD_COND_INITIALIZER;
static volatile int    log_shutdown  = 0;

/* ── kernel monitor fd ─────────────────────────────────────── */
static int monitor_fd = -1;

/* ── forward declarations ──────────────────────────────────── */
static void  supervisor_loop(const char *base_rootfs);
static Container *find_container(const char *id);

/* ═══════════════════════════════════════════════════════════
 *  BOUNDED BUFFER — producer push / consumer pop
 * ═══════════════════════════════════════════════════════════ */

static void log_push(const char *id, const char *line)
{
    pthread_mutex_lock(&log_mutex);

    /* Block while full, unless we are shutting down */
    while (log_count == LOG_BUF_SLOTS && !log_shutdown)
        pthread_cond_wait(&log_not_full, &log_mutex);

    if (!log_shutdown) {
        LogEntry *e = &log_buf[log_tail];
        strncpy(e->container_id, id,   sizeof(e->container_id) - 1);
        strncpy(e->line,         line, sizeof(e->line)         - 1);
        e->container_id[sizeof(e->container_id) - 1] = '\0';
        e->line[sizeof(e->line) - 1]                 = '\0';
        log_tail  = (log_tail + 1) % LOG_BUF_SLOTS;
        log_count++;
        pthread_cond_signal(&log_not_empty);
    }

    pthread_mutex_unlock(&log_mutex);
}

/* Returns 1 if an entry was popped, 0 on shutdown-and-empty */
static int log_pop(LogEntry *out)
{
    pthread_mutex_lock(&log_mutex);

    while (log_count == 0 && !log_shutdown)
        pthread_cond_wait(&log_not_empty, &log_mutex);

    if (log_count == 0) {          /* shutdown with empty buffer */
        pthread_mutex_unlock(&log_mutex);
        return 0;
    }

    *out      = log_buf[log_head];
    log_head  = (log_head + 1) % LOG_BUF_SLOTS;
    log_count--;
    pthread_cond_signal(&log_not_full);
    pthread_mutex_unlock(&log_mutex);
    return 1;
}

/* ═══════════════════════════════════════════════════════════
 *  CONSUMER THREAD — writes log entries to per-container files
 * ═══════════════════════════════════════════════════════════ */

static void *consumer_thread(void *arg)
{
    (void)arg;
    LogEntry entry;

    while (log_pop(&entry)) {
        /* look up the log file path for this container */
        char log_path[256] = "";

        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].in_use &&
                strcmp(containers[i].id, entry.container_id) == 0) {
                strncpy(log_path, containers[i].log_path,
                        sizeof(log_path) - 1);
                log_path[sizeof(log_path) - 1] = '\0';
                break;
            }
        }
        pthread_mutex_unlock(&containers_mutex);

        if (log_path[0]) {
            FILE *f = fopen(log_path, "a");
            if (f) {
                fprintf(f, "%s\n", entry.line);
                fclose(f);
            }
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  PRODUCER THREAD — reads one pipe fd and feeds the buffer
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    int  fd;
    char id[64];
} ProducerArg;

static void *producer_thread(void *arg)
{
    ProducerArg *pa   = (ProducerArg *)arg;
    char         buf[LOG_LINE_MAX];
    char         line[LOG_LINE_MAX];
    int          llen = 0;
    ssize_t      n;

    while ((n = read(pa->fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n' || llen == LOG_LINE_MAX - 1) {
                line[llen] = '\0';
                if (llen > 0)
                    log_push(pa->id, line);
                llen = 0;
            } else {
                line[llen++] = buf[i];
            }
        }
    }

    /* flush any partial line at EOF */
    if (llen > 0) {
        line[llen] = '\0';
        log_push(pa->id, line);
    }

    close(pa->fd);
    free(pa);
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  CONTAINER CHILD ENTRY POINT  (runs inside clone)
 * ═══════════════════════════════════════════════════════════ */

typedef struct {
    char rootfs[256];
    char cmd[256];
    char args[8][256];
    int  argc;
    int  nice_val;
    int  pipe_stdout_w;
    int  pipe_stderr_w;
} ContainerArgs;

static int container_main(void *arg)
{
    ContainerArgs *ca = (ContainerArgs *)arg;

    /* redirect stdout / stderr into supervisor pipes */
    dup2(ca->pipe_stdout_w, STDOUT_FILENO);
    dup2(ca->pipe_stderr_w, STDERR_FILENO);
    close(ca->pipe_stdout_w);
    close(ca->pipe_stderr_w);

    /* chroot into the container rootfs */
    if (chroot(ca->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* mount /proc so the container can see its own process tree */
    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0)
        perror("mount /proc (non-fatal)");

    /* set hostname in the new UTS namespace */
    sethostname(ca->cmd, strlen(ca->cmd));

    /* apply scheduling niceness */
    if (ca->nice_val != 0)
        nice(ca->nice_val);

    /* build argv array */
    char *argv_exec[10];
    int   i;
    argv_exec[0] = ca->cmd;
    for (i = 0; i < ca->argc && i < 8; i++)
        argv_exec[i + 1] = ca->args[i];
    argv_exec[i + 1] = NULL;

    execvp(ca->cmd, argv_exec);
    perror("execvp");
    return 127;
}

/* ═══════════════════════════════════════════════════════════
 *  FIND / ALLOC CONTAINER SLOT  (call with mutex held)
 * ═══════════════════════════════════════════════════════════ */

static Container *find_container(const char *id)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].in_use && strcmp(containers[i].id, id) == 0)
            return &containers[i];
    return NULL;
}

static Container *alloc_slot(void)
{
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (!containers[i].in_use)
            return &containers[i];
    return NULL;
}

/* ═══════════════════════════════════════════════════════════
 *  LAUNCH A CONTAINER
 * ═══════════════════════════════════════════════════════════ */

static int launch_container(const char *id, const char *rootfs,
                             const char *cmd,
                             long soft_mib, long hard_mib,
                             int nice_val,
                             char *errbuf, size_t errlen)
{
    pthread_mutex_lock(&containers_mutex);

    if (find_container(id)) {
        snprintf(errbuf, errlen, "container '%s' already exists", id);
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    Container *c = alloc_slot();
    if (!c) {
        snprintf(errbuf, errlen, "too many containers (max %d)", MAX_CONTAINERS);
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    memset(c, 0, sizeof(*c));
    strncpy(c->id, id, sizeof(c->id) - 1);
    c->id[sizeof(c->id) - 1] = '\0';
    c->soft_mib   = soft_mib;
    c->hard_mib   = hard_mib;
    c->start_time = time(NULL);
    c->state      = STATE_STARTING;
    c->in_use     = 1;
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, id);

    /* create stdout and stderr pipes */
    if (pipe(c->pipe_stdout) < 0 || pipe(c->pipe_stderr) < 0) {
        snprintf(errbuf, errlen, "pipe() failed: %s", strerror(errno));
        c->in_use = 0;
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    pthread_mutex_unlock(&containers_mutex);

    /* build the ContainerArgs on the heap; child gets a COW copy */
    ContainerArgs *ca = calloc(1, sizeof(*ca));
    if (!ca) {
        snprintf(errbuf, errlen, "calloc failed");
        return -1;
    }
    strncpy(ca->rootfs, rootfs, sizeof(ca->rootfs) - 1);
    strncpy(ca->cmd,    cmd,    sizeof(ca->cmd)    - 1);
    ca->argc         = 0;
    ca->nice_val     = nice_val;
    ca->pipe_stdout_w = c->pipe_stdout[1];
    ca->pipe_stderr_w = c->pipe_stderr[1];

    /* allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        snprintf(errbuf, errlen, "malloc stack failed");
        free(ca);
        return -1;
    }

    int   flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid   = clone(container_main, stack + STACK_SIZE, flags, ca);
    /*
     * Without CLONE_VM the child gets a COW copy of the address space,
     * so freeing stack and ca in the parent is safe — the child already
     * has its own copies of those pages.
     */
    free(stack);
    free(ca);

    if (pid < 0) {
        snprintf(errbuf, errlen, "clone() failed: %s", strerror(errno));
        pthread_mutex_lock(&containers_mutex);
        close(c->pipe_stdout[0]); close(c->pipe_stdout[1]);
        close(c->pipe_stderr[0]); close(c->pipe_stderr[1]);
        c->in_use = 0;
        pthread_mutex_unlock(&containers_mutex);
        return -1;
    }

    /* close write-ends in the parent */
    close(c->pipe_stdout[1]);
    close(c->pipe_stderr[1]);

    pthread_mutex_lock(&containers_mutex);
    c->pid   = pid;
    c->state = STATE_RUNNING;
    pthread_mutex_unlock(&containers_mutex);

    /* spawn one producer thread per pipe */
    pthread_t     pt1, pt2;
    ProducerArg  *pa1 = malloc(sizeof(*pa1));
    ProducerArg  *pa2 = malloc(sizeof(*pa2));
    if (!pa1 || !pa2) {
        free(pa1); free(pa2);
        snprintf(errbuf, errlen, "malloc ProducerArg failed");
        return -1;
    }
    pa1->fd = c->pipe_stdout[0];
    pa2->fd = c->pipe_stderr[0];
    strncpy(pa1->id, id, sizeof(pa1->id) - 1);  pa1->id[sizeof(pa1->id)-1] = '\0';
    strncpy(pa2->id, id, sizeof(pa2->id) - 1);  pa2->id[sizeof(pa2->id)-1] = '\0';
    pthread_create(&pt1, NULL, producer_thread, pa1);
    pthread_create(&pt2, NULL, producer_thread, pa2);
    pthread_detach(pt1);
    pthread_detach(pt2);

    /* register with the kernel memory monitor (if loaded) */
    if (monitor_fd >= 0) {
        struct container_reg reg;
        memset(&reg, 0, sizeof(reg));
        reg.pid              = pid;
        reg.soft_limit_bytes = soft_mib * 1024L * 1024L;
        reg.hard_limit_bytes = hard_mib * 1024L * 1024L;
        strncpy(reg.id, id, MONITOR_NAME_LEN - 1);
        if (ioctl(monitor_fd, MONITOR_REGISTER, &reg) < 0)
            fprintf(stderr, "[supervisor] ioctl REGISTER failed: %s\n",
                    strerror(errno));
    }

    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  SIGCHLD HANDLER — reap children, update state
 * ═══════════════════════════════════════════════════════════ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&containers_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (!containers[i].in_use || containers[i].pid != pid)
                continue;

            if (WIFEXITED(status)) {
                containers[i].exit_code = WEXITSTATUS(status);
                containers[i].state     = STATE_EXITED;
            } else if (WIFSIGNALED(status)) {
                int s = WTERMSIG(status);
                containers[i].exit_signal = s;
                containers[i].state =
                    (s == SIGKILL && !containers[i].stop_requested)
                    ? STATE_KILLED : STATE_STOPPED;
            }

            /* unregister from kernel monitor */
            if (monitor_fd >= 0) {
                struct container_unreg req;
                memset(&req, 0, sizeof(req));
                req.pid = pid;
                strncpy(req.id, containers[i].id, MONITOR_NAME_LEN - 1);
                ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
            }
            break;
        }
        pthread_mutex_unlock(&containers_mutex);
    }
}

/* ═══════════════════════════════════════════════════════════
 *  COMMAND HANDLERS
 * ═══════════════════════════════════════════════════════════ */

/* Parse "id rootfs cmd [--soft-mib N] [--hard-mib N] [--nice N]" */
static void handle_start(char *args, char *resp, size_t rlen)
{
    char id[64]="", rootfs[256]="", cmd[256]="";
    long soft = DEFAULT_SOFT_MIB, hard = DEFAULT_HARD_MIB;
    int  nice_val = 0;
    char errbuf[256] = "";

    char *tok = strtok(args, " \t");
    if (!tok) { snprintf(resp, rlen, "ERR missing id");     return; }
    strncpy(id, tok, sizeof(id) - 1);

    tok = strtok(NULL, " \t");
    if (!tok) { snprintf(resp, rlen, "ERR missing rootfs"); return; }
    strncpy(rootfs, tok, sizeof(rootfs) - 1);

    tok = strtok(NULL, " \t");
    if (!tok) { snprintf(resp, rlen, "ERR missing cmd");    return; }
    strncpy(cmd, tok, sizeof(cmd) - 1);

    while ((tok = strtok(NULL, " \t")) != NULL) {
        if (strcmp(tok, "--soft-mib") == 0) {
            char *v = strtok(NULL, " \t");
            if (v) soft = atol(v);
        } else if (strcmp(tok, "--hard-mib") == 0) {
            char *v = strtok(NULL, " \t");
            if (v) hard = atol(v);
        } else if (strcmp(tok, "--nice") == 0) {
            char *v = strtok(NULL, " \t");
            if (v) nice_val = atoi(v);
        }
    }

    if (soft > hard) {
        snprintf(resp, rlen, "ERR soft limit cannot exceed hard limit");
        return;
    }

    mkdir(LOG_DIR, 0755);

    if (launch_container(id, rootfs, cmd, soft, hard, nice_val,
                         errbuf, sizeof(errbuf)) == 0)
        snprintf(resp, rlen, "OK started %s", id);
    else
        snprintf(resp, rlen, "ERR %s", errbuf);
}

static void handle_ps(char *resp, size_t rlen)
{
    snprintf(resp, rlen, "%-16s %-8s %-20s %-10s %-8s %-8s\n",
             "ID", "PID", "STATE", "EXIT", "SOFT_MiB", "HARD_MiB");

    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (!containers[i].in_use) continue;
        Container *c = &containers[i];

        char exit_info[16] = "-";
        if (c->state == STATE_EXITED)
            snprintf(exit_info, sizeof(exit_info), "code=%d", c->exit_code);
        else if (c->state == STATE_STOPPED || c->state == STATE_KILLED)
            snprintf(exit_info, sizeof(exit_info), "sig=%d", c->exit_signal);

        char line[256];
        snprintf(line, sizeof(line),
                 "%-16s %-8d %-20s %-10s %-8ld %-8ld\n",
                 c->id, c->pid, state_str[c->state], exit_info,
                 c->soft_mib, c->hard_mib);
        strncat(resp, line, rlen - strlen(resp) - 1);
    }
    pthread_mutex_unlock(&containers_mutex);
}

static void handle_logs(const char *id, char *resp, size_t rlen)
{
    char log_path[256] = "";

    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(id);
    if (c) {
        strncpy(log_path, c->log_path, sizeof(log_path) - 1);
        log_path[sizeof(log_path) - 1] = '\0';
    }
    pthread_mutex_unlock(&containers_mutex);

    if (!log_path[0]) {
        snprintf(resp, rlen, "ERR no container '%s'", id);
        return;
    }

    FILE *f = fopen(log_path, "r");
    if (!f) {
        snprintf(resp, rlen, "(log file not created yet)");
        return;
    }

    size_t used = 0;
    char   line[512];
    while (fgets(line, sizeof(line), f) && used < rlen - 2) {
        size_t ll = strlen(line);
        if (used + ll < rlen - 1) {
            memcpy(resp + used, line, ll);
            used += ll;
        }
    }
    resp[used] = '\0';
    fclose(f);

    if (used == 0)
        snprintf(resp, rlen, "(no output yet)");
}

static void handle_stop(const char *id, char *resp, size_t rlen)
{
    pthread_mutex_lock(&containers_mutex);
    Container *c = find_container(id);
    if (!c) {
        pthread_mutex_unlock(&containers_mutex);
        snprintf(resp, rlen, "ERR no container '%s'", id);
        return;
    }
    if (c->state != STATE_RUNNING) {
        pthread_mutex_unlock(&containers_mutex);
        snprintf(resp, rlen, "ERR container '%s' is not running", id);
        return;
    }
    c->stop_requested = 1;
    pid_t pid = c->pid;
    pthread_mutex_unlock(&containers_mutex);

    /* SIGTERM first, then SIGKILL after 3 s */
    kill(pid, SIGTERM);
    for (int i = 0; i < 30; i++) {
        usleep(100000);
        pthread_mutex_lock(&containers_mutex);
        int still = (c->state == STATE_RUNNING);
        pthread_mutex_unlock(&containers_mutex);
        if (!still) break;
    }
    pthread_mutex_lock(&containers_mutex);
    if (c->state == STATE_RUNNING)
        kill(pid, SIGKILL);
    pthread_mutex_unlock(&containers_mutex);

    snprintf(resp, rlen, "OK stopped %s", id);
}

/* ═══════════════════════════════════════════════════════════
 *  SUPERVISOR: UNIX SOCKET EVENT LOOP
 * ═══════════════════════════════════════════════════════════ */

static volatile int supervisor_running = 1;

static void sigterm_handler(int sig)
{
    (void)sig;
    supervisor_running = 0;
}

static void supervisor_loop(const char *base_rootfs)
{
    (void)base_rootfs;

    /* try to open kernel monitor device */
    monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] /dev/container_monitor not available "
                "(kernel module not loaded?)\n");
    else
        fprintf(stderr, "[supervisor] kernel monitor connected\n");

    /* install signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* start the single consumer (logger) thread */
    pthread_t consumer;
    pthread_create(&consumer, NULL, consumer_thread, NULL);

    /* create UNIX domain socket */
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); exit(1); }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    unlink(SOCK_PATH);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    if (listen(srv, 16) < 0) { perror("listen"); exit(1); }

    fprintf(stderr, "[supervisor] listening on %s\n", SOCK_PATH);

    /* non-blocking accept so SIGTERM can break the loop */
    fcntl(srv, F_SETFL, O_NONBLOCK);

    char resp[8192];

    while (supervisor_running) {
        int client = accept(srv, NULL, NULL);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(50000);
                continue;
            }
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        char cmd[1024] = {0};
        ssize_t n = read(client, cmd, sizeof(cmd) - 1);
        if (n <= 0) { close(client); continue; }
        cmd[n] = '\0';
        cmd[strcspn(cmd, "\n")] = '\0';   /* strip trailing newline */

        memset(resp, 0, sizeof(resp));

        if (strncmp(cmd, "start ", 6) == 0)
            handle_start(cmd + 6, resp, sizeof(resp));

        else if (strncmp(cmd, "run ", 4) == 0) {
            handle_start(cmd + 4, resp, sizeof(resp));
            /* append PID so the CLI client can poll for completion */
            if (strncmp(resp, "OK", 2) == 0) {
                char tok[64] = {0};
                sscanf(cmd + 4, "%63s", tok);
                pthread_mutex_lock(&containers_mutex);
                Container *c   = find_container(tok);
                pid_t      pid = c ? c->pid : -1;
                pthread_mutex_unlock(&containers_mutex);
                char *end = resp + strlen(resp);
                snprintf(end, sizeof(resp) - (size_t)(end - resp),
                         " pid=%d", pid);
            }
        }

        else if (strncmp(cmd, "ps",     2) == 0)
            handle_ps(resp, sizeof(resp));

        else if (strncmp(cmd, "logs ", 5) == 0)
            handle_logs(cmd + 5, resp, sizeof(resp));

        else if (strncmp(cmd, "stop ", 5) == 0)
            handle_stop(cmd + 5, resp, sizeof(resp));

        else
            snprintf(resp, sizeof(resp), "ERR unknown command: %s", cmd);

        write(client, resp, strlen(resp));
        close(client);
    }

    /* ── orderly shutdown ── */
    fprintf(stderr, "[supervisor] shutting down...\n");

    pthread_mutex_lock(&containers_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].in_use && containers[i].state == STATE_RUNNING) {
            containers[i].stop_requested = 1;
            kill(containers[i].pid, SIGKILL);
        }
    }
    pthread_mutex_unlock(&containers_mutex);

    sleep(1);   /* give children a moment to exit */

    /* signal consumer thread to drain and exit */
    pthread_mutex_lock(&log_mutex);
    log_shutdown = 1;
    pthread_cond_broadcast(&log_not_empty);
    pthread_cond_broadcast(&log_not_full);
    pthread_mutex_unlock(&log_mutex);

    pthread_join(consumer, NULL);

    close(srv);
    unlink(SOCK_PATH);
    if (monitor_fd >= 0) close(monitor_fd);

    fprintf(stderr, "[supervisor] exited cleanly\n");
}

/* ═══════════════════════════════════════════════════════════
 *  CLI CLIENT — sends a text command to the supervisor socket
 * ═══════════════════════════════════════════════════════════ */

static char run_id_str[64] = "";

static void run_sigint_handler(int sig)
{
    (void)sig;
    if (!run_id_str[0]) return;

    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return;

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, SOCK_PATH, sizeof(a.sun_path) - 1);

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "stop %s", run_id_str);
        write(s, msg, strlen(msg));
        char buf[256];
        read(s, buf, sizeof(buf));
        close(s);
    } else {
        close(s);
    }
}

static int cli_send(const char *cmd, char *resp_buf, size_t rlen)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor — is it running?\n");
        close(s);
        return -1;
    }

    write(s, cmd, strlen(cmd));
    ssize_t n = read(s, resp_buf, (int)rlen - 1);
    resp_buf[n > 0 ? n : 0] = '\0';
    close(s);
    return 0;
}

/* ═══════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════ */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine run   <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine ps\n"
            "  engine logs  <id>\n"
            "  engine stop  <id>\n");
        return 1;
    }

    const char *subcmd = argv[1];

    /* ── SUPERVISOR mode ── */
    if (strcmp(subcmd, "supervisor") == 0) {
        const char *base = (argc >= 3) ? argv[2] : ".";
        supervisor_loop(base);
        return 0;
    }

    /* ── CLI modes ── */
    char cmd_buf[1024] = {0};
    char resp[8192]    = {0};

    if (strcmp(subcmd, "ps") == 0) {
        if (cli_send("ps", resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "logs") == 0 && argc >= 3) {
        snprintf(cmd_buf, sizeof(cmd_buf), "logs %s", argv[2]);
        if (cli_send(cmd_buf, resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "stop") == 0 && argc >= 3) {
        snprintf(cmd_buf, sizeof(cmd_buf), "stop %s", argv[2]);
        if (cli_send(cmd_buf, resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "start") == 0 && argc >= 5) {
        snprintf(cmd_buf, sizeof(cmd_buf), "start");
        for (int i = 2; i < argc; i++) {
            strncat(cmd_buf, " ",    sizeof(cmd_buf) - strlen(cmd_buf) - 1);
            strncat(cmd_buf, argv[i], sizeof(cmd_buf) - strlen(cmd_buf) - 1);
        }
        if (cli_send(cmd_buf, resp, sizeof(resp)) == 0)
            printf("%s\n", resp);
        return 0;
    }

    if (strcmp(subcmd, "run") == 0 && argc >= 5) {
        strncpy(run_id_str, argv[2], sizeof(run_id_str) - 1);
        signal(SIGINT,  run_sigint_handler);
        signal(SIGTERM, run_sigint_handler);

        snprintf(cmd_buf, sizeof(cmd_buf), "run");
        for (int i = 2; i < argc; i++) {
            strncat(cmd_buf, " ",    sizeof(cmd_buf) - strlen(cmd_buf) - 1);
            strncat(cmd_buf, argv[i], sizeof(cmd_buf) - strlen(cmd_buf) - 1);
        }
        if (cli_send(cmd_buf, resp, sizeof(resp)) < 0) return 1;
        printf("%s\n", resp);

        /* poll ps until this container is no longer "running" */
        if (strncmp(resp, "OK", 2) == 0) {
            const char *id = argv[2];
            while (1) {
                sleep(1);
                char ps_resp[4096] = {0};
                if (cli_send("ps", ps_resp, sizeof(ps_resp)) < 0) break;

                /* locate the line for this container */
                char *line = strstr(ps_resp, id);
                if (!line || !strstr(line, "running")) break;
            }
        }
        return 0;
    }

    fprintf(stderr, "Unknown or incomplete subcommand: %s\n", subcmd);
    return 1;
}
