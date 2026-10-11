#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>

#include "models.h"
#include "server.h"
#include "session.h"
#include "sock.h"

#define BUF_CAP (MSG_MAX + 1)

typedef struct {
    int fd;
    int cancelled;
    int n_tok;
    double t_start;
    double t_first;
    session *sess;
} stream_ctx;

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int client_cancelled(int fd)
{
    uint8_t h[8];
    ssize_t r = recv(fd, h, 8, MSG_DONTWAIT | MSG_PEEK );
    if (r < 8) return 0;
    return h[4] == MSG_CANCEL;
}

static int on_token(const char *text, int len, int kind, void *ud)
{
    stream_ctx *c = ud;
    if (c->sess) session_append(c->sess, text, (size_t)len);

    if (c->n_tok == 0)
    {
        c->t_first = now_sec() - c->t_start;
        char b[64];
        int n = snprintf(b, sizeof(b), "%.3f", c->t_first);
        msg_write(c->fd, MSG_STATUS, b, (uint32_t)n);
    }
    c->n_tok++;

    uint8_t mt = MSG_CHUNK;
    if (kind == TOK_TOOL_CALL) mt = MSG_TOOL_CALL;
    else if (kind == TOK_TOOL_RESULT) mt = MSG_TOOL_RESULT;
    else if (kind == TOK_TOOL_ERROR) mt = MSG_TOOL_ERROR;

    if (msg_write(c->fd, mt, text, (uint32_t)len) < 0) return -1;

    if ((c->n_tok & 7) == 0 && client_cancelled(c->fd))
    {
        c->cancelled = 1;
        return -1;
    }
    return 0;
}

static void send_models(int fd, const model_list *l)
{
    char out[8192];
    int n = 0;
    for (int i = 0; i < l->count; i++)
    {
        n += snprintf(out + n, sizeof(out) - n, "%s\n", l->entries[i].name);
    }
    msg_write(fd, MSG_MODELS, out, (uint32_t)n);
}

static int split_id(char *buf, char **rest)
{
    char *nl = strchr(buf, '\n');
    if (!nl) { *rest = buf; return 0; }
    *nl = '\0';
    *rest = nl + 1;
    return atoi(buf);
}

static void handle_client(int fd, model_list *list, model_slot *slot,
                            int n_ctx, int n_threads, char *buf)
{
    uint8_t type;
    uint32_t len;

    while (msg_read(fd, &type, buf, BUF_CAP, &len) > 0)
    {
        switch (type)
        {
            case MSG_LIST:
                models_scan(list->dir, list);
                send_models(fd, list);
                break;

            case MSG_LOAD:
                msg_writes(fd, MSG_STATUS, "loading");
                if (model_load(slot, list, buf, n_ctx, n_threads) < 0) { 
                    msg_writes(fd, MSG_ERROR, "load failed");
                }
                else{ 
                    msg_writes(fd, MSG_STATUS, "ready");
                }
                break;

            case MSG_RESET:{
                session *s = session_find(atoi(buf));
                if (s) agent_reset(slot, s);
                msg_writes(fd, MSG_STATUS, "reset");
                break;
            }

            case MSG_DROP: {
                session *s = session_find(atoi(buf));
                if (s) session_drop(slot->ctx, s);
                msg_writes(fd, MSG_STATUS, "dropped");
                break;
            }

            case MSG_PROMPT: {
                if (!slot->loaded) { msg_writes(fd, MSG_ERROR, "no model loaded"); break; }

                char *text = NULL;
                int cid = split_id(buf, &text);
                session *s = session_get(slot->ctx, cid);
                if (!s) { msg_writes(fd, MSG_ERROR, "no free session"); break; }

                session_reset_buf(s);
                s->state = SESS_RUNNING;

                stream_ctx c = { .fd = fd, .t_start = now_sec(), .sess = s };
                msg_writes(fd, MSG_STATUS, "generating");

                int rc = agent_generate(slot, s, text, on_token, &c);
                s->state = (rc < 0) ? SESS_ERROR : SESS_DONE;
                
                if (c.cancelled) {
                    msg_writes(fd, MSG_DONE, "cancelled");
                } else if (rc == -2) {
                    msg_writes(fd, MSG_FULL, "context window full");
                } else if (rc < 0) {
                    msg_writes(fd, MSG_ERROR, "generation failed");
                } else {
                    double el = now_sec() - c.t_start;
                    char st[160];
                    int n = snprintf(st, sizeof(st),
                                    "{\"tokens\":%d,\"seconds\":%.2f,\"tps\":%.2f,\"ttft\":%.3f}",
                                    c.n_tok, el, c.n_tok / (el > 0 ? el : 1), c.t_first);
                    msg_write(fd, MSG_STATS, st, (uint32_t)n);
                    msg_writes(fd, MSG_DONE, "ok");
                }
                break;
            }

            case MSG_CANCEL:
                break;

            case MSG_TAIL: {
                char *offs = NULL;
                int cid = split_id(buf, &offs);
                session *s = session_find(cid);
                if (!s) { msg_writes(fd, MSG_ERROR, "no such session"); break; }

                size_t off = (size_t)strtoul(offs, NULL, 10);
                char out[8192];
                int st = 0;
                size_t n = session_tail(s, off, out, sizeof(out), &st);
                msg_write(fd, MSG_CHUNK, out, (uint32_t)n);
                msg_writes(fd, MSG_DONE, st == SESS_RUNNING ? "running" : "idle");
                break;
            }

            default:
                msg_writes(fd, MSG_ERROR, "unknown message type");
                break;
        }
    }
}

int server_run (const char *sock_path, const char *model_dir, int n_ctx, int n_threads)
{
   model_list list;
   model_slot slot;
   memset(&slot, 0, sizeof(slot));

   if (models_scan(model_dir, &list) < 0) return -1;
   sessions_init();
   agent_load_base("/root/projects/agentInfra/src/skills/BASE.md");
   fprintf(stderr, "server: %d models in %s\n", list.count, model_dir);

   int lfd = sock_listen(sock_path, 4);
   if (lfd < 0) return -1;
   fprintf(stderr, "server: listening on %s\n", sock_path);

   char *buf = malloc(BUF_CAP);
   if (!buf) { close(lfd); return -1; }

   for (;;)
   {
        int fd = sock_accept(lfd);
        if (fd < 0) break;
        handle_client(fd, &list, &slot, n_ctx, n_threads, buf);
        close(fd);
   }

   free(buf);
   model_unload(&slot);
   close(lfd);
   unlink(sock_path);
   return 0;
}
