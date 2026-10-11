#include <stdlib.h>
#include <string.h>

#include "session.h"

static session g_sess[SESSIONS_MAX];
static pthread_mutex_t g_table = PTHREAD_MUTEX_INITIALIZER;

void sessions_init(void)
{
    for (int i = 0; i < SESSIONS_MAX; i++)
    {
        memset(&g_sess[i], 0, sizeof(session));
        g_sess[i].seq = i;
        g_sess[i].state = SESS_IDLE;
        pthread_mutex_init(&g_sess[i].lock, NULL);
    }
}

session *session_find(int id)
{
    if (id <= 0) return NULL;
    pthread_mutex_lock(&g_table);
    session *found = NULL;
    for (int i = 0; i < SESSIONS_MAX; i++)
    {
        if (g_sess[i].id == id) { found = &g_sess[i]; break; } 
    }
    pthread_mutex_unlock(&g_table);
    return found;
}

// wipe a slot's KV and buffer. Caller holds g_table.
static void slot_clear(struct llama_context *ctx, session *s)
{
    if (ctx) llama_memory_seq_rm(llama_get_memory(ctx), s->seq, -1, -1);
    free(s->buf);
    s->buf = NULL;
    s->len = s->cap = 0;
    s->id = 0;
    s->n_past = 0;
    s->state = SESS_IDLE;
}

session *session_get(struct llama_context *ctx, int id)
{
    if (id <= 0) return NULL;

    pthread_mutex_lock(&g_table);

    for (int i = 0; i < SESSIONS_MAX; i++)
    {
        if (g_sess[i].id == id)
        {
            g_sess[i].last_used = time(NULL);
            pthread_mutex_unlock(&g_table);
            return &g_sess[i];
        }
    }

    for (int i = 0; i < SESSIONS_MAX; i++)
    {
        if (g_sess[i].id == 0)
        {
            g_sess[i].id = id;
            g_sess[i].n_past = 0;
            g_sess[i].state = SESS_IDLE;
            g_sess[i].last_used = time(NULL);
            pthread_mutex_unlock(&g_table);
            return &g_sess[i];
        }
    }

    session *old = NULL;
    for (int i = 0; i < SESSIONS_MAX; i++)
    {
        if (g_sess[i].state == SESS_RUNNING ||
                g_sess[i].state == SESS_QUEUED) continue;
        if (!old || g_sess[i].last_used < old->last_used) old = &g_sess[i];
    }
    if (!old) { pthread_mutex_unlock(&g_table); return NULL; }
    slot_clear(ctx, old);
    old->id = id;
    old->last_used = time(NULL);
    pthread_mutex_unlock(&g_table);
    return old;
}

void session_drop(struct llama_context *ctx, session *s)
{
    if (!s) return;
    pthread_mutex_lock(&g_table);
    pthread_mutex_lock(&s->lock);
    slot_clear(ctx, s);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_unlock(&g_table);
}

void session_reset_buf(session *s)
{
    pthread_mutex_lock(&s->lock);
    s->len = 0;
    if (s->buf) s->buf[0] = '\0';
    pthread_mutex_unlock(&s->lock);
}

int session_append(session *s, const char *p, size_t n)
{
    pthread_mutex_lock(&s->lock);

    if (s->len + n + 1 > s->cap)
    {
        size_t want = s->cap ? s->cap * 2 : 4096;
        while (want < s->len + n + 1) want *= 2;
        char *nb = realloc(s->buf, want);
        if (!nb) { pthread_mutex_unlock(&s->lock); return -1; }
        s->buf = nb;
        s->cap = want;
    }

    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = '\0';
    s->last_used = time(NULL);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

size_t session_tail(session *s, size_t off, char *out, size_t max, int *state)
{
   pthread_mutex_lock(&s->lock);
   size_t n = 0;
   if (off < s->len)
   {
       n = s->len - off;
       if (n > max) n = max;
       memcpy(out, s->buf + off, n);
   }
   if (state) *state = s->state;
   pthread_mutex_unlock(&s->lock);
   return n;
}
