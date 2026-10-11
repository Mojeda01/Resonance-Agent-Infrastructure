#ifndef SERVER_H
#define SERVER_H

#include "models.h"
#include "session.h"

#define TOK_TEXT 0
#define TOK_TOOL_CALL 1
#define TOK_TOOL_RESULT 2
#define TOK_TOOL_ERROR 3

typedef int (*tok_cb)(const char *text, int len, int kind, void *ud);

void agent_reset(model_slot *ms, session *s);
int  agent_load_base(const char *path);
int  agent_generate(model_slot *ms, session *s, const char *prompt,
        tok_cb cb, void *ud);

int server_run(const char *sock_path, const char *model_dir,
               int n_ctx, int n_threads);

#endif
