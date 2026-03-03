/*
 * gateway.h — HTTP server: REST API + web chat UI
 *
 * Endpoints:
 *   GET  /            — Web chat UI (from chat.html or built-in fallback)
 *   GET  /api/status  — JSON health + version
 *   POST /api/chat    — JSON chat  { "message": "..." } → { "response": "..." }
 *   GET  /api/tasks   — JSON array of active background scheduler tasks
 *   GET  /api/tmux    — JSON array of active tmux sessions
 *
 * Thread model: bounded worker threads (GATEWAY_MAX_THREADS).
 * Agent calls serialised with a mutex (agent is stateful, not thread-safe).
 */
#ifndef GATEWAY_H
#define GATEWAY_H

#include "../core/aham.h"
#include "../agent/agent.h"
#include "../tools/scheduler/scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gateway gateway_t;

/*
 * Create a gateway.
 *
 * @agent:     Agent instance (must outlive gateway). NULL → chat returns 503.
 * @scheduler: Scheduler instance for /api/tasks. NULL → returns empty array.
 * @port:      TCP port. 0 → 8080.
 * @html_path: Path to chat.html. NULL or missing → built-in minimal fallback.
 * @debug:     true → verbose per-request logging to stderr.
 *             Also enabled by DEBUG=1 or DEBUG=true env var at runtime.
 */
gateway_t *gateway_create(agent_t *agent,
                                       scheduler_t *scheduler,
                                       int port,
                                       const char *html_path, bool debug);

/* Start listening (non-blocking, spawns listener thread). */
result_t gateway_start(gateway_t *gw);

/* Graceful shutdown. */
void gateway_destroy(gateway_t *gw);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_H */
