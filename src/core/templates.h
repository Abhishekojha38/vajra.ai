/*
 * templates.h — Workspace seeder
 *
 * Seeds workspace from bundled templates on first run.
 * Existing files are never overwritten.
 *
 * Layout seeded:
 *   templates/SOUL.md            → <workspace>/SOUL.md
 *   templates/AGENT.md           → <workspace>/AGENT.md
 *   templates/TOOLS.md           → <workspace>/TOOLS.md
 *   templates/memory/MEMORY.md   → <workspace>/memory/MEMORY.md
 *
 * Skills live at <workspace>/skills/ (managed by skills.c, not seeded here).
 * HISTORY.md is created by history.c on first startup.
 */
#ifndef TEMPLATES_H
#define TEMPLATES_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Seed workspace from templates directory.
 * @template_dir:  Path to bundled templates (default: "templates").
 * @workspace_dir: Workspace root (default: ".").
 */
void templates_seed(const char *template_dir,
                           const char *workspace_dir);

#ifdef __cplusplus
}
#endif

#endif /* TEMPLATES_H */
