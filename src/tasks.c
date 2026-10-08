/* tasks — task store backend, mirroring the
 * projects.c/agents.c pattern: lenient load, strict save, atomic
 * tmp+rename writes, one directory per task. A task carries its
 * project reference, a title and a tree of actions (title,
 * description, state — pending, in progress, completed, completed
 * partially, completed unsuccessfully) nesting without a fixed
 * depth limit. */
#define _POSIX_C_SOURCE 200809L

#include "tasks.h"
#include "projects.h"
#include "theme.h" /* theme_dir(): resolved config directory */
#include "util.h"

#include <cJSON.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h> /* rmdir */

/* ---------- helpers ---------- */

static const char *tasks_dir(char *buf, size_t n)
{
    snprintf(buf, n, "%s/tasks", theme_dir());
    return buf;
}

static int valid_id(const char *s)
{
    if (strlen(s) != TASK_ID_LEN) return 0;
    for (size_t i = 0; i < TASK_ID_LEN; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

/* a project reference: PROJECT_ID_LEN lowercase hex chars */
static int valid_project_id(const char *s)
{
    if (strlen(s) != PROJECT_ID_LEN) return 0;
    for (size_t i = 0; i < PROJECT_ID_LEN; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

static int id_used(const tasks_t *c, size_t upto, const char *id)
{
    for (size_t i = 0; i < upto; i++)
        if (strcmp(c->items[i].id, id) == 0) return 1;
    return 0;
}

static int strptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* newest first, id as the tiebreaker for a stable order */
static int task_cmp(const void *pa, const void *pb)
{
    const task_t *a = pa, *b = pb;
    if (a->created != b->created) return a->created < b->created ? 1 : -1;
    return strcmp(a->id, b->id);
}

/* ---------- actions ---------- */

static const char *const ACTION_STATES[] = {
    "pending", "in_progress", "completed", "partial", "failed",
};
#define STATE_COUNT (sizeof ACTION_STATES / sizeof ACTION_STATES[0])

static const char *const ACTION_TYPE_NAMES[] = {
    "act", "observe", "analyze", "find_root_cause", "validate", "improve",
};
#define TYPE_COUNT (sizeof ACTION_TYPE_NAMES / sizeof ACTION_TYPE_NAMES[0])

static int state_from_name(const char *s)
{
    for (size_t i = 0; i < STATE_COUNT; i++)
        if (strcmp(ACTION_STATES[i], s) == 0) return (int)i;
    return -1;
}

static const char *state_name(int s)
{
    return (s >= 0 && (size_t)s < STATE_COUNT) ? ACTION_STATES[s]
                                               : ACTION_STATES[ACTION_PENDING];
}

static int type_from_name(const char *s)
{
    for (size_t i = 0; i < TYPE_COUNT; i++)
        if (strcmp(ACTION_TYPE_NAMES[i], s) == 0) return (int)i;
    return -1;
}

static const char *type_name(int t)
{
    return (t >= 0 && (size_t)t < TYPE_COUNT)
               ? ACTION_TYPE_NAMES[t]
               : ACTION_TYPE_NAMES[ACTION_ACT];
}

/* ---------- dependencies ---------- */

/* a dependency path: dot-separated decimal indexes into the task's
 * action tree, like "0" or "2.1" — non-empty segments, each an
 * index of sane magnitude, the whole under ACTION_DEP_PATH_MAX */
int action_dep_path_ok(const char *s)
{
    if (!s || !s[0] || strlen(s) >= ACTION_DEP_PATH_MAX) return 0;
    size_t run = 0;
    for (const char *p = s; ; p++) {
        if (*p == '\0' || *p == '.') {
            if (run == 0 || run > 10) return 0; /* empty or absurd */
            if (*p == '\0') return 1;
            run = 0;
        } else if (*p >= '0' && *p <= '9') run++;
        else return 0;
    }
}

/* the action `path` names, walking sibling lists by index — the
 * path "2.1" is actions[2]->children[1]. NULL when any index walks
 * past its list (the same walk the planner's find_parent_list does
 * for parent paths, stopping on the action itself) */
action_t *actions_resolve(action_t *actions, const char *path)
{
    if (!action_dep_path_ok(path)) return NULL;
    action_t *list = actions;
    const char *p = path;
    for (;;) {
        size_t idx = 0;
        while (*p >= '0' && *p <= '9') idx = idx * 10 + (size_t)(*p++ - '0');
        action_t *a = NULL;
        size_t k = 0;
        for (a = list; a; a = a->next_sibling, k++)
            if (k == idx) break;
        if (!a) return NULL;
        if (*p == '\0') return a;
        p++; /* skip the dot; the format check guarantees a digit */
        list = a->first_child;
    }
}

/* does following depends_on edges from `from` reach `target` (one
 * edge or more)? The budget caps the walk: a cycle stored by
 * another writer would recurse forever, so past it the answer is
 * "yes" — a dependency chain that deep is broken either way */
static int deps_reach(action_t *root, action_t *from,
                      const action_t *target, int budget)
{
    if (budget <= 0) return 1;
    for (int k = 0; k < from->dep_count; k++) {
        action_t *d = actions_resolve(root, from->depends_on[k]);
        if (d && (d == target || deps_reach(root, d, target, budget - 1)))
            return 1;
    }
    return 0;
}

/* drop the k-th dependency (kept dense: a memmove of the tails) */
static void drop_dep(action_t *a, int k)
{
    if (k < 0 || k >= a->dep_count) return;
    memmove(a->depends_on[k], a->depends_on[k + 1],
            (size_t)(a->dep_count - k - 1) * ACTION_DEP_PATH_MAX);
    a->dep_count--;
}

/*
 * Cross-check every action's dependencies against the whole tree:
 * each path must name another action of this task, and the links
 * must not form a circle. Strict mode reports the first offender
 * into err_field/err_msg (prefix like "tasks[0].actions") and
 * returns -1; lenient mode repairs in place — a dangling or
 * self reference drops, a circle breaks at this action's first
 * edge into it. `root` is the tree the paths resolve in.
 */
static int deps_walk(action_t *list, action_t *root, const char *prefix,
                     int strict, char *ef, size_t efn,
                     char *em, size_t emn)
{
    int i = 0;
    for (action_t *a = list; a; a = a->next_sibling, i++) {
        char aprefix[1024];
        path_set(aprefix, sizeof aprefix, prefix);
        path_add_index(aprefix, sizeof aprefix, (size_t)i);
        for (int k = 0; k < a->dep_count; k++) {
            action_t *d = actions_resolve(root, a->depends_on[k]);
            if (!d) {
                if (strict) {
                    snprintf(ef, efn, "%s.depends_on[%d]", aprefix, k);
                    snprintf(em, emn,
                             "no action at path \"%s\" in this task",
                             a->depends_on[k]);
                    return -1;
                }
                drop_dep(a, k--); /* lenient: the reference dangles */
                continue;
            }
            if (d == a) {
                if (strict) {
                    snprintf(ef, efn, "%s.depends_on[%d]", aprefix, k);
                    snprintf(em, emn, "an action cannot depend on itself");
                    return -1;
                }
                drop_dep(a, k--); /* lenient: self reference */
                continue;
            }
        }
        if (deps_reach(root, a, a, 256)) {
            if (strict) {
                snprintf(ef, efn, "%s.depends_on", aprefix);
                snprintf(em, emn, "circular dependency");
                return -1;
            }
            /* lenient: cut this action's first edge into the circle */
            for (int k = 0; k < a->dep_count; k++) {
                action_t *d = actions_resolve(root, a->depends_on[k]);
                if (d && deps_reach(root, d, a, 255)) {
                    drop_dep(a, k--);
                    break;
                }
            }
        }
        if (a->first_child) {
            char cprefix[1024];
            path_set(cprefix, sizeof cprefix, aprefix);
            path_add(cprefix, sizeof cprefix, ".children");
            if (deps_walk(a->first_child, root, cprefix, strict,
                          ef, efn, em, emn) != 0)
                return -1; /* strict only: lenient never fails */
        }
    }
    return 0;
}


static void actions_free(action_t *a)
{
    while (a) {
        action_t *next = a->next_sibling;
        actions_free(a->first_child);
        free(a);
        a = next;
    }
}

static int parse_action_list(const cJSON *arr, action_t **head, int depth,
                             int strict, char *ef, size_t efn,
                             char *em, size_t emn, const char *prefix);

/*
 * Parse one action object into *out. Strict mode (PUT) rejects any
 * problem into err_field/err_msg; lenient mode (load) keeps the
 * good fields and defaults. `prefix` is the JSON path of this
 * action ("tasks[0].actions[1]"); `depth` counts nested action
 * lists, capped at ACTION_DEPTH_MAX. On strict failure every
 * sub-action built so far is freed. Returns 0 on success.
 */
static int parse_action(const cJSON *obj, action_t *out, int depth,
                        int strict, char *ef, size_t efn,
                        char *em, size_t emn, const char *prefix)
{
    static const char *const keys[] = {
        "title", "description", "state", "type", "depends_on", "children",
    };
    char field[256], child_prefix[1024];
    const cJSON *j;
    memset(out, 0, sizeof *out);

    j = cJSON_GetObjectItemCaseSensitive(obj, "title");
    if (!cJSON_IsString(j) || !j->valuestring || !j->valuestring[0] ||
        strlen(j->valuestring) >= sizeof out->title ||
        !valid_utf8_text(j->valuestring, strlen(j->valuestring), 0)) {
        path_set(field, sizeof field, prefix);
        path_add(field, sizeof field, ".title");
        snprintf(ef, efn, "%s", field);
        snprintf(em, emn, "required, text, max %d bytes, no control characters",
                 ACTION_TITLE_MAX - 1);
        return -1; /* strict and lenient: an action without a title is dropped */
    }
    snprintf(out->title, sizeof out->title, "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "description");
    if (j && (!cJSON_IsString(j) ||
              strlen(cJSON_IsString(j) ? j->valuestring : "") >=
                  sizeof out->description ||
              !valid_utf8_text(j->valuestring, strlen(j->valuestring), 1))) {
        if (strict) {
            path_set(field, sizeof field, prefix);
            path_add(field, sizeof field, ".description");
            snprintf(ef, efn, "%s", field);
            snprintf(em, emn,
                     "text, max %d bytes, no control characters "
                     "except newlines", ACTION_DESC_MAX - 1);
            return -1;
        }
    } else if (j && j->valuestring) {
        snprintf(out->description, sizeof out->description, "%s",
                 j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "state");
    if (j && (!cJSON_IsString(j) || state_from_name(j->valuestring) < 0)) {
        if (strict) {
            path_set(field, sizeof field, prefix);
            path_add(field, sizeof field, ".state");
            snprintf(ef, efn, "%s", field);
            snprintf(em, emn,
                     "pending, in_progress, completed, partial or failed");
            return -1;
        }
    } else if (j && cJSON_IsString(j)) {
        out->state = state_from_name(j->valuestring);
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "type");
    if (j && (!cJSON_IsString(j) || type_from_name(j->valuestring) < 0)) {
        if (strict) {
            path_set(field, sizeof field, prefix);
            path_add(field, sizeof field, ".type");
            snprintf(ef, efn, "%s", field);
            snprintf(em, emn,
                     "observe, analyze, find_root_cause, act, validate "
                     "or improve");
            return -1;
        }
    } else if (j && cJSON_IsString(j)) {
        out->type = type_from_name(j->valuestring);
    }

    /* the dependency links: tree paths of the actions this one
     * waits for. Format, count and duplicates are checked here;
     * whether a path actually names a sibling elsewhere in the
     * tree (and no circle forms) is checked once the whole task
     * is parsed, in deps_walk */
    j = cJSON_GetObjectItemCaseSensitive(obj, "depends_on");
    if (j) {
        char dfield[1024];
        path_set(dfield, sizeof dfield, prefix);
        path_add(dfield, sizeof dfield, ".depends_on");
        if (!cJSON_IsArray(j)) {
            if (strict) {
                snprintf(ef, efn, "%s", dfield);
                snprintf(em, emn, "must be an array of action paths");
                return -1;
            }
        } else {
            int di = 0;
            const cJSON *d = NULL;
            cJSON_ArrayForEach(d, j) {
                if (!cJSON_IsString(d) || !d->valuestring ||
                    !action_dep_path_ok(d->valuestring)) {
                    if (strict) {
                        snprintf(ef, efn, "%s[%d]", dfield, di);
                        snprintf(em, emn, "dot-separated action indexes, "
                                         "like \"0\" or \"2.1\", max %d "
                                         "bytes", ACTION_DEP_PATH_MAX - 1);
                        return -1;
                    }
                    di++;
                    continue; /* lenient: skip the broken link */
                }
                if (out->dep_count >= ACTION_DEPS_MAX) {
                    if (strict) {
                        snprintf(ef, efn, "%s[%d]", dfield, di);
                        snprintf(em, emn, "too many dependencies (max %d)",
                                 ACTION_DEPS_MAX);
                        return -1;
                    }
                    break; /* lenient: keep the first ACTION_DEPS_MAX */
                }
                int dup = 0;
                for (int k = 0; k < out->dep_count; k++)
                    if (strcmp(out->depends_on[k], d->valuestring) == 0)
                        dup = 1;
                if (dup) {
                    if (strict) {
                        snprintf(ef, efn, "%s[%d]", dfield, di);
                        snprintf(em, emn,
                                 "the same dependency is listed twice");
                        return -1;
                    }
                    di++;
                    continue; /* lenient: one copy is enough */
                }
                snprintf(out->depends_on[out->dep_count],
                         ACTION_DEP_PATH_MAX, "%s", d->valuestring);
                out->dep_count++;
                di++;
            }
        }
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "children");
    if (j) {
        path_set(child_prefix, sizeof child_prefix, prefix);
        path_add(child_prefix, sizeof child_prefix, ".children");
        if (!cJSON_IsArray(j)) {
            if (strict) {
                snprintf(ef, efn, "%s", child_prefix);
                snprintf(em, emn, "must be an array of actions");
                return -1;
            }
        } else if (depth + 1 >= ACTION_DEPTH_MAX) {
            if (strict) {
                snprintf(ef, efn, "%s", child_prefix);
                snprintf(em, emn, "too deeply nested (max %d levels)",
                         ACTION_DEPTH_MAX);
                return -1;
            }
        } else if (parse_action_list(j, &out->first_child, depth + 1, strict,
                                     ef, efn, em, emn,
                                     child_prefix) != 0 && strict) {
            return -1; /* out->first_child freed by parse_action_list */
        }
    }

    if (strict) { /* unknown keys are typos -> reject (like theme.c) */
        cJSON_ArrayForEach(j, obj) {
            int known = 0;
            for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (j->string && strcmp(j->string, keys[k]) == 0) known = 1;
            if (!known) {
                path_set(field, sizeof field, prefix);
                path_add(field, sizeof field, ".");
                path_add(field, sizeof field, j->string ? j->string : "");
                snprintf(ef, efn, "%s", field);
                snprintf(em, emn, "unknown setting");
                actions_free(out->first_child);
                out->first_child = NULL;
                return -1;
            }
        }
    }
    return 0;
}

/*
 * Parse an array of actions into a sibling list at *head. Strict
 * mode rejects the whole list on the first problem (freeing what
 * was built); lenient mode keeps the parseable actions. Returns 0
 * on success.
 */
static int parse_action_list(const cJSON *arr, action_t **head, int depth,
                             int strict, char *ef, size_t efn,
                             char *em, size_t emn, const char *prefix)
{
    *head = NULL;
    action_t **tail = head;
    size_t i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, arr) {
        if (!cJSON_IsObject(child)) {
            if (ef && efn) {
                path_set(ef, efn, prefix);
                path_add_index(ef, efn, i);
            }
            snprintf(em, emn, "must be an object");
            if (strict) { actions_free(*head); return -1; }
            i++;
            continue;
        }
        action_t *one = calloc(1, sizeof *one);
        if (!one) {
            snprintf(em, emn, "out of memory");
            if (strict) { actions_free(*head); return -1; }
            i++;
            continue;
        }
        char item_prefix[1024];
        path_set(item_prefix, sizeof item_prefix, prefix);
        path_add_index(item_prefix, sizeof item_prefix, i);
        if (parse_action(child, one, depth, strict, ef, efn, em, emn,
                         item_prefix) != 0) {
            free(one);
            if (strict) { actions_free(*head); return -1; }
            i++;
            continue; /* lenient: skip the broken action */
        }
        *tail = one;
        tail = &one->next_sibling;
        i++;
    }
    return 0;
}

/* the action list as a json array (NULL for an empty list); each
 * action emits its title plus only the non-default fields */
static cJSON *actions_to_cjson(const action_t *a)
{
    if (!a) return NULL;
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;
    for (const action_t *p = a; p; p = p->next_sibling) {
        cJSON *o = cJSON_CreateObject();
        if (!o || !cJSON_AddStringToObject(o, "title", p->title)) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        if (p->description[0] &&
            !cJSON_AddStringToObject(o, "description", p->description)) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        if (p->state != ACTION_PENDING &&
            !cJSON_AddStringToObject(o, "state", state_name(p->state))) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        if (p->type != ACTION_ACT &&
            !cJSON_AddStringToObject(o, "type", type_name(p->type))) {
            cJSON_Delete(o);
            cJSON_Delete(arr);
            return NULL;
        }
        if (p->dep_count > 0) {
            const char *deps[ACTION_DEPS_MAX];
            for (int k = 0; k < p->dep_count; k++)
                deps[k] = p->depends_on[k];
            cJSON *da = cJSON_CreateStringArray(deps, p->dep_count);
            if (!da) {
                cJSON_Delete(o);
                cJSON_Delete(arr);
                return NULL;
            }
            cJSON_AddItemToObject(o, "depends_on", da);
        }
        if (p->first_child) {
            cJSON *kids = actions_to_cjson(p->first_child);
            if (!kids) {
                cJSON_Delete(o);
                cJSON_Delete(arr);
                return NULL;
            }
            cJSON_AddItemToObject(o, "children", kids);
        }
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

void tasks_clear(tasks_t *c)
{
    for (size_t i = 0; i < c->count; i++) {
        actions_free(c->items[i].actions);
        context_free(c->items[i].context);
    }
    c->count = 0;
}

/* ---------- entry parsing (shared by load & PUT) ---------- */

/*
 * Parse one task object into *out. Strict mode (PUT) rejects
 * any problem into err_field/err_msg; lenient mode (load) repairs
 * what it can and skips nothing short of an unusable project
 * reference. `seen` is the list being built (ids are generated to
 * avoid the ids already in it). Returns 0 on success.
 */
static int parse_task(const cJSON *obj, size_t index, task_t *out,
                      const tasks_t *seen,
                      int strict,
                      char *err_field, size_t err_field_n,
                      char *err_msg, size_t err_msg_n)
{
    memset(out, 0, sizeof *out);
    char prefix[48];
    const cJSON *j;
    snprintf(prefix, sizeof prefix, "tasks[%zu]", index);

    /* the id: optional on the wire (new tasks), never
     * rewritten once assigned */
    j = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (cJSON_IsString(j) && j->valuestring && valid_id(j->valuestring)) {
        snprintf(out->id, sizeof out->id, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.id", prefix);
        snprintf(err_msg, err_msg_n,
                 "must be %d lowercase hex characters", TASK_ID_LEN);
        return -1;
    } else {
        do gen_hex_id(out->id, TASK_ID_LEN);
        while (seen && id_used(seen, seen->count, out->id));
    }

    /* the owning project: required, a well-formed project id. Its
     * existence is deliberately not checked — the whole list is
     * PUT after every edit, so rejecting a task whose project was
     * deleted would block all further saves; instead the task is
     * kept and GET flags the dangling reference live (project_ok),
     * like a vanished project directory */
    j = cJSON_GetObjectItemCaseSensitive(obj, "project");
    if (!cJSON_IsString(j) || !j->valuestring ||
        strlen(j->valuestring) >= sizeof out->project ||
        !valid_project_id(j->valuestring)) {
        if (strict) {
            snprintf(err_field, err_field_n, "%s.project", prefix);
            snprintf(err_msg, err_msg_n,
                     "required, the id of the project this task belongs to");
            return -1;
        }
        return -1; /* lenient: unplaceable without a project */
    }
    snprintf(out->project, sizeof out->project, "%s", j->valuestring);

    j = cJSON_GetObjectItemCaseSensitive(obj, "title");
    if (cJSON_IsString(j) && j->valuestring &&
        strlen(j->valuestring) < sizeof out->title &&
        valid_utf8_text(j->valuestring, strlen(j->valuestring), 0)) {
        snprintf(out->title, sizeof out->title, "%s", j->valuestring);
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.title", prefix);
        snprintf(err_msg, err_msg_n, "text, max %d bytes, no control characters",
                 TASK_TITLE_MAX - 1);
        return -1;
    }

    j = cJSON_GetObjectItemCaseSensitive(obj, "created");
    if (cJSON_IsNumber(j) && j->valuedouble >= 0 &&
        j->valuedouble == (double)(long long)j->valuedouble) {
        out->created = (long long)j->valuedouble;
    } else if (j && strict) {
        snprintf(err_field, err_field_n, "%s.created", prefix);
        snprintf(err_msg, err_msg_n, "must be a unix timestamp in seconds");
        return -1;
    } else {
        out->created = (long long)time(NULL); /* missing, or lenient repair */
    }

    if (strict) { /* unknown keys are typos -> reject (like theme.c) */
        static const char *const keys[] = {
            "id", "project", "title", "created", "actions", "context",
        };
        cJSON_ArrayForEach(j, obj) {
            int known = 0;
            for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++)
                if (j->string && strcmp(j->string, keys[k]) == 0) known = 1;
            if (!known) {
                snprintf(err_field, err_field_n, "%s.%s", prefix,
                         j->string ? j->string : "");
                snprintf(err_msg, err_msg_n, "unknown setting");
                return -1;
            }
        }
    }

    /* the action tree: last, so earlier failures never leak it */
    j = cJSON_GetObjectItemCaseSensitive(obj, "actions");
    if (j) {
        char aprefix[64];
        snprintf(aprefix, sizeof aprefix, "%s.actions", prefix);
        if (!cJSON_IsArray(j)) {
            snprintf(err_field, err_field_n, "%s", aprefix);
            snprintf(err_msg, err_msg_n, "must be an array of actions");
            if (strict) return -1;
        } else if (parse_action_list(j, &out->actions, 0, strict,
                                     err_field, err_field_n,
                                     err_msg, err_msg_n,
                                     aprefix) != 0 && strict) {
            return -1; /* the built list is freed by parse_action_list */
        }
    }

    /* dependency links cross-reference the tree built above, so
     * they are checked once it is whole: strict rejects the first
     * bad link (freeing the tree here, like the context failure
     * below), lenient repairs in place */
    if (out->actions) {
        char vprefix[64];
        snprintf(vprefix, sizeof vprefix, "%s.actions", prefix);
        if (deps_walk(out->actions, out->actions, vprefix, strict,
                      err_field, err_field_n, err_msg, err_msg_n) != 0 &&
            strict) {
            actions_free(out->actions);
            out->actions = NULL;
            return -1;
        }
    }

    /* the context items: after the tree; on strict failure the tree
     * built above is freed here, context_from_json frees its own */
    j = cJSON_GetObjectItemCaseSensitive(obj, "context");
    if (j) {
        char cprefix[64];
        snprintf(cprefix, sizeof cprefix, "%s.context", prefix);
        if (context_from_json(j, &out->context, strict,
                              err_field, err_field_n,
                              err_msg, err_msg_n, cprefix) != 0 && strict) {
            actions_free(out->actions);
            out->actions = NULL;
            return -1;
        }
    }
    return 0;
}

/* ---------- load / save ---------- */

int tasks_load(tasks_t *c)
{
    memset(c, 0, sizeof *c);
    char dir[4352];
    tasks_dir(dir, sizeof dir);

    DIR *d = opendir(dir);
    if (!d) return 1; /* no tasks yet */

    /* collect candidate directory names, sorted for a stable order */
    enum { MAX_ENTRIES = 1024 };
    char *names[MAX_ENTRIES];
    size_t nn = 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!valid_id(e->d_name)) continue; /* only {ID} directories */
        if (nn >= MAX_ENTRIES) break;
        names[nn++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, nn, sizeof names[0], strptr_cmp);

    for (size_t i = 0; i < nn && c->count < TASKS_MAX; i++) {
        char name[TASK_ID_LEN + 1];
        snprintf(name, sizeof name, "%s", names[i]);
        free(names[i]);
        char path[4352 + 64];
        snprintf(path, sizeof path, "%s/%s/task.json", dir, name);
        char *buf = read_whole_file(path, 1024 * 1024);
        if (!buf) continue; /* directory without details: not listed */
        cJSON *j = cJSON_Parse(buf);
        free(buf);
        if (!j || !cJSON_IsObject(j)) { cJSON_Delete(j); continue; }
        task_t one;
        /* lenient: no projects context here — dangling references
         * are kept and flagged by GET, like a vanished project dir */
        if (parse_task(j, c->count, &one, c, 0,
                       NULL, 0, NULL, 0) != 0) {
            cJSON_Delete(j);
            continue;
        }
        /* the directory is the identity, whatever the file claims */
        snprintf(one.id, sizeof one.id, "%s", name);
        cJSON_Delete(j);
        if (id_used(c, c->count, one.id)) {
            actions_free(one.actions);
            context_free(one.context);
            continue;
        }
        c->items[c->count++] = one;
    }
    qsort(c->items, c->count, sizeof c->items[0], task_cmp);
    return 0;
}

/* delete a directory tree (a task's); only ever called on
 * paths inside tasks/ whose name is a valid id */
static void rm_rf(const char *path)
{
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[4352 + 128];
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        struct stat st;
        if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) rm_rf(child);
        else remove(child);
    }
    closedir(d);
    rmdir(path);
}

int tasks_save(const tasks_t *c)
{
    char dir[4352];
    tasks_dir(dir, sizeof dir);
    mkdir(dir, 0700); /* first task creates the store */

    for (size_t i = 0; i < c->count; i++) {
        const task_t *tk = &c->items[i];
        char cdir[4352 + 64], path[4352 + 96];
        snprintf(cdir, sizeof cdir, "%s/%s", dir, tk->id);
        snprintf(path, sizeof path, "%s/task.json", cdir);
        mkdir(cdir, 0700); /* exists for already-saved tasks */

        cJSON *o = cJSON_CreateObject();
        cJSON *ok = o;
        if (ok) ok = cJSON_AddStringToObject(o, "id", tk->id);
        if (ok) ok = cJSON_AddStringToObject(o, "project", tk->project);
        if (ok) ok = cJSON_AddStringToObject(o, "title", tk->title);
        if (ok) ok = cJSON_AddNumberToObject(o, "created", (double)tk->created);
        if (ok && tk->actions) {
            cJSON *acts = actions_to_cjson(tk->actions);
            if (acts) cJSON_AddItemToObject(o, "actions", acts);
            else ok = NULL;
        }
        if (ok && tk->context) {
            cJSON *ctx = context_to_cjson(tk->context, 0);
            if (ctx) cJSON_AddItemToObject(o, "context", ctx);
            else ok = NULL;
        }
        int rc = ok ? save_json_atomic(path, o) : -1;
        cJSON_Delete(o);
        if (rc != 0) return -1;
    }

    /* delete the directories of removed tasks */
    DIR *d = opendir(dir);
    if (!d) return 0;
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!valid_id(e->d_name)) continue; /* never touch unknowns */
        char full[4352 + 64];
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (id_used(c, c->count, e->d_name)) continue;
        rm_rf(full);
    }
    closedir(d);
    return 0;
}

/* ---------- JSON in (strict) / out ---------- */

tasks_parse_result_t tasks_from_json(const char *buf, size_t len,
                                     tasks_t *out,
                                     char *err_field,
                                     size_t err_field_n,
                                     char *err_msg, size_t err_msg_n)
{
    err_field[0] = '\0';
    err_msg[0] = '\0';
    memset(out, 0, sizeof *out);

    char *copy = malloc(len + 1);
    if (!copy) {
        snprintf(err_msg, err_msg_n, "out of memory");
        return TASKS_E_JSON;
    }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    cJSON *j = cJSON_Parse(copy);
    free(copy);
    if (!j || !cJSON_IsArray(j)) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "expected a JSON array of tasks");
        return TASKS_E_JSON;
    }

    int n = cJSON_GetArraySize(j);
    if (n > TASKS_MAX) {
        cJSON_Delete(j);
        snprintf(err_msg, err_msg_n, "too many tasks (max %d)",
                 TASKS_MAX);
        return TASKS_E_FIELD;
    }

    size_t i = 0;
    const cJSON *child = NULL;
    cJSON_ArrayForEach(child, j) {
        if (!cJSON_IsObject(child)) {
            snprintf(err_field, err_field_n, "tasks[%zu]", i);
            snprintf(err_msg, err_msg_n, "must be an object");
            cJSON_Delete(j);
            return TASKS_E_FIELD;
        }
        task_t one;
        if (parse_task(child, i, &one, out, 1,
                       err_field, err_field_n, err_msg, err_msg_n) != 0) {
            tasks_clear(out);
            cJSON_Delete(j);
            return TASKS_E_FIELD;
        }
        if (id_used(out, out->count, one.id)) {
            snprintf(err_field, err_field_n, "tasks[%zu].id", i);
            snprintf(err_msg, err_msg_n, "duplicate id");
            actions_free(one.actions);
            context_free(one.context);
            tasks_clear(out);
            cJSON_Delete(j);
            return TASKS_E_FIELD;
        }
        out->items[out->count++] = one;
        i++;
    }

    cJSON_Delete(j);
    qsort(out->items, out->count, sizeof out->items[0], task_cmp);
    return TASKS_OK;
}

