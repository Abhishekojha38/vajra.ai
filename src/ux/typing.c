/*
 * typing.c — Animated spinner for the Aham CLI
 *
 * Format while thinking:
 *   ⠋ Thinking (iteration 1)...
 *
 * The background thread writes only while readline is NOT active
 * (between Enter → response). See typing.h for the full explanation.
 */
#include "typing.h"
#include "../core/types.h"
#include "../core/aham.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *spinner_frames[] = {
    "⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"
};
#define SPINNER_COUNT 10

static const char *STATE_LABEL[] = {
    "Ready","Thinking","Executing Tool","Calling API","Idle"
};

/* Shared state */

static pthread_t           g_typing_thread;
static volatile bool       g_typing_active = false;
static char               *g_typing_message = NULL;
static pthread_mutex_t     g_typing_mutex   = PTHREAD_MUTEX_INITIALIZER;
static agent_state_t g_state          = STATE_READY;
static bool                g_daemon_mode    = false;
static agent_state_t g_last_daemon    = (agent_state_t)-1;

/* Spinner thread */

static void *typing_thread_fn(void *arg) {
    (void)arg;
    int frame = 0;

    while (g_typing_active) {
        pthread_mutex_lock(&g_typing_mutex);
        /* Use the message directly (e.g. "Thinking (iteration 1)...") */
        const char *msg = g_typing_message ? g_typing_message
                                           : "Thinking...";
        pthread_mutex_unlock(&g_typing_mutex);

        fprintf(stderr, "\r\033[K  %s %s",
                spinner_frames[frame], msg);
        fflush(stderr);

        frame = (frame + 1) % SPINNER_COUNT;
        usleep(80000); /* 80 ms */
    }

    fprintf(stderr, "\r\033[K");
    fflush(stderr);
    return NULL;
}

/* Public API */

void status_init(const char *model, bool daemon_mode) {
    (void)model;
    g_daemon_mode   = daemon_mode;
    g_typing_active = false;
    g_state         = STATE_READY;
}

void status_set(agent_state_t state) {
    status_set_detail(state, NULL);
}

void status_set_detail(agent_state_t state, const char *detail) {
    if (g_daemon_mode) {
        if (state != g_last_daemon) {
            g_last_daemon = state;
            fprintf(stderr, "  [%s]%s%s\n",
                    STATE_LABEL[state],
                    detail && detail[0] ? " " : "",
                    detail && detail[0] ? detail : "");
            fflush(stderr);
        }
        return;
    }

    pthread_mutex_lock(&g_typing_mutex);
    g_state = state;
    free(g_typing_message);
    g_typing_message = (detail && detail[0]) ? strdup(detail) : NULL;
    pthread_mutex_unlock(&g_typing_mutex);

    bool should_spin = (state == STATE_THINKING ||
                        state == STATE_TOOL      ||
                        state == STATE_API);

    if (should_spin && !g_typing_active) {
        g_typing_active = true;
        pthread_create(&g_typing_thread, NULL, typing_thread_fn, NULL);
    } else if (!should_spin && g_typing_active) {
        g_typing_active = false;
        pthread_join(g_typing_thread, NULL);
    }
}

void status_shutdown(void) {
    if (g_typing_active) {
        g_typing_active = false;
        pthread_join(g_typing_thread, NULL);
    }
    pthread_mutex_lock(&g_typing_mutex);
    free(g_typing_message);
    g_typing_message = NULL;
    pthread_mutex_unlock(&g_typing_mutex);
}
