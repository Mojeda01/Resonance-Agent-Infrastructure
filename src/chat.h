#ifndef AGENT_CHAT_H
#define AGENT_CHAT_H

#include "llama.h"
#include <stddef.h>

typedef struct 
{
    char *data;
    size_t len;
    size_t cap;
} sbuf_t;

int sbuf_append(sbuf_t *b, const char *s, size_t n);
int sbuf_cat(sbuf_t *b, const char *s);
void sbuf_reset(sbuf_t *b);
void sbuf_free(sbuf_t *b);

typedef int (*tool_dispatch_fn) 
    (
        const char *name,
        const char *args,
        sbuf_t *out,
        void *user
    );

#define CHAT_MAX_MESSAGES 128
#define CHAT_MAX_TOOL_HOPS 3

typedef struct {
    struct llama_model *model;
    struct llama_context *ctx;
    const struct llama_vocab *vocab;
    struct llama_sampler *smpl;
    const char *tmpl;

    struct llama_chat_message msgs[CHAT_MAX_MESSAGES];
    int n_msgs;
    
    int n_persistent_msgs;

    llama_token *kv_tokens;
    int kv_len;
    int n_ctx;

    tool_dispatch_fn dispatch;
    void *dispatch_user;

    int verbose;
} chat_session_t;

typedef struct {
    const char *model_path;
    const char *system_prompt;
    int n_ctx;
    int n_threads;
    int n_predict;
    float temp;
    int verbose;
} chat_params_t;

chat_params_t chat_default_params(void);

int chat_open(chat_session_t *s, const chat_params_t *p);
void chat_close(chat_session_t *s);

void chat_set_dispatch(chat_session_t *s, tool_dispatch_fn fn, void *user);

int chat_send(chat_session_t *s, const char *skill_prompt, const char *user_msg,
        int n_predict, sbuf_t *reply);

void chat_reset(chat_session_t *s);

#endif

