/*
 * main.c — Aham AI Agent entry point
 *
 * MODES
 *   (default)       Interactive CLI REPL with readline
 *   --daemon / -d   Headless HTTP daemon; talk via curl or browser
 *
 * LOG LEVELS
 *   Default: WARN — quiet normal operation.
 *   --log-level debug|info|warn|error   or   LOG_LEVEL env var.
 *   Env var takes precedence over the CLI flag.
 *
 * CONFIGURATION
 *   All runtime settings live in aham.conf.  The .env file is for secrets
 *   only (API_KEY and LiteLLM proxy keys) — nothing else belongs there.
 *
 *   Priority (highest → lowest):
 *     1. Shell-exported env vars
 *     2. .env file  (secrets only: API_KEY, GROQ_API_KEY, …)
 *     3. aham.conf (everything else)
 *     4. Built-in defaults
 *
 * ENV VARS (shell / .env — override the corresponding aham.conf values)
 *   API_KEY     API key for the remote LLM provider   (.env)
 *   ENV_FILE    path to .env file                     (default: .env)
 *   CONFIG      path to config file                   (default: aham.conf)
 *   LOG_LEVEL   debug|info|warn|error                 (shell only)
 *   LOG_FILE    append logs here                      (shell only)
 */
#include "../core/aham.h"
#include "../core/templates.h"
#include "../agent/agent.h"
#include "../agent/tool_registry.h"
#include "../providers/provider.h"
#include "../providers/ollama/ollama.h"
#include "../providers/openai/openai.h"
#include "../providers/registry.h"
#include "../memory/md_memory.h"
#include "../history/history.h"
#include "../security/allowlist.h"
#include "../tools/shell/shell_tool.h"
#include "../tools/file_ops/file_tool.h"
#include "../tools/scheduler/scheduler.h"
#include "../tools/skills/skills.h"
#include "../tools/serial/serial_tool.h"
#include "../heartbeat/heartbeat.h"
#include "../gateway/gateway.h"
#include "../providers/http_client.h"
#include "../ux/slash_commands.h"
#include "../ux/typing.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAS_READLINE
#  include <readline/readline.h>
#  include <readline/history.h>
#else
static char *readline(const char *prompt) {
    printf("%s", prompt);
    fflush(stdout);
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return NULL;
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
    return strdup(buf);
}
static void add_history(const char *line) { (void)line; }
#endif

/* Global singletons */

static agent_t     *g_agent     = NULL;
static provider_t  *g_provider  = NULL;
static memory_t    *g_memory    = NULL;
static history_t   *g_history   = NULL;
static allowlist_t *g_allowlist = NULL;
static scheduler_t *g_scheduler = NULL;
static skills_t    *g_skills    = NULL;
static heartbeat_t *g_heartbeat = NULL;
static gateway_t   *g_gateway   = NULL;
static int                g_memory_slot = -1;
static char               g_workspace[512] = ".";

/* Signals */

static volatile bool g_running      = true;
static volatile bool g_sigint_caught = false;

/* SIGTERM / SIGHUP → clean shutdown */
static void sig_term(int sig) { (void)sig; g_running = false; }

/* SIGINT (Ctrl-C) → cancel current readline line, do NOT exit.
 * If pressed while the agent is thinking (not in readline), it just sets the
 * flag; the loop checks it and reprints the prompt.
 * Ctrl-D (EOF) is the intended "exit" key for the CLI. */
static void sig_handler(int sig) {
    (void)sig;
    g_sigint_caught = true;
#ifdef HAS_READLINE
#ifndef __APPLE__
    /* Interrupt readline so it returns NULL immediately.
     * We distinguish this from EOF via g_sigint_caught. */
    rl_done = 1;
#endif
#endif
}

/* Agent callbacks */

static void on_thinking(const char *status, void *ud) {
    (void)ud;
    status_set_detail(STATE_THINKING, status);
}

/* Helpers */

static char *load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf)  { fclose(f); return NULL; }
    buf[fread(buf, 1, (size_t)sz, f)] = '\0';
    fclose(f);
    return buf;
}

static const char *env_or_cfg(const char *env, config_t *cfg,
                               const char *sec, const char *key,
                               const char *def) {
    const char *v = getenv(env);
    return (v && *v) ? v : config_get(cfg, sec, key, def);
}

