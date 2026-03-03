/*
 * shell_tool.c — Shell command execution with allowlist and safe timeout.
 *
 * CRASH FIX: The previous implementation used alarm() + popen().  alarm()
 * delivers SIGALRM to the process, which by default terminates it — exactly
 * what caused the "Alarm clock" crash.  This version replaces the approach
 * with fork() + pipe() + waitpid() with WNOHANG polling so we can enforce
 * a hard wall-clock deadline without installing any signal handler and
 * without interfering with other subsystems (scheduler thread, curl, etc.)
 * that may also rely on SIGALRM-free operation.
 *
 * Embedded-friendly choices:
 *   - Fixed 4 KB read buffer (stack-allocated) — no large heap allocations
 *     during I/O.
 *   - Output capped at MAX_OUTPUT_SIZE; excess is discarded, not buffered.
 *   - nanosleep(100 ms) polling keeps CPU idle between waitpid checks.
 */
#include "shell_tool.h"
#include "../../core/aham.h"
#include "../../core/log.h"
#include "../../core/cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_OUTPUT_SIZE (32 * 1024)  /* 32 KB default; override via aham.conf [tools] max_output_bytes */
#define DEFAULT_TIMEOUT 30           /* seconds */
#define POLL_INTERVAL_MS 100         /* waitpid polling interval */

static allowlist_t *g_shell_allowlist = NULL;

/* cJSON error helper */

static char *json_error(const char *msg, const char *detail) {
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "error", msg);
    if (detail) cJSON_AddStringToObject(obj, "detail", detail);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}

/* Safe timed execution */

/**
 * shell_run_timed - Fork, exec via sh, collect output, enforce deadline.
 *
 * Creates a pipe, forks a child that runs `sh -c command`, and reads from
 * the pipe in the parent.  The parent polls waitpid() every POLL_INTERVAL_MS
 * milliseconds.  If the child has not exited within `timeout_sec` seconds
 * the parent sends SIGKILL and returns exit_code = -1 with a timeout note.
 *
 * @command:     Shell command string passed to "sh -c".
 * @timeout_sec: Hard deadline in seconds (1–300).
 * @out_buf:     Receives heap-allocated NUL-terminated output string.
 *               Caller must free().  Set even on timeout/error.
 * @out_exit:    Receives the process exit code, or -1 on signal/timeout.
 * Returns true on success (child ran and was reaped), false on fork/pipe
 * failure (in which case out_buf contains a short error description).
 */
static bool shell_run_timed(const char *command, int timeout_sec,
                             char **out_buf, int *out_exit) {
    *out_exit = -1;
    *out_buf  = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        *out_buf = strdup(strerror(errno));
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        *out_buf = strdup(strerror(errno));
        return false;
    }

    if (pid == 0) {
        /* Child */
        close(pipefd[0]);
        /* Redirect stdout and stderr to the write end of the pipe. */
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127); /* execl failed */
    }

    /* Parent */
    close(pipefd[1]); /* Close write end in parent */

    strbuf_t sb;
    strbuf_init(&sb, 2048);

    /* Set the read end non-blocking so we can interleave polling. */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    char buf[4096];
    bool timed_out = false;
    struct timespec deadline, now, sleep_ts;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout_sec;

    sleep_ts.tv_sec  = 0;
    sleep_ts.tv_nsec = POLL_INTERVAL_MS * 1000000L;

    for (;;) {
        /* Drain available pipe data */
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
            if (sb.len < MAX_OUTPUT_SIZE) {
                size_t take = (size_t)n;
                if (sb.len + take > MAX_OUTPUT_SIZE) take = MAX_OUTPUT_SIZE - sb.len;
                buf[take] = '\0';
                strbuf_append(&sb, buf);
            }
        }

        /* Check if child has exited */
        int wstatus = 0;
        pid_t ret = waitpid(pid, &wstatus, WNOHANG);
        if (ret == pid) {
            *out_exit = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : -1;
            break;
        }

        /* Check deadline */
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec &&
             now.tv_nsec >= deadline.tv_nsec)) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            timed_out = true;
            break;
        }

        nanosleep(&sleep_ts, NULL);
    }

    /* One last drain after child exits */
    {
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
            if (sb.len < MAX_OUTPUT_SIZE) {
                size_t take = (size_t)n;
                if (sb.len + take > MAX_OUTPUT_SIZE) take = MAX_OUTPUT_SIZE - sb.len;
                buf[take] = '\0';
                strbuf_append(&sb, buf);
            }
        }
    }
    close(pipefd[0]);

    if (timed_out) {
        strbuf_append(&sb, "\n[aham: command timed out and was killed]");
    } else if (sb.len >= MAX_OUTPUT_SIZE) {
        strbuf_append(&sb, "\n[aham: output truncated]");
    }

    *out_buf = strdup(sb.data);
    strbuf_free(&sb);
    return true;
}

/* Tool execute callback */

static char *shell_execute(const cJSON *args, void *user_data) {
    (void)user_data;

    const char *command = cJSON_GetStringValue(
        cJSON_GetObjectItem(args, "command"));
    if (!command || !*command) {
        return json_error("'command' parameter required", NULL);
    }

    cJSON *tv = cJSON_GetObjectItem(args, "timeout");
    int timeout = (tv && cJSON_IsNumber(tv)) ? tv->valueint : DEFAULT_TIMEOUT;
    if (timeout <= 0 || timeout > 300) timeout = DEFAULT_TIMEOUT;

    /* Allowlist check */
    if (g_shell_allowlist &&
        allowlist_is_enabled(g_shell_allowlist, ACL_COMMAND)) {
        if (!allowlist_check(g_shell_allowlist, ACL_COMMAND, command)) {
            LOG_WARN("Shell command BLOCKED by allowlist: %.200s", command);
            return json_error("Command not permitted by allowlist", command);
        }
    }

    LOG_DEBUG("Shell exec (timeout %ds): %.200s", timeout, command);

    char *raw_output = NULL;
    int   exit_code  = -1;

    if (!shell_run_timed(command, timeout, &raw_output, &exit_code)) {
        char *err = json_error("Failed to spawn process", raw_output);
        free(raw_output);
        return err;
    }

    /* Sanitize output: strip ANSI escape codes and control characters.
     * Raw terminal output from embedded tools (dmesg, top, serial) contains
     * ESC sequences and \r that cause JSON parse failures in some upstream
     * providers when cJSON encodes them as \uXXXX control escapes. */
    char *output = sanitize_output(raw_output);
    free(raw_output);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "command",   command);
    cJSON_AddNumberToObject(result, "exit_code", exit_code);
    cJSON_AddStringToObject(result, "output",    output);
    free(output);

    char *json = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return json;
}

/* Registration */

void shell_tool_register(allowlist_t *al) {
    g_shell_allowlist = al;

    static const tool_t tool = {
        .name        = "shell_exec",
        .description = "Execute a shell command and return its output (stdout + stderr). "
                       "The command runs in a sandboxed child process with a hard timeout.",
        .parameters_schema =
            "{\"type\":\"object\",\"properties\":{"
            "\"command\":{\"type\":\"string\","
              "\"description\":\"The shell command to execute\"},"
            "\"timeout\":{\"type\":\"integer\","
              "\"description\":\"Timeout in seconds (default 30, max 300)\"}"
            "},\"required\":[\"command\"]}",
        .execute   = shell_execute,
        .user_data = NULL,
    };
    tool_register(&tool);
    LOG_DEBUG("Shell tool registered (fork+waitpid timeout, no SIGALRM)");
}
