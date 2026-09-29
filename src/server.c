/* server — single-threaded epoll HTTP server with Server-Sent Events.
 *
 * One thread, one epoll loop: static assets are answered from the embedded
 * table, /api/time keeps the connection open and gets a tick every second.
 * Blocking operations are avoided (all sockets non-blocking), so one slow
 * SSE client can never stall the others.
 */
#define _POSIX_C_SOURCE 200809L

#include "server.h"
#include "http.h"
#include "assets_gen.h"
#include "theme.h"
#include "projects.h"
#include "agents.h"
#include "tasks.h"
#include "mcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define READ_CHUNK  16384
#define MAX_REQUEST (64 * 1024)    /* request-head cap before we give up */
#define MAX_BODY    (256 * 1024)   /* request-body cap (agents PUT etc.) */
#define MAX_BACKLOG (256 * 1024)  /* unwritten bytes before dropping a client */
#define MAX_EVENTS  128

typedef struct {
    int  fd;
    char peer[64];               /* remote address, for logging */
    char *in;  size_t in_len, in_cap;
    char *out; size_t out_len, out_cap, out_off;
    bool is_sse;                 /* connection is an SSE stream now */
    bool no_more_read;           /* peer half-closed its side */
    bool close_after_flush;      /* finish writing, then close */
    bool dead;                   /* destroyed at the end of the event batch */
    http_request_t pending;      /* request whose body is still arriving */
    size_t pending_len;
    bool has_pending;
} conn_t;

typedef struct {
    int epfd;
    int listen_fd;
    conn_t **by_fd;              /* fd -> conn_t* (NULL slots) */
    int fd_cap;
    int nconns;
    theme_t theme;               /* current theme (reloaded via API) */
    projects_t projects;         /* current project list (replaced via API) */
    llms_t llms;                 /* named llm endpoints (replaced via API) */
    agents_t agents;             /* current agent list (replaced via API) */
    tasks_t tasks; /* the store behind /api/tasks */
} server_t;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    fputs("[flower] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool buf_reserve(char **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return true;
    size_t nc = *cap ? *cap : 4096;
    while (nc < need) nc *= 2;
    char *nb = realloc(*buf, nc);
    if (!nb) return false;
    *buf = nb;
    *cap = nc;
    return true;
}

/* ---------- connection plumbing ---------- */

static void conn_update_events(server_t *s, conn_t *c)
{
    struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.fd = c->fd };
    if (c->out_off < c->out_len) ev.events |= EPOLLOUT;
    if (epoll_ctl(s->epfd, EPOLL_CTL_MOD, c->fd, &ev) != 0)
        c->dead = true;
}

static void conn_flush(conn_t *c)
{
    while (c->out_off < c->out_len) {
        ssize_t n = write(c->fd, c->out + c->out_off, c->out_len - c->out_off);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        c->dead = true; /* EPIPE, ECONNRESET, ... */
        return;
    }
    if (c->out_off >= c->out_len) {
        c->out_off = c->out_len = 0;
        if (c->close_after_flush) c->dead = true;
    } else if (c->out_len - c->out_off > MAX_BACKLOG) {
        c->dead = true; /* too slow, drop it */
    }
}

static void conn_write(server_t *s, conn_t *c, const void *data, size_t len)
{
    if (c->dead) return;
    if (c->out_off > 0) { /* compact */
        memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
        c->out_len -= c->out_off;
        c->out_off = 0;
    }
    if (c->out_len + len > MAX_BACKLOG) { c->dead = true; return; }
    if (!buf_reserve(&c->out, &c->out_cap, c->out_len + len)) { c->dead = true; return; }
    memcpy(c->out + c->out_len, data, len);
    c->out_len += len;
    conn_flush(c); /* opportunistic: usually the kernel buffer takes it all */
    if (!c->dead) conn_update_events(s, c);
}