/* Refresh memory context in system prompt slot after each turn. */
static void refresh_memory(agent_t *a) {
    if (g_memory_slot < 0 || g_memory_slot >= a->system_part_count) return;
    char *ctx = memory_get_context(g_memory);
    if (!ctx) return;
    free(a->system_parts[g_memory_slot]);
    a->system_parts[g_memory_slot] = ctx;

    if (a->message_count > 0 && !strcmp(a->messages[0].role, "system")) {
        free(a->messages[0].content);
        strbuf_t sb; strbuf_init(&sb, 4096);
        for (int i = 0; i < a->system_part_count; i++) {
            strbuf_append(&sb, a->system_parts[i]);
            strbuf_append(&sb, "\n\n");
        }
        a->messages[0].content = strdup(sb.data);
        strbuf_free(&sb);
    }
}

/* Cleanup */

static void cleanup(config_t *cfg) {
    status_shutdown();
    gateway_destroy(g_gateway);
    heartbeat_destroy(g_heartbeat);
    skills_destroy(g_skills);
    scheduler_destroy(g_scheduler);
    memory_destroy(g_memory);
    agent_destroy(g_agent);
    if (g_provider) g_provider->destroy(g_provider);
    allowlist_destroy(g_allowlist);
    tool_registry_shutdown();
    http_cleanup();
    history_destroy(g_history);
    config_free(cfg);
    log_shutdown();
}

/* Usage */

static void print_usage(const char *argv0) {
    fprintf(stderr,
        "Usage: %s [OPTIONS]\n\n"
        "Options:\n"
        "  -d, --daemon            Headless HTTP daemon mode\n"
        "  -p, --port <port>       Gateway port (default: 8080)\n"
        "      --debug             Verbose logging: tool calls with args (use --log-level info)\n"
        "  -l, --log-level <lvl>   debug | info | warn | error  (default: warn)\n"
        "  -L, --log-file <path>   Append logs to file\n"
        "  -c, --config <path>     Config file (default: aham.conf)\n"
        "  -h, --help              Show this help\n\n"
        "Daemon mode:\n"
        "  %s --daemon --port 8080\n"
        "  %s --daemon --debug\n\n"
        "  curl -X POST http://HOST:8080/api/chat \\\n"
        "       -H 'Content-Type: application/json' \\\n"
        "       -d '{\"message\":\"Check disk usage\"}'\n"
        "  # Browser: http://HOST:8080/\n\n",
        argv0, argv0, argv0);
}

/* Argument parsing */

typedef struct {
    bool        daemon_mode;
    bool        debug_mode;
    int         port;
    const char *log_level;
    const char *log_file;
    const char *config;
} cli_args_t;

static bool parse_args(int argc, char *argv[], cli_args_t *out) {
    memset(out, 0, sizeof(*out));
    const char *debug_env = getenv("DEBUG");
    if (debug_env && (!strcmp(debug_env, "1") || !strcasecmp(debug_env, "true")))
        out->debug_mode = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-h") || !strcmp(a, "--help"))    { return false; }
        else if (!strcmp(a, "-d") || !strcmp(a, "--daemon"))   { out->daemon_mode = true; }
        else if (!strcmp(a, "--debug"))                        { out->debug_mode  = true; }
        else if ((!strcmp(a, "-p") || !strcmp(a, "--port")) && i+1 < argc)
            { out->port = atoi(argv[++i]); }
        else if ((!strcmp(a, "-l") || !strcmp(a, "--log-level")) && i+1 < argc)
            { out->log_level = argv[++i]; }
        else if ((!strcmp(a, "-L") || !strcmp(a, "--log-file")) && i+1 < argc)
            { out->log_file = argv[++i]; }
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i+1 < argc)
            { out->config = argv[++i]; }
        else { fprintf(stderr, "Unknown option: %s\n", a); return false; }
    }
    return true;
}

/* Resolve log level: CLI flag > LOG_LEVEL env > [logging] level in
 * aham.conf > built-in default (warn).  cfg may be NULL (pre-config path). */
static log_level_t resolve_log_level(const char *flag, config_t *cfg) {
    const char *src = getenv("LOG_LEVEL");
    if (!src || !*src) src = flag;
    if ((!src || !*src) && cfg)
        src = config_get(cfg, "logging", "level", NULL);
    if (!src || !*src) return LOG_WARN;
    if (!strcmp(src, "debug")) return LOG_DEBUG;
    if (!strcmp(src, "info"))  return LOG_INFO;
    if (!strcmp(src, "error")) return LOG_ERROR;
    return LOG_WARN;
}

/* Common startup */

