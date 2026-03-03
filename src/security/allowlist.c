/*
 * allowlist.c — Security allowlist implementation
 */
#include "allowlist.h"
#include "../core/log.h"

#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_RULES 256

typedef struct {
    char *pattern;
} acl_rule_t;

typedef struct {
    acl_rule_t rules[MAX_RULES];
    int        count;
    bool       enabled;
} acl_list_t;

struct allowlist {
    acl_list_t lists[3]; /* COMMAND, PATH, ENDPOINT */
};

static const char *type_names[] = { "command", "path", "endpoint" };

allowlist_t *allowlist_create(void) {
    allowlist_t *al = calloc(1, sizeof(allowlist_t));
    /* Disabled by default — allows everything */
    for (int i = 0; i < 3; i++) {
        al->lists[i].enabled = false;
    }
    LOG_DEBUG("Allowlist system created");
    return al;
}

result_t allowlist_add(allowlist_t *al, acl_type_t type,
                                    const char *pattern) {
    acl_list_t *list = &al->lists[type];
    if (list->count >= MAX_RULES) {
        return err(ERR_GENERIC, "Max rules reached for %s",
                         type_names[type]);
    }
    list->rules[list->count].pattern = strdup(pattern);
    list->count++;
    list->enabled = true;
    LOG_DEBUG("Allowlist: added %s rule: %s", type_names[type], pattern);
    return ok();
}

result_t allowlist_load(allowlist_t *al, const char *config_path) {
    FILE *f = fopen(config_path, "r");
    if (!f) {
        LOG_DEBUG("No allowlist config at %s (allowlists disabled)", config_path);
        return ok();
    }

    char line[1024];
    acl_type_t current_type = ACL_COMMAND;

    while (fgets(line, sizeof(line), f)) {
        char *s = str_trim(line);
        if (*s == '\0' || *s == '#') continue;

        if (strcmp(s, "[commands]") == 0) { current_type = ACL_COMMAND; continue; }
        if (strcmp(s, "[paths]") == 0)    { current_type = ACL_PATH;    continue; }
        if (strcmp(s, "[endpoints]") == 0){ current_type = ACL_ENDPOINT; continue; }

        /* Parse "allow = val1, val2, val3" */
        if (strncmp(s, "allow", 5) == 0) {
            char *eq = strchr(s, '=');
            if (!eq) continue;
            char *vals = str_trim(eq + 1);

            /* Split by comma */
            char *token = strtok(vals, ",");
            while (token) {
                char *trimmed = str_trim(token);
                if (*trimmed) {
                    allowlist_add(al, current_type, trimmed);
                }
                token = strtok(NULL, ",");
            }
        }
    }

    fclose(f);
    LOG_DEBUG("Loaded allowlist config from %s", config_path);
    return ok();
}

bool allowlist_check(allowlist_t *al, acl_type_t type,
                           const char *value) {
    acl_list_t *list = &al->lists[type];

    /* If not enabled, allow everything */
    if (!list->enabled) return true;

    for (int i = 0; i < list->count; i++) {
        const char *pattern = list->rules[i].pattern;

        switch (type) {
        case ACL_COMMAND: {
            /* Check if command starts with the allowed pattern */
            if (fnmatch(pattern, value, 0) == 0) return true;
            /* Also check just the command name (before first space) */
            const char *space = strchr(value, ' ');
            if (space) {
                char *cmd = strndup(value, (size_t)(space - value));
                bool match = (fnmatch(pattern, cmd, 0) == 0);
                free(cmd);
                if (match) return true;
            }
            break;
        }
        case ACL_PATH:
            /* Prefix match for paths */
            if (str_starts_with(value, pattern)) return true;
            break;

        case ACL_ENDPOINT:
            /* Exact or prefix match for endpoints */
            if (str_starts_with(value, pattern)) return true;
            break;
        }
    }

    LOG_WARN("BLOCKED %s: %s", type_names[type], value);
    return false;
}

bool allowlist_is_enabled(allowlist_t *al, acl_type_t type) {
    return al->lists[type].enabled;
}

void allowlist_set_enabled(allowlist_t *al, acl_type_t type,
                                 bool enabled) {
    al->lists[type].enabled = enabled;
}

void allowlist_destroy(allowlist_t *al) {
    if (!al) return;
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < al->lists[t].count; i++) {
            free(al->lists[t].rules[i].pattern);
        }
    }
    free(al);
}
