#ifndef SESSION_H
#define SESSION_H

#include <pthread.h>
#include <stddef.h>
#include <time.h>

#include "llama.h"

#define SESSIONS_MAX 8 

enum {
    SESS_IDLE = 0,
    SESS_QUEUED,
    SESS_RUNNING,
    SESS_DONE,
    SESS_ERROR
};

typedef struct {
    int id;
    int seq;
    int n_past;
    char *buf;
    size_t len;
    size_t cap;
    int state;
    time_t last_used;
    pthread_mutex_t lock;
} session;

void sessions_init(void);

session *session_get(struct llama_context *ctx, int id);

session *session_find(int id);

void session_drop(struct llama_context *ctx, session *s);

void session_reset_buf(session *s);

int session_append(session *s, const char *p, size_t n);

size_t session_tail(session *s, size_t off, char *out, size_t max, int *state);

#endif