static config_t *boot(const cli_args_t *args) {
    /* .env file — loaded first so its vars feed both log init and aham.conf.
     * Only secrets (API_KEY, proxy keys) should live here now.
     * Shell-exported vars always take precedence (setenv overwrite=0).
     * Path: ENV_FILE env var > default ".env". */
    {
        const char *env_path = getenv("ENV_FILE");
        if (!env_path || !*env_path) env_path = ".env";
        env_load(env_path);
    }

    /* Config — load before logging so [logging] section feeds log init */
    const char *cfg_path = args->config ? args->config : getenv("CONFIG");
    if (!cfg_path || !*cfg_path) cfg_path = "aham.conf";
    config_t *cfg = config_load(cfg_path);

    /* Logging — priority: CLI flag > LOG_LEVEL env > [logging] level in
     * aham.conf > built-in default (warn).  Log file follows same priority. */
    {
        const char *log_file = args->log_file
            ? args->log_file
            : (getenv("LOG_FILE")
                ? getenv("LOG_FILE")
                : config_get(cfg, "logging", "file", NULL));
        log_init(resolve_log_level(args->log_level, cfg), log_file);
    }

    /* Workspace — everything lives under here */
    const char *workspace = env_or_cfg("WORKSPACE", cfg, "paths", "workspace", ".");
    snprintf(g_workspace, sizeof(g_workspace), "%s", workspace);

    /* Set TMUX_SOCKET so child processes (shell_exec, scripts) always
     * use the private socket — prevents agent tmux sessions from hijacking
     * the user's terminal or interfering with any existing tmux server.
     * Priority: TMUX_SOCKET env var > aham.conf [tmux] socket > default */
    {
        const char *sock = env_or_cfg("TMUX_SOCKET", cfg,
                                      "tmux", "socket",
                                      "/tmp/aham-tmux.sock");
        setenv("TMUX_SOCKET", sock, 0);
        LOG_DEBUG("TMUX_SOCKET=%s", sock);
    }

    /* Seed workspace from templates (never overwrites existing files) */
    const char *tmpl_dir = env_or_cfg("TEMPLATE_DIR", cfg, "paths", "templates", "templates");
    templates_seed(tmpl_dir, workspace);

    /* History — open before anything so startup events are logged */
    char hist_path[512];
    snprintf(hist_path, sizeof(hist_path), "%s/memory/HISTORY.md", workspace);
    /* Ensure memory dir exists for history */
    char mem_dir[512];
    snprintf(mem_dir, sizeof(mem_dir), "%s/memory", workspace);
    {   /* Create memory dir if missing */
        struct { char p[512]; } tmp; snprintf(tmp.p, sizeof(tmp.p), "%s", mem_dir);
        /* Use mkdir directly — memory module also calls this, but history needs it first */
        mkdir(tmp.p, 0755);
    }
    g_history = history_create(hist_path);
    history_log(g_history, HISTORY_SYSTEM, "Agent Starting",
                      "version: " VERSION_STRING);

    /* Core subsystems */
    http_init();
    tool_registry_init();

    /* Allowlist */
    g_allowlist = allowlist_create();
    allowlist_load(g_allowlist,
        env_or_cfg("ALLOWLIST", cfg, "security", "allowlist", "allowlist.conf"));

    /* Provider */
    const char *provider_type = env_or_cfg("PROVIDER", cfg, "provider", "type", "ollama");
    const char *model   = env_or_cfg("MODEL",   cfg, "provider", "model",   NULL);
    /* api_url: explicit override for remote/self-hosted providers.
     * Read from API_URL env var or [provider] api_url in aham.conf.
     * Kept separate from the Ollama URL so that setting url=http://localhost:11434
     * for Ollama does NOT bleed into remote provider calls. */
    const char *api_url = env_or_cfg("API_URL",  cfg, "provider", "api_url", NULL);
    const char *api_key = env_or_cfg("API_KEY",  cfg, "provider", "api_key", NULL);
    /* Ollama URL: its own key (ollama_url) with separate fallback chain.
     * Falls back to the legacy [provider] url key so existing configs still work,
     * then to the compiled-in default. */
    const char *ollama_url_cfg = config_get(cfg, "provider", "ollama_url", NULL);
    if (!ollama_url_cfg || !*ollama_url_cfg)
        ollama_url_cfg = config_get(cfg, "provider", "url", NULL);
    const char *ollama_url_env = getenv("OLLAMA_URL");
    const char *ollama_url = (ollama_url_env && *ollama_url_env) ? ollama_url_env
                           : (ollama_url_cfg && *ollama_url_cfg) ? ollama_url_cfg
                           : "http://localhost:11434";

    if (!model || !*model) {
        fprintf(stderr, "❌  No model set. Add 'model = ...' to aham.conf or set MODEL.\n");
        history_log(g_history, HISTORY_ERROR, "Startup Failed", "No model configured");
        cleanup(cfg); exit(1);
    }

    if (!strcasecmp(provider_type, "ollama")) {
        g_provider = ollama_create(model, ollama_url);
        if (!g_provider) {
            fprintf(stderr,
                "❌  Cannot reach Ollama at %s\n"
                "    Start: ollama serve\n"
                "    Pull:  ollama pull %s\n", ollama_url, model);
            history_log(g_history, HISTORY_ERROR, "Startup Failed",
                              "Cannot connect to Ollama");
            cleanup(cfg); exit(1);
        }
    } else {
        /* Any OpenAI-compatible provider — registry handles URLs and headers */
        g_provider = openai_create(model, api_url, api_key, provider_type);
        if (!g_provider) {
            /* Try auto-detect from key/url if explicit name failed */
            const provider_spec_t *spec = registry_detect(api_key, api_url);
            if (spec) g_provider = openai_create(model, api_url, api_key, spec->name);
        }
        if (!g_provider) {
            fprintf(stderr,
                "❌  Cannot create provider '%s'.\n"
                "    Check API_KEY / aham.conf [provider] api_key and url.\n",
                provider_type);
            history_log(g_history, HISTORY_ERROR, "Startup Failed",
                              "Cannot create provider");
            cleanup(cfg); exit(1);
        }
    }

    char hist_detail[256];
    snprintf(hist_detail, sizeof(hist_detail),
             "provider: %s\nmodel: %s\nworkspace: %s",
             provider_type, model, workspace);
    history_log(g_history, HISTORY_SYSTEM, "Agent Initialized", hist_detail);

    /* Agent */
    g_agent = agent_create(g_provider);
    g_agent->on_thinking    = on_thinking;
    g_agent->max_iterations = config_get_int(cfg, "agent", "max_iterations", 10);
    g_agent->max_messages   = config_get_int(cfg, "agent", "max_messages",   20);

    /* Bootstrap .md files from workspace root */
    char soul_path[512], agent_path[512], tools_path[512];
    snprintf(soul_path,  sizeof(soul_path),  "%s/SOUL.md",  workspace);
    snprintf(agent_path, sizeof(agent_path), "%s/AGENT.md", workspace);
    snprintf(tools_path, sizeof(tools_path), "%s/TOOLS.md", workspace);

    char *soul = load_file(soul_path);
    if (soul) { agent_add_system_part(g_agent, soul); free(soul); }
    char *inst = load_file(agent_path);
    if (inst) { agent_add_system_part(g_agent, inst); free(inst); }
    /* TOOLS.md documents non-obvious tool constraints; loaded after AGENT.md */
    char *tools_doc = load_file(tools_path);
    if (tools_doc) { agent_add_system_part(g_agent, tools_doc); free(tools_doc); }

    /* Tools */
    shell_tool_register(g_allowlist);
    file_tool_register(g_allowlist);

    /* Serial — device registry at workspace/platform_config/device.conf */
    {
        char serial_conf[512];
        snprintf(serial_conf, sizeof(serial_conf),
                 "%s/platform_config/device.conf", workspace);
        serial_tool_register(serial_conf);
    }

    /* Memory — lives at workspace/memory/MEMORY.md */
    g_memory = memory_create(mem_dir);
    memory_register_tools(g_memory);
    char *mctx = memory_get_context(g_memory);
    g_memory_slot = g_agent->system_part_count;
    agent_add_system_part(g_agent, mctx);
    free(mctx);

    /* Scheduler */
    g_scheduler = scheduler_create();
    scheduler_register_tools(g_scheduler);

    /* Skills — workspace/skills/<name>/SKILL.md */
    g_skills = skills_create(workspace);
    char *sp = skills_get_prompt(g_skills);
    if (sp && *sp) agent_add_system_part(g_agent, sp);
    free(sp);

    /* Heartbeat */
    g_heartbeat = heartbeat_create(
        config_get_int(cfg, "heartbeat", "interval", 60));

    signal(SIGINT,  sig_handler);   /* Ctrl-C: cancel readline line, stay alive */
    signal(SIGTERM, sig_term);      /* kill: clean shutdown */
    signal(SIGHUP,  sig_term);      /* terminal close: clean shutdown */

    /* Status bar */
    status_init(model, args->daemon_mode);

    return cfg;
}