/* ---------- live-store helpers ---------- */

int tasks_find_id(const tasks_t *c, const char *id)
{
    if (!id) return -1;
    for (size_t i = 0; i < c->count; i++)
        if (strcmp(c->items[i].id, id) == 0) return (int)i;
    return -1;
}

void task_clear_actions(task_t *t)
{
    if (!t) return;
    actions_free(t->actions);
    t->actions = NULL;
}

static size_t count_actions(const action_t *a)
{
    size_t n = 0;
    for (; a; a = a->next_sibling) n += 1 + count_actions(a->first_child);
    return n;
}

size_t task_count_actions(const task_t *t)
{
    return t ? count_actions(t->actions) : 0;
}

int action_type_from_name(const char *name)
{
    return name ? type_from_name(name) : -1;
}

const char *action_type_name(int type)
{
    return type_name(type);
}

char *tasks_to_json(const tasks_t *c, int with_flags,
                    const projects_t *projects)
{
    cJSON *j = cJSON_CreateArray();
    if (!j) return NULL;
    for (size_t i = 0; i < c->count; i++) {
        const task_t *tk = &c->items[i];
        cJSON *o = cJSON_CreateObject();
        int ok = o &&
            cJSON_AddStringToObject(o, "id", tk->id) &&
            cJSON_AddStringToObject(o, "project", tk->project) &&
            cJSON_AddStringToObject(o, "title", tk->title) &&
            cJSON_AddNumberToObject(o, "created", (double)tk->created);
        if (ok && tk->actions) {
            cJSON *acts = actions_to_cjson(tk->actions);
            if (acts) cJSON_AddItemToObject(o, "actions", acts);
            else ok = 0;
        }
        if (ok && tk->context) {
            cJSON *ctx = context_to_cjson(tk->context, 'T');
            if (ctx) cJSON_AddItemToObject(o, "context", ctx);
            else ok = 0;
        }
        if (ok && with_flags)
            ok = cJSON_AddBoolToObject(o, "project_ok",
                                       projects &&
                                       projects_find_id(projects,
                                                        tk->project) >= 0) != NULL;
        if (!ok) {
            cJSON_Delete(o);
            cJSON_Delete(j);
            return NULL;
        }
        cJSON_AddItemToArray(j, o);
    }
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return s;
}
