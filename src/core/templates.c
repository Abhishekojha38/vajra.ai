/*
 * templates.c — Seed workspace from bundled templates on first run
 *
 * Template layout:
 *   templates/
 *     SOUL.md                  → <workspace>/SOUL.md
 *     AGENT.md                 → <workspace>/AGENT.md
 *     TOOLS.md                 → <workspace>/TOOLS.md
 *     memory/
 *       MEMORY.md              → <workspace>/memory/MEMORY.md
 *
 * Rules:
 *   - Never overwrites an existing file.
 *   - Creates directories as needed.
 *   - Safe to call on every startup.
 *
 * Skills live at <workspace>/skills/ and are managed separately.
 * HISTORY.md is created by history.c, not by the template seeder.
 */
#include "templates.h"
#include "log.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) return;
    if (mkdir(path, 0755) < 0 && errno != EEXIST)
        LOG_WARN("Cannot create directory %s: %s", path, strerror(errno));
}

static void copy_if_missing(const char *src, const char *dst) {
    if (file_exists(dst)) return;
    FILE *in = fopen(src, "r");
    if (!in) return;
    FILE *out = fopen(dst, "w");
    if (!out) {
        LOG_WARN("template: cannot create %s: %s", dst, strerror(errno));
        fclose(in);
        return;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    LOG_WARN("template: seeded %s", dst);
}

void templates_seed(const char *template_dir, const char *workspace_dir) {
    if (!template_dir  || !*template_dir)  template_dir  = "templates";
    if (!workspace_dir || !*workspace_dir) workspace_dir = ".";

    /* Top-level prompt files */
    static const char *top[] = { "SOUL.md", "AGENT.md", "TOOLS.md", NULL };
    for (int i = 0; top[i]; i++) {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s/%s", template_dir,  top[i]);
        snprintf(dst, sizeof(dst), "%s/%s", workspace_dir, top[i]);
        copy_if_missing(src, dst);
    }

    /* Memory directory: seed MEMORY.md template */
    char mem_dir[512];
    snprintf(mem_dir, sizeof(mem_dir), "%s/memory", workspace_dir);
    ensure_dir(mem_dir);

    char src_mem[512], dst_mem[512];
    snprintf(src_mem, sizeof(src_mem), "%s/memory/MEMORY.md", template_dir);
    snprintf(dst_mem, sizeof(dst_mem), "%s/memory/MEMORY.md", workspace_dir);
    copy_if_missing(src_mem, dst_mem);
}