/* Daemon mode */

static void run_daemon(const cli_args_t *args, config_t *cfg) {
    signal(SIGHUP, SIG_IGN);

    int port = args->port > 0 ? args->port
             : atoi(env_or_cfg("PORT", cfg, "gateway", "port", "8080"));

    const char *html_path = env_or_cfg("CHAT_HTML", cfg,
                                        "gateway", "html_path", "assets/chat.html");

    g_gateway = gateway_create(g_agent, g_scheduler, port, html_path, args->debug_mode);
    result_t r = gateway_start(g_gateway);
    if (r.status != OK) {
        fprintf(stderr, "❌  Gateway failed: %s\n", r.message);
        history_log(g_history, HISTORY_ERROR, "Gateway Failed", r.message);
        result_free(&r);
        cleanup(cfg); exit(1);
    }
    result_free(&r);

    char detail[256];
    snprintf(detail, sizeof(detail), "port: %d\ndebug: %s\nhtml: %s",
             port, args->debug_mode ? "on" : "off", html_path);
    history_log(g_history, HISTORY_SYSTEM, "Daemon Started", detail);

    status_set(STATE_IDLE);
    while (g_running) pause();

    fprintf(stderr, "\nAham daemon shutting down…\n");
    history_log(g_history, HISTORY_SYSTEM, "Agent Stopped", "clean shutdown");
    cleanup(cfg);
}

