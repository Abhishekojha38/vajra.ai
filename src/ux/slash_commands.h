/*
 * slash_commands.h — Slash command dispatcher
 *
 * Supported commands:
 *   /help             — List all commands
 *   /tools            — List registered tools with descriptions
 *   /skills           — List loaded skills with descriptions
 *   /skills reload    — Re-scan skills/ directory from disk
 *   /status           — Agent, provider, model, message count
 *   /tasks            — List active background (scheduler) tasks
 *   /tmux             — List active tmux sessions
 *   /new              — Start a fresh conversation
 *   /model            — Show current model (change requires restart)
 *   /usage            — Message statistics
 *   /quit /exit       — Exit Aham
 */
#ifndef SLASH_COMMANDS_H
#define SLASH_COMMANDS_H

#include "../core/aham.h"

#ifdef __cplusplus
extern "C" {
#endif

struct agent;

typedef struct {
    struct agent *agent;
    void *provider;   /* provider_t*  */
    void *scheduler;  /* scheduler_t* */
    void *memory;     /* memory_t*    */
    void *skills;     /* skills_t*    */
} slash_ctx_t;

/*
 * Handle input if it starts with '/'.
 * Returns true if the input was a slash command (handled or unknown).
 * output receives a NUL-terminated human-readable response.
 * Special output "__QUIT__" signals the caller to exit.
 */
bool slash_handle(const char *input, slash_ctx_t *ctx,
                        char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* SLASH_COMMANDS_H */