static void conn_destroy(server_t *s, conn_t *c)
{
    epoll_ctl(s->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    if (c->fd < s->fd_cap && s->by_fd[c->fd] == c) s->by_fd[c->fd] = NULL;
    free(c->in);
    free(c->out);
    free(c);
    s->nconns--;
}

static void conn_init(server_t *s, int fd, const char *peer)
{
    if (fd >= s->fd_cap) {
        int nc = s->fd_cap ? s->fd_cap : 64;
        while (nc <= fd) nc *= 2;
        conn_t **nb = realloc(s->by_fd, (size_t)nc * sizeof *nb);
        if (!nb) { close(fd); return; }
        memset(nb + s->fd_cap, 0, (size_t)(nc - s->fd_cap) * sizeof *nb);
        s->by_fd = nb;
        s->fd_cap = nc;
    }
    conn_t *c = calloc(1, sizeof *c);
    if (!c) { close(fd); return; }
    c->fd = fd;
    snprintf(c->peer, sizeof c->peer, "%s", peer);
    s->by_fd[fd] = c;
    s->nconns++;

    struct epoll_event ev = { .events = EPOLLIN | EPOLLRDHUP, .data.fd = fd };
    if (epoll_ctl(s->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        s->by_fd[fd] = NULL;
        s->nconns--;
        free(c);
        close(fd);
    }
}

/* ---------- responses & SSE ---------- */

static void respond(server_t *s, conn_t *c, int code, const char *ctype,
                    const void *body, size_t body_len, bool keep_alive,
                    bool head_only, const char *extra)
{
    if (!keep_alive) c->close_after_flush = true; /* must be set before writing */
    char *buf;
    size_t n;
    if (http_build_response(&buf, &n, code, ctype, extra, body, body_len,
                            keep_alive, head_only) != 0) {
        c->dead = true;
        return;
    }
    conn_write(s, c, buf, n);
    free(buf);
}

static void sse_send_time(server_t *s, conn_t *c)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    time_t t = ts.tv_sec;
    struct tm tm;
    gmtime_r(&t, &tm);
    char iso[32];
    strftime(iso, sizeof iso, "%Y-%m-%dT%H:%M:%SZ", &tm);
    char ev[192];
    int n;
    /* every 15 s add a comment line: ignored by browsers, but keeps
     * idle-pruning proxies/middleboxes from silently reaping the stream */
    if (ts.tv_sec % 15 == 0)
        n = snprintf(ev, sizeof ev, ": ping\n\ndata: {\"unix\":%lld,\"iso\":\"%s\"}\n\n",
                     (long long)ts.tv_sec, iso);
    else
        n = snprintf(ev, sizeof ev, "data: {\"unix\":%lld,\"iso\":\"%s\"}\n\n",
                     (long long)ts.tv_sec, iso);
    if (n > 0 && (size_t)n < sizeof ev)
        conn_write(s, c, ev, (size_t)n);
}

static void sse_start(server_t *s, conn_t *c)
{
    char *head;
    size_t head_len;
    if (http_build_sse_head(&head, &head_len) != 0) {
        c->dead = true;
        return;
    }
    conn_write(s, c, head, head_len);
    free(head);
    if (c->dead) return;
    static const char retry[] = "retry: 2000\n";
    conn_write(s, c, retry, sizeof retry - 1);
    c->is_sse = true;
    if (!c->dead) sse_send_time(s, c); /* first event right away */
}

/* ---------- request handling ---------- */

static void respond_json(server_t *s, conn_t *c, int code, const char *json,
                         bool keep_alive)
{
    respond(s, c, code, "application/json", json, strlen(json),
            keep_alive, false, NULL);
}

static void handle_theme_put(server_t *s, conn_t *c, http_request_t *req,
                             const char *body, size_t body_len, int *status)
{
    theme_t nt;
    char efield[80], emsg[160];
    theme_parse_result_t pr = theme_from_json(body, body_len, &nt,
                                              efield, sizeof efield,
                                              emsg, sizeof emsg);
    if (pr != THEME_OK) {
        *status = (pr == THEME_E_JSON) ? 400 : 422;
        char *eb = theme_error_json(emsg, efield);
        if (eb) {
            respond_json(s, c, *status, eb, req->keep_alive);
            free(eb);
        } else {
            respond_json(s, c, *status, "{\"error\":\"invalid theme\"}",
                         req->keep_alive);
        }
        return;
    }
    if (theme_save(&nt) != 0) {
        *status = 500;
        respond_json(s, c, 500,
                     "{\"error\":\"cannot write theme.json in config dir\"}",
                     req->keep_alive);
        return;
    }
    s->theme = nt;
    *status = 200;
    char *j = theme_to_json(&nt);
    if (j) {
        respond_json(s, c, 200, j, req->keep_alive);
        free(j);
    } else {
        respond_json(s, c, 200, "{}", req->keep_alive);
    }
}

static void handle_projects_put(server_t *s, conn_t *c, http_request_t *req,
                                const char *body, size_t body_len, int *status)
{
    projects_t np;
    char efield[80], emsg[160];
    projects_parse_result_t pr = projects_from_json(body, body_len, &np,
                                                    efield, sizeof efield,
                                                    emsg, sizeof emsg);
    if (pr != PROJECTS_OK) {
        *status = (pr == PROJECTS_E_JSON) ? 400 : 422;
        char *eb = theme_error_json(emsg, efield);
        if (eb) {
            respond_json(s, c, *status, eb, req->keep_alive);
            free(eb);
        } else {
            respond_json(s, c, *status, "{\"error\":\"invalid project list\"}",
                         req->keep_alive);
        }
        return;
    }
    if (projects_save(&np) != 0) {
        projects_clear(&np);
        *status = 500;
        respond_json(s, c, 500,
                     "{\"error\":\"cannot write projects.json in config dir\"}",
                     req->keep_alive);
        return;
    }
    projects_clear(&s->projects); /* the replaced contexts are heap now */
    s->projects = np;
    *status = 200;
    char *j = projects_to_json(&np, 0);
    if (j) {
        respond_json(s, c, 200, j, req->keep_alive);
        free(j);
    } else {
        respond_json(s, c, 200, "[]", req->keep_alive);
    }
}

static void handle_llms_put(server_t *s, conn_t *c, http_request_t *req,
                            const char *body, size_t body_len, int *status)
{
    llms_t nl;
    char efield[96], emsg[160];
    llms_parse_result_t pr = llms_from_json(body, body_len, &nl,
                                             efield, sizeof efield,
                                             emsg, sizeof emsg);
    if (pr != LLMS_OK) {
        *status = (pr == LLMS_E_JSON) ? 400 : 422;
        char *eb = theme_error_json(emsg, efield);
        if (eb) {
            respond_json(s, c, *status, eb, req->keep_alive);
            free(eb);
        } else {
            respond_json(s, c, *status, "{\"error\":\"invalid llm list\"}",
                         req->keep_alive);
        }
        return;
    }
    if (llms_save(&nl) != 0) {
        llms_free(&nl);
        *status = 500;
        respond_json(s, c, 500,
                     "{\"error\":\"cannot write llms.json in config dir\"}",
                     req->keep_alive);
        return;
    }
    llms_free(&s->llms);
    s->llms = nl;
    *status = 200;
    char *j = llms_to_json(&nl);
    if (j) {
        respond_json(s, c, 200, j, req->keep_alive);
        free(j);
    } else {
        respond_json(s, c, 200, "[]", req->keep_alive);
    }
}

static void handle_agents_put(server_t *s, conn_t *c, http_request_t *req,
                              const char *body, size_t body_len, int *status)
{
    agents_t na;
    char efield[96], emsg[160];
    agents_parse_result_t pr = agents_from_json(body, body_len, &s->llms,
                                                 &na, efield, sizeof efield,
                                                 emsg, sizeof emsg);
    if (pr != AGENTS_OK) {
        *status = (pr == AGENTS_E_JSON) ? 400 : 422;
        char *eb = theme_error_json(emsg, efield);
        if (eb) {
            respond_json(s, c, *status, eb, req->keep_alive);
            free(eb);
        } else {
            respond_json(s, c, *status, "{\"error\":\"invalid agent list\"}",
                         req->keep_alive);
        }
        return;
    }
    if (agents_save(&na) != 0) {
        agents_free(&na);
        *status = 500;
        respond_json(s, c, 500,
                     "{\"error\":\"cannot write agents/ in config dir\"}",
                     req->keep_alive);
        return;
    }
    agents_free(&s->agents);
    s->agents = na;
    *status = 200;
    char *j = agents_to_json(&na, NULL, 0); /* PUT echo: refs just validated */
    if (j) {
        respond_json(s, c, 200, j, req->keep_alive);
        free(j);
    } else {
        respond_json(s, c, 200, "[]", req->keep_alive);
    }
}

static void handle_tasks_put(server_t *s, conn_t *c,
                                     http_request_t *req,
                                     const char *body, size_t body_len,
                                     int *status)
{
    tasks_t nc;
    char efield[96], emsg[160];
    tasks_parse_result_t pr = tasks_from_json(
        body, body_len, &nc,
        efield, sizeof efield, emsg, sizeof emsg);
    if (pr != TASKS_OK) {
        *status = (pr == TASKS_E_JSON) ? 400 : 422;
        char *eb = theme_error_json(emsg, efield);
        if (eb) {
            respond_json(s, c, *status, eb, req->keep_alive);
            free(eb);
        } else {
            respond_json(s, c, *status,
                         "{\"error\":\"invalid task list\"}",
                         req->keep_alive);
        }
        return;
    }
    if (tasks_save(&nc) != 0) {
        tasks_clear(&nc);
        *status = 500;
        respond_json(s, c, 500,
                     "{\"error\":\"cannot write tasks/ in config dir\"}",
                     req->keep_alive);
        return;
    }
    tasks_clear(&s->tasks);
    s->tasks = nc;
    *status = 200;
    char *j = tasks_to_json(&nc, 0, NULL); /* echo: validated */
    if (j) {
        respond_json(s, c, 200, j, req->keep_alive);
        free(j);
    } else {
        respond_json(s, c, 200, "[]", req->keep_alive);
    }
}

static void handle_request(server_t *s, conn_t *c, http_request_t *req,
                           const char *body, size_t body_len)
{
    char *q = strchr(req->target, '?');
    if (q) *q = '\0';
    const char *path = req->target;
    if (strcmp(path, "/") == 0) path = "/index.html";

    bool get  = strcmp(req->method, "GET")  == 0;
    bool head = strcmp(req->method, "HEAD") == 0;
    bool put  = strcmp(req->method, "PUT")  == 0;
    bool post = strcmp(req->method, "POST") == 0;
    int status = 200;

    if (get || head) {
        if (strcmp(path, "/api/time") == 0) {
            if (head) {
                status = 405;
                respond(s, c, 405, "text/plain; charset=utf-8",
                        "method not allowed\n", 19, false, false,
                        "Allow: GET\r\n");
            } else {
                logmsg("%s %s 200 (sse) [%s]", req->method, path, c->peer);
                sse_start(s, c);
                return;
            }
        } else if (strcmp(path, "/api/theme") == 0) {
            char *j = theme_to_json(&s->theme);
            if (j) {
                respond(s, c, 200, "application/json", j, strlen(j),
                        req->keep_alive, head, NULL);
                free(j);
            } else {
                status = 500;
                respond_json(s, c, 500, "{\"error\":\"out of memory\"}",
                             req->keep_alive);
            }
        } else if (strcmp(path, "/api/projects") == 0) {
            char *j = projects_to_json(&s->projects, 1);
            if (j) {
                respond(s, c, 200, "application/json", j, strlen(j),
                        req->keep_alive, head, NULL);
                free(j);
            } else {
                status = 500;
                respond_json(s, c, 500, "{\"error\":\"out of memory\"}",
                             req->keep_alive);
            }
        } else if (strcmp(path, "/api/llms") == 0) {
            char *j = llms_to_json(&s->llms);
            if (j) {
                respond(s, c, 200, "application/json", j, strlen(j),
                        req->keep_alive, head, NULL);
                free(j);
            } else {
                status = 500;
                respond_json(s, c, 500, "{\"error\":\"out of memory\"}",
                             req->keep_alive);
            }
        } else if (strcmp(path, "/api/agents") == 0) {
            char *j = agents_to_json(&s->agents, &s->llms, 1);
            if (j) {
                respond(s, c, 200, "application/json", j, strlen(j),
                        req->keep_alive, head, NULL);
                free(j);
            } else {
                status = 500;
                respond_json(s, c, 500, "{\"error\":\"out of memory\"}",
                             req->keep_alive);
            }
        } else if (strcmp(path, "/api/tasks") == 0) {
            char *j = tasks_to_json(&s->tasks, 1, &s->projects);
            if (j) {
                respond(s, c, 200, "application/json", j, strlen(j),
                        req->keep_alive, head, NULL);
                free(j);
            } else {
                status = 500;
                respond_json(s, c, 500, "{\"error\":\"out of memory\"}",
                             req->keep_alive);
            }
        } else if (strcmp(path, "/mcp") == 0) {
            status = 405;
            respond(s, c, 405, "text/plain; charset=utf-8",
                    "method not allowed\n", 19, false, false,
                    "Allow: POST\r\n");
        } else {
            const asset_t *a = asset_find(path);
            if (!a) {
                status = 404;
                respond(s, c, 404, "text/plain; charset=utf-8",
                        "not found\n", 10, req->keep_alive, false, NULL);
            } else {
                respond(s, c, 200, a->mime, a->data, a->size,
                        req->keep_alive, head, NULL);
            }
        }
    } else if (put && strcmp(path, "/api/theme") == 0) {
        handle_theme_put(s, c, req, body, body_len, &status);
    } else if (post && strcmp(path, "/api/theme/reset") == 0) {
        theme_t def;
        theme_defaults(&def);
        if (theme_save(&def) != 0) {
            status = 500;
            respond_json(s, c, 500, "{\"error\":\"cannot write theme.json\"}",
                         req->keep_alive);
        } else {
            s->theme = def;
            char *j = theme_to_json(&def);
            if (j) {
                respond_json(s, c, 200, j, req->keep_alive);
                free(j);
            } else {
                respond_json(s, c, 200, "{}", req->keep_alive);
            }
        }
    } else if (put && strcmp(path, "/api/projects") == 0) {
        handle_projects_put(s, c, req, body, body_len, &status);
    } else if (put && strcmp(path, "/api/llms") == 0) {
        handle_llms_put(s, c, req, body, body_len, &status);
    } else if (put && strcmp(path, "/api/agents") == 0) {
        handle_agents_put(s, c, req, body, body_len, &status);
    } else if (put && strcmp(path, "/api/tasks") == 0) {
        handle_tasks_put(s, c, req, body, body_len, &status);
    } else if (post && strcmp(path, "/mcp") == 0) {
        /* flower's own mcp server (streamable-http transport, plain
         * json replies). A tools/call fetches a web page while this
         * event loop waits — a known limit, fine for the runner's
         * rare, serial calls. */
        int note = 0;
        char *j = mcp_handle_post(body, body_len, &note);
        if (!j) { /* notification: accepted, nothing to say */
            status = 202;
            respond(s, c, 202, NULL, "", 0, req->keep_alive, false, NULL);
        } else {
            respond_json(s, c, 200, j, req->keep_alive);
            free(j);
        }
    } else if (strcmp(path, "/api/theme") == 0 ||
               strcmp(path, "/api/projects") == 0 ||
               strcmp(path, "/api/llms") == 0 ||
               strcmp(path, "/api/agents") == 0 ||
               strcmp(path, "/api/tasks") == 0) {
        status = 405;
        respond(s, c, 405, "text/plain; charset=utf-8",
                "method not allowed\n", 19, false, false,
                "Allow: GET, HEAD, PUT\r\n");
    } else if (strcmp(path, "/api/theme/reset") == 0) {
        status = 405;
        respond(s, c, 405, "text/plain; charset=utf-8",
                "method not allowed\n", 19, false, false, "Allow: POST\r\n");
    } else if (strcmp(path, "/mcp") == 0) {
        status = 405;
        respond(s, c, 405, "text/plain; charset=utf-8",
                "method not allowed\n", 19, false, false, "Allow: POST\r\n");
    } else {
        status = 405;
        respond(s, c, 405, "text/plain; charset=utf-8",
                "method not allowed\n", 19, false, false,
                "Allow: GET, HEAD\r\n");
    }
    logmsg("%s %s %d [%s]", req->method, path, status, c->peer);
}

static void process_input(server_t *s, conn_t *c)
{
    for (;;) {
        if (c->dead || c->is_sse || c->close_after_flush) return;

        /* body of an already-parsed request still arriving? */
        if (c->has_pending) {
            if (c->in_len < c->pending_len) return;
            http_request_t req = c->pending;
            size_t body_len = c->pending_len;
            c->has_pending = false;
            handle_request(s, c, &req, c->in, body_len);
            if (c->dead || c->is_sse || c->close_after_flush) return;
            memmove(c->in, c->in + body_len, c->in_len - body_len);
            c->in_len -= body_len;
            continue;
        }

        if (c->in_len == 0) return;
        http_request_t req;
        size_t consumed = 0;
        int r = http_parse_request(c->in, c->in_len, &req, &consumed);
        if (r == 0) {
            if (c->in_len > MAX_REQUEST)
                respond(s, c, 431, "text/plain; charset=utf-8",
                        "request too large\n", 18, false, false, NULL);
            return;
        }
        if (r < 0) {
            respond(s, c, 400, "text/plain; charset=utf-8",
                    "bad request\n", 12, false, false, NULL);
            return;
        }
        /* strip the head; the body (if any) now sits at the front */
        memmove(c->in, c->in + consumed, c->in_len - consumed);
        c->in_len -= consumed;

        if (req.chunked) {
            respond(s, c, 400, "text/plain; charset=utf-8",
                    "chunked bodies not supported\n", 29, false, false, NULL);
            return;
        }
        if (req.has_body && req.content_length > MAX_BODY) {
            respond(s, c, 413, "text/plain; charset=utf-8",
                    "body too large\n", 15, false, false, NULL);
            return;
        }
        size_t body_len = (req.has_body && req.content_length > 0)
                              ? req.content_length : 0;
        if (body_len > 0 && c->in_len < body_len) {
            c->pending = req; /* remember it, wait for the rest */
            c->pending_len = body_len;
            c->has_pending = true;
            return;
        }
        handle_request(s, c, &req, c->in, body_len);
        if (c->dead || c->is_sse || c->close_after_flush) return;
        memmove(c->in, c->in + body_len, c->in_len - body_len);
        c->in_len -= body_len;
    }
}

static void on_readable(server_t *s, conn_t *c)
{
    if (c->no_more_read) return;

    if (c->is_sse) { /* only watch for EOF; discard anything the client sends */
        char sink[4096];
        for (;;) {
            ssize_t n = read(c->fd, sink, sizeof sink);
            if (n > 0) continue;
            if (n == 0) { c->dead = true; return; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            c->dead = true;
            return;
        }
    }

    for (;;) {
        if (!buf_reserve(&c->in, &c->in_cap, c->in_len + READ_CHUNK)) {
            c->dead = true;
            return;
        }
        ssize_t n = read(c->fd, c->in + c->in_len, c->in_cap - c->in_len);
        if (n > 0) {
            c->in_len += (size_t)n;
            process_input(s, c);
            if (c->dead || c->is_sse || c->close_after_flush) return;
            continue;
        }
        if (n == 0) { c->dead = true; return; } /* peer closed */
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        c->dead = true;
        return;
    }
}

/* ---------- accept & listener ---------- */

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void on_accept(server_t *s)
{
    for (;;) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int fd = accept(s->listen_fd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            logmsg("accept: %s", strerror(errno));
            return;
        }
        const void *src = &((struct sockaddr_in *)&ss)->sin_addr;
        if (ss.ss_family == AF_INET6)
            src = &((struct sockaddr_in6 *)&ss)->sin6_addr;
        char peer[64];
        if (!inet_ntop(ss.ss_family, src, peer, sizeof peer))
            snprintf(peer, sizeof peer, "unknown");
        set_nonblock(fd);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        conn_init(s, fd, peer);
    }
}

static int listen_socket(const char *bind_addr, uint16_t port,
                         char *url, size_t url_len)
{
    char portstr[8];
    snprintf(portstr, sizeof portstr, "%u", (unsigned)port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int rc = getaddrinfo(bind_addr, portstr, &hints, &res);
    if (rc != 0) {
        logmsg("getaddrinfo(%s): %s", bind_addr ? bind_addr : "*",
             gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (bind(fd, ai->ai_addr, ai->ai_addrlen) == 0 &&
            listen(fd, 128) == 0) {
            set_nonblock(fd);
            if (bind_addr && strchr(bind_addr, ':'))
                snprintf(url, url_len, "http://[%s]:%u/", bind_addr, (unsigned)port);
            else
                snprintf(url, url_len, "http://%s:%u/",
                         bind_addr ? bind_addr : "0.0.0.0", (unsigned)port);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) logmsg("bind %s:%u: %s", bind_addr ? bind_addr : "*",
                     (unsigned)port, strerror(errno));
    return fd;
}

/* ---------- main loop ---------- */

int server_run(const char *bind_addr, uint16_t port)
{
    char url[128];
    int lfd = listen_socket(bind_addr, port, url, sizeof url);
    if (lfd < 0) return 1;

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        logmsg("epoll_create1: %s", strerror(errno));
        close(lfd);
        return 1;
    }

    server_t s;
    memset(&s, 0, sizeof s);
    s.epfd = epfd;
    s.listen_fd = lfd;

    struct epoll_event lev = { .events = EPOLLIN, .data.fd = lfd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &lev);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: epoll_wait must return */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    logmsg("listening on %s", url);

    int tl = theme_load(&s.theme);
    projects_load(&s.projects);
    llms_load(&s.llms);
    agents_load(&s.agents);
    tasks_load(&s.tasks);
    logmsg("config: %s (%s, %d project%s, %d llm%s, %d agent%s, "
           "%d task%s)", theme_dir(),
           tl == 0 ? "theme.json loaded" : "using default theme",
           (int)s.projects.count, s.projects.count == 1 ? "" : "s",
           (int)s.llms.count, s.llms.count == 1 ? "" : "s",
           (int)s.agents.count, s.agents.count == 1 ? "" : "s",
           (int)s.tasks.count, s.tasks.count == 1 ? "" : "s");

    long long next_tick = now_ms() / 1000 * 1000 + 1000;

    while (!g_stop) {
        long long now = now_ms();
        int timeout = now >= next_tick ? 0 : (int)(next_tick - now);
        struct epoll_event evs[MAX_EVENTS];
        int n = epoll_wait(epfd, evs, MAX_EVENTS, timeout);
        if (n < 0 && errno != EINTR) {
            logmsg("epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = evs[i].data.fd;
            uint32_t re = evs[i].events;
            if (fd == lfd) {
                if (re & EPOLLIN) on_accept(&s);
                continue;
            }
            conn_t *c = (fd >= 0 && fd < s.fd_cap) ? s.by_fd[fd] : NULL;
            if (!c || c->dead) continue;
            if (re & (EPOLLERR | EPOLLHUP)) { c->dead = true; continue; }
            if (re & EPOLLRDHUP) c->no_more_read = true;
            if ((re & EPOLLIN) && !c->no_more_read) on_readable(&s, c);
            if (!c->dead && (re & EPOLLOUT)) {
                conn_flush(c);
                if (!c->dead) conn_update_events(&s, c);
            }
            /* peer half-closed and nothing left to write: done */
            if (!c->dead && c->no_more_read && !c->is_sse && c->out_len == 0)
                c->dead = true;
        }

        /* sweep closed conns (deferred so handlers never see freed memory) */
        for (int fd = 0; fd < s.fd_cap; fd++)
            if (s.by_fd[fd] && s.by_fd[fd]->dead)
                conn_destroy(&s, s.by_fd[fd]);

        /* clock tick: push the current time to every SSE client */
        now = now_ms();
        if (now >= next_tick) {
            for (int fd = 0; fd < s.fd_cap; fd++) {
                conn_t *c = s.by_fd[fd];
                if (c && !c->dead && c->is_sse) sse_send_time(&s, c);
            }
            next_tick = now / 1000 * 1000 + 1000;
        }
    }

    logmsg("shutting down (%d connection%s closed)",
         s.nconns, s.nconns == 1 ? "" : "s");
    for (int fd = 0; fd < s.fd_cap; fd++)
        if (s.by_fd[fd]) conn_destroy(&s, s.by_fd[fd]);
    free(s.by_fd);
    llms_free(&s.llms);
    agents_free(&s.agents);
    tasks_clear(&s.tasks);
    close(epfd);
    close(lfd);
    return 0;
}
