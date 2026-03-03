/*
 * slash_commands.c — Slash command parser and dispatcher
 */
#include "slash_commands.h"
#include "../core/log.h"
#include "../agent/agent.h"
#include "../agent/tool_registry.h"
#include "../providers/provider.h"
#include "../tools/skills/skills.h"
#include "../tools/scheduler/scheduler.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* Helpers */

/* Append to fixed output buffer safely, returning remaining space. */
static int buf_append(char *buf, size_t buf_size, const char *fmt, ...) {
    size_t used = strlen(buf);
    if (used >= buf_size - 1) return 0;
    va_list ap;
    va_start(ap, fmt);
    int written = vsnprintf(buf + used, buf_size - used, fmt, ap);
    va_end(ap);
    return written;
}

/* Command handlers */

static void cmd_help(char *out, size_t sz) {
    snprintf(out, sz,
        "━━━ Aham Commands ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  /tools            — List all registered tools\n"
        "  /skills           — List loaded skills\n"
        "  /skills reload    — Re-scan skills/ directory from disk\n"
        "  /status           — Agent info (provider, model, messages)\n"
        "  /tasks            — List active background (scheduler) tasks\n"
        "  /tmux             — List active tmux sessions\n"
        "  /new              — Start a fresh conversation\n"
        "  /model            — Show current model\n"
        "  /usage            — Conversation statistics\n"
        "  /help             — Show this help\n"
        "  /quit             — Exit Aham\n"
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_status(char *out, size_t sz, slash_ctx_t *ctx) {
    provider_t *p = (provider_t *)ctx->provider;
    snprintf(out, sz,
        "━━━ Aham Status ━━━\n"
        "  Version  : %s\n"
        "  Provider : %s\n"
        "  Model    : %s\n"
        "  Messages : %d\n"
        "  Tools    : %d registered\n"
        "  Skills   : %d loaded\n"
        "━━━━━━━━━━━━━━━━━━━━\n",
        VERSION_STRING,
        p ? p->name  : "none",
        p ? p->model : "none",
        ctx->agent   ? agent_message_count(ctx->agent) : 0,
        tool_count(),
        ctx->skills  ? skills_count((skills_t *)ctx->skills) : 0);
}

static void cmd_tools(char *out, size_t sz) {
    out[0] = '\0';
    int n = tool_count();
    buf_append(out, sz, "━━━ Tools (%d) ━━━\n", n);
    for (int i = 0; i < n; i++) {
        const tool_t *t = tool_get(i);
        if (!t) continue;
        buf_append(out, sz, "  \033[36m%-24s\033[0m %s\n", t->name, t->description);
    }
    buf_append(out, sz, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

static void cmd_skills(char *out, size_t sz, slash_ctx_t *ctx) {
    out[0] = '\0';
    skills_t *sk = (skills_t *)ctx->skills;
    int n = skills_count(sk);
    if (n == 0) {
        snprintf(out, sz, "No skills loaded. Add SKILL.md files to the skills/ directory.\n");
        return;
    }
    buf_append(out, sz, "━━━ Skills (%d) ━━━\n", n);
    for (int i = 0; i < n; i++) {
        const char *name = skills_name(sk, i);
        const char *desc = skills_description(sk, i);
        /* Print with availability indicator */
        buf_append(out, sz, "  \033[35m%-24s\033[0m %s\n", name, desc ? desc : "");
    }
    buf_append(out, sz,
        "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
        "  \033[2mUse /skills reload to refresh from disk.\033[0m\n");
}

static void cmd_skills_reload(char *out, size_t sz, slash_ctx_t *ctx) {
    skills_t *sk = (skills_t *)ctx->skills;
    if (!sk) { snprintf(out, sz, "Skills system not available.\n"); return; }
    result_t r = skills_reload(sk);
    if (r.status == OK)
        snprintf(out, sz, "✓ Skills reloaded. %d skill(s) loaded.\n",
                 skills_count(sk));
    else
        snprintf(out, sz, "Reload failed: %s\n", r.message ? r.message : "unknown error");
    result_free(&r);
}

static void cmd_tasks(char *out, size_t sz, slash_ctx_t *ctx) {
    scheduler_t *sched = (scheduler_t *)ctx->scheduler;
    out[0] = '\0';
    buf_append(out, sz, "━━━ Background Tasks ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    scheduler_task_snapshot(sched, out + strlen(out), sz - strlen(out));
    buf_append(out, sz, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
                        "  \033[2mUse schedule_add / schedule_control tools to manage.\033[0m\n");
}

static void cmd_tmux(char *out, size_t sz) {
    out[0] = '\0';
    buf_append(out, sz, "━━━ tmux Sessions ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    const char *sock = getenv("TMUX_SOCKET");
    char cmd[256];
    if (sock && *sock)
        snprintf(cmd, sizeof(cmd), "tmux -S '%s' ls 2>&1", sock);
    else
        snprintf(cmd, sizeof(cmd), "tmux ls 2>&1");

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        buf_append(out, sz, "  tmux not available.\n");
    } else {
        char line[256];
        int found = 0;
        while (fgets(line, sizeof(line), fp)) {
            /* Skip tmux error lines (no server / no sessions) */
            if (strstr(line, "no server running") ||
                strstr(line, "no sessions") ||
                strstr(line, "error")) {
                buf_append(out, sz, "  No active tmux sessions.\n");
                found = -1; /* sentinel: already handled */
                break;
            }
            buf_append(out, sz, "  \033[36m%s\033[0m", line);
            found++;
        }
        pclose(fp);
        if (found == 0)
            buf_append(out, sz, "  No active tmux sessions.\n");
    }
    buf_append(out, sz, "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
}

/* Dispatcher */

bool slash_handle(const char *input, slash_ctx_t *ctx,
                        char *output, size_t output_size) {
    if (!input || input[0] != '/') return false;
    output[0] = '\0';

    if (!strcmp(input, "/help")) {
        cmd_help(output, output_size);
        return true;
    }

    if (!strcmp(input, "/status")) {
        cmd_status(output, output_size, ctx);
        return true;
    }

    if (!strcmp(input, "/tools")) {
        cmd_tools(output, output_size);
        return true;
    }

    if (!strcmp(input, "/skills reload")) {
        cmd_skills_reload(output, output_size, ctx);
        return true;
    }

    if (!strcmp(input, "/skills")) {
        cmd_skills(output, output_size, ctx);
        return true;
    }

    if (!strcmp(input, "/tasks")) {
        cmd_tasks(output, output_size, ctx);
        return true;
    }

    if (!strcmp(input, "/tmux")) {
        cmd_tmux(output, output_size);
        return true;
    }

    if (!strcmp(input, "/new")) {
        if (ctx->agent) agent_new_conversation(ctx->agent);
        snprintf(output, output_size, "✨ New conversation started.\n");
        return true;
    }

    if (!strncmp(input, "/model", 6)) {
        provider_t *p = (provider_t *)ctx->provider;
        const char *arg = str_trim((char *)(input + 6));
        if (!*arg) {
            snprintf(output, output_size, "Model: %s (%s)\n",
                     p ? p->model : "none", p ? p->name : "none");
        } else {
            snprintf(output, output_size,
                "Set model = %s in aham.conf and restart.\n", arg);
        }
        return true;
    }

    if (!strcmp(input, "/usage")) {
        snprintf(output, output_size,
            "Messages in conversation: %d\n",
            ctx->agent ? agent_message_count(ctx->agent) : 0);
        return true;
    }

    if (!strcmp(input, "/quit") || !strcmp(input, "/exit")) {
        snprintf(output, output_size, "__QUIT__");
        return true;
    }

    snprintf(output, output_size,
             "Unknown command: %s  (try /help)\n", input);
    return true;
}