/* CLI mode */

/* Shorten workspace path: replace $HOME prefix with ~ */
static const char *display_workspace(const char *ws) {
    const char *home = getenv("HOME");
    if (home && *home && strncmp(ws, home, strlen(home)) == 0) {
        static char buf[512];
        snprintf(buf, sizeof(buf), "~%s", ws + strlen(home));
        return buf;
    }
    return ws;
}

static void run_cli(const cli_args_t *args, config_t *cfg) {
    (void)args;  /* daemon_mode/debug_mode used in run_daemon, not here */
    /* Header */
    const char *ws = display_workspace(g_workspace);
    printf("\n"
           "  \033[96m────────────────────────────────────────────────────\033[0m\n"
           "\033[1;35m  Aham (I am) AI ⚡ \033[0m\n"
           "  Model    : \033[36m%s\033[0m\n"
           "  Version  : \033[32m" VERSION_STRING "\033[0m\n"
           "  Workspace: \033[33m%s\033[0m\n"
           "  Tip      : Type '/help' for commands, Ctrl-C to cancel, Ctrl-D to quit\n"
           "  \033[96m────────────────────────────────────────────────────\033[0m\n"
           "\n",
           g_provider->model,
           ws);

    slash_ctx_t sctx = {
        .agent     = g_agent,
        .provider  = g_provider,
        .scheduler = g_scheduler,
        .memory    = g_memory,
        .skills    = g_skills,
    };

    status_set(STATE_READY);

    while (g_running) {

        /* Reset sigint flag before each readline call */
        g_sigint_caught = false;

        char *input = readline("\033[2m>\033[0m ");

        /* Ctrl-C: cancel current input, reprint prompt, do NOT exit */
        if (!input && g_sigint_caught) {
            printf("\n");   /* newline after the ^C echo */
            continue;
        }

        /* Ctrl-D / EOF / pipe closed: clean exit */
        if (!input) break;

        char *t = str_trim(input);
        if (!*t) { free(input); continue; }   /* empty Enter — ignore */
        add_history(t);

        char slash_out[8192] = {0};
        if (slash_handle(t, &sctx, slash_out, sizeof(slash_out))) {
            if (!strcmp(slash_out, "__QUIT__")) { free(input); break; }
            printf("%s", slash_out);
            free(input); continue;
        }

        status_set(STATE_THINKING);
        char *resp = agent_chat(g_agent, t);
        status_set(STATE_READY);

        if (resp) {
            printf("\n\033[1;32m⚡ Aham:\033[0m %s\n\n", resp);
            history_log(g_history, HISTORY_SYSTEM, "Conversation Turn",
                              "status: completed");
            free(resp);
        }

        refresh_memory(g_agent);
        free(input);
    }

    printf("\n\033[2m⚡ Aham signing off.\033[0m\n\n");
    history_log(g_history, HISTORY_SYSTEM, "Agent Stopped", "user quit");
    cleanup(cfg);
}

/* main */

int main(int argc, char *argv[]) {
    cli_args_t args;
    if (!parse_args(argc, argv, &args)) {
        print_usage(argv[0]);
        return 0;
    }

    config_t *cfg = boot(&args);
    if (args.daemon_mode) run_daemon(&args, cfg);
    else                  run_cli(&args, cfg);
    return 0;
}
