#include "chat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int sbuf_append(sbuf_t *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 1024;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p) return -1;
        b->data = p;
        b->cap = cap;
    }

    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int sbuf_cat(sbuf_t *b, const char *s) { return sbuf_append(b,s,strlen(s)); }

void sbuf_reset(sbuf_t * b) { b->len = 0; if (b->data) b->data[0] = '\0'; }

void sbuf_free(sbuf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void quiet_log(enum ggml_log_level lvl, const char *txt, void *u)
{
    (void)lvl; (void)txt; (void)u;
}

static int msg_push(chat_session_t *s, const char *role, const char *content)
{
    if (s->n_msgs >= CHAT_MAX_MESSAGES)
    {
        fprintf(stderr, "chat: message history full\n");
        return -1;
    }
    char *r = strdup(role);
    char *c = strdup(content);
    if (!r || !c) { free(r); free(c); return -1; }

    s->msgs[s->n_msgs].role = r;
    s->msgs[s->n_msgs].content = c;
    s->n_msgs++;
    return 0;
}

static void msgs_free(chat_session_t *s)
{
    for (int i = 0; i < s->n_msgs; i++)
    {
        free((void *)s->msgs[i].role);
        free((void *)s->msgs[i].content);
    }
    s->n_msgs = 0;
}

static void msgs_truncate(chat_session_t *s, int keep)
{
    for (int i = keep; i < s->n_msgs; i++)
    {
        free((void *)s->msgs[i].role);
        free((void *)s->msgs[i].content);
    }
    if (s->n_msgs > keep) s->n_msgs = keep;
}

static int render_transcript(chat_session_t *s, sbuf_t *out)
{
    sbuf_reset(out);

    if (!s->tmpl)
    {
        for (int i = 0; i < s->n_msgs; i++)
        {
            sbuf_cat(out, s->msgs[i].role);
            sbuf_cat(out, ": ");
            sbuf_cat(out, s->msgs[i].content);
            sbuf_cat(out, "\n");
        }
        sbuf_cat(out, "assistant: ");
        return 0;
    }
    int need = llama_chat_apply_template(s->tmpl, s->msgs, (size_t)s->n_msgs,
                                        true, NULL, 0);
    if (need <= 0) return -1;
    char *buf = malloc((size_t)need + 1);
    if (!buf) return -1;
      
    int got = llama_chat_apply_template(s->tmpl, s->msgs, (size_t)s->n_msgs,
                true, buf, need + 1);


    if (got <= 0) { free(buf); return -1; }
    buf[got] = '\0';

    int rc = sbuf_append(out, buf, (size_t)got);
    free(buf);
    return rc;
}

static int prefill (chat_session_t *s, const llama_token *toks, int n)
{
    int common = 0;
    while (common < s->kv_len && common < n && s->kv_tokens[common] 
            == toks[common])
    {
        common++;
    }

    if (common < s->kv_len)
    {
        llama_memory_t mem = llama_get_memory(s->ctx);
        llama_memory_seq_rm(mem, 0, common, -1);
        s->kv_len = common;
    }

    if (common == n) common = n - 1;
    if (common < 0) common = 0;

    if (s->verbose && common > 0)
    {
        fprintf(stderr, "[cache: reused %d/%d tokens]\n", common, n);
    }

    const int nb = 256;
    for (int i = common; i < n; i += nb)
    {
        int chunk = (n - i < nb) ? (n - i) : nb;
        struct llama_batch  b = llama_batch_get_one((llama_token *)(toks + i),chunk);
        if (llama_decode(s->ctx, b) != 0)
        {
            fprintf(stderr, "chat: decode failed during prefill\n");
            return -1;
        }
    }

    memcpy(s->kv_tokens, toks, (size_t)n * sizeof(llama_token));
    s->kv_len = n;
    return 0;
}

static int generate(chat_session_t *s, int n_predict, sbuf_t *out)
{
    sbuf_t rendered = {0};
    llama_token *toks = NULL;
    int rc = -1;

    if (render_transcript(s, &rendered) != 0) goto done;

    if (s->verbose)
        fprintf(stderr, "--- prompt ---\n%s\n--------------\n", rendered.data);

    int n = -llama_tokenize(s->vocab, rendered.data, (int)rendered.len,
                            NULL, 0, true, true);
    if (n <= 0) { fprintf(stderr, "chat: tokenize failed\n"); goto done; }

    if (n >= s->n_ctx - 8) {
        fprintf(stderr, "chat: transcript (%d tokens) too long for context (%d)\n",
                n, s->n_ctx);
        goto done;
    }

    toks = malloc((size_t)n * sizeof(llama_token));
    if (!toks) goto done;

    if (llama_tokenize(s->vocab, rendered.data, (int)rendered.len,
                       toks, n, true, true) < 0) goto done;

    if (prefill(s, toks, n) != 0) goto done;

    llama_sampler_reset(s->smpl);

    char piece[512];
    for (int i = 0; i < n_predict; i++) {
        if (s->kv_len >= s->n_ctx - 1) {
            fprintf(stderr, "\nchat: context full\n");
            break;
        }

        llama_token tok = llama_sampler_sample(s->smpl, s->ctx, -1);
        if (llama_vocab_is_eog(s->vocab, tok)) break;

        int np = llama_token_to_piece(s->vocab, tok, piece, sizeof piece, 0, true);
        if (np > 0) {
            sbuf_append(out, piece, (size_t)np);
            fwrite(piece, 1, (size_t)np, stdout);
            fflush(stdout);
        }

        if (s->kv_len < s->n_ctx) s->kv_tokens[s->kv_len++] = tok;

        struct llama_batch b = llama_batch_get_one(&tok, 1);
        if (llama_decode(s->ctx, b) != 0) {
            fprintf(stderr, "\nchat: decode failed\n");
            break;
        }
    }

    rc = 0;

done:
    free(toks);
    sbuf_free(&rendered);
    return rc;
}

static const char *skip_think(const char *text)
{
    const char *end = strstr(text, "</think>");
    if (!end) return text;
    end += 8;
    while (*end == '\n' || *end == '\r' || *end == ' ') end++;
    return end;
}

static int at_line_start(const char *base, const char *p)
{
    return p == base || p[-1] == '\n';
}

static int find_tool_call(const char *text, char *name, size_t nlen,
                            const char **args_out)
{
    
    const char *body = skip_think(text);
    const char *p = body;

    for (;;)
    {
        p = strstr(p, "TOOL:");
        if (!p) return 0;
        if (at_line_start(body,p)) break;
        p += 5;
    }

    p += 5;
    while (*p == ' ' || *p == '\t') p++;

    size_t i = 0;
    while (*p && !isspace((unsigned char)*p) && i + 1 < nlen)
    {
        name[i++] = *p++;
    }
    name[i] = '\0';
    if (i == 0) return 0;

    while (*p == ' ' || *p == '\t') p++;
    *args_out = p;
    return 1;
}

static void trim_line(char *s)
{
    char *nl = strpbrk(s, "\r\n");
    if (nl) *nl = '\0';
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n-1])) s[--n] = '\0';
}

// public API
chat_params_t chat_default_params(void)
{
    chat_params_t p;
    p.model_path = NULL;
    p.system_prompt = NULL;
    p.n_ctx = 4096;
    p.n_threads = 4;
    p.n_predict = 512;
    p.temp = 0.4f;
    p.verbose = 0;
    return p;
}

int chat_open (chat_session_t *s, const chat_params_t *p)
{
    memset(s,0,sizeof *s);
    s->verbose = p->verbose;

    if (!p->verbose) llama_log_set(quiet_log,NULL);

    llama_backend_init();

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;

    s->model = llama_model_load_from_file(p->model_path, mp);
    if (!s->model)
    {
        fprintf(stderr, "chat: failed to load model: %s\n", p->model_path);
        llama_backend_free();
        return -1;
    }

    s->vocab = llama_model_get_vocab(s->model);
    s->tmpl = llama_model_chat_template(s->model,NULL);

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)p->n_ctx;
    cp.n_batch = 256;
    cp.n_ubatch = 128;
    cp.n_threads = p->n_threads;
    cp.n_threads_batch = p->n_threads;

    s->ctx = llama_init_from_model(s->model, cp);
    if (!s->ctx)
    {
        fprintf(stderr, "chat: failed to create context");
        llama_model_free(s->model);
        llama_backend_free();
        return -1;
    }

    s->n_ctx = p->n_ctx;
    s->kv_tokens = malloc((size_t)p->n_ctx * sizeof(llama_token));
    if (!s->kv_tokens)
    {
        chat_close(s);
        return -1;
    }
    s->kv_len = 0;

    struct llama_sampler_chain_params scp = llama_sampler_chain_default_params();
    s->smpl = llama_sampler_chain_init(scp);
    llama_sampler_chain_add(s->smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(s->smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(s->smpl, llama_sampler_init_temp(p->temp));
    llama_sampler_chain_add(s->smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    if (p->system_prompt && msg_push(s, "system", p->system_prompt) != 0)
    {
        chat_close(s);
        return -1;
    }
    
    s->n_persistent_msgs = s->n_msgs;

    return 0;
}

void chat_close(chat_session_t *s)
{
    msgs_free(s);
    if (s->smpl) llama_sampler_free(s->smpl);
    if (s->ctx) llama_free(s->ctx);
    if (s->model) llama_model_free(s->model);
    free(s->kv_tokens);
    llama_backend_free();
    memset(s, 0, sizeof *s);
}

void chat_set_dispatch(chat_session_t *s, tool_dispatch_fn fn, void *user)
{
    s->dispatch = fn;
    s->dispatch_user = user;
}

void chat_reset(chat_session_t *s)
{
    msgs_truncate(s, s->n_persistent_msgs);
    if (s->verbose)
    {
        fprintf(stderr, "[reset: %d message(s) kept]\n", s->n_msgs);
    }
}

int chat_send(chat_session_t *s, const char *skill_prompt, const char *user_msg,
                int n_predict, sbuf_t *reply)
{
    if (skill_prompt && *skill_prompt)
    {
        sbuf_t m = {0};
        sbuf_cat(&m, skill_prompt);
        sbuf_cat(&m, "\n\n---\n\n");
        sbuf_cat(&m, user_msg);

        int rc = msg_push(s, "user", m.data);
        sbuf_free(&m);
        if (rc != 0) return -1;
    } else {
        if (msg_push(s, "user", user_msg) != 0) return -1;
    }

    for (int hop = 0; hop <= CHAT_MAX_TOOL_HOPS; hop++)
    {
        sbuf_t turn = {0};

        if (generate(s,n_predict,&turn) != 0)
        {
            sbuf_free(&turn);
            return  -1;
        }
        printf("\n");
        
        const char *visible = turn.data ? skip_think(turn.data) : "";
        if (msg_push(s, "assistant", visible) != 0) 
        {
            sbuf_free(&turn);
            return -1;
        }

        char name[64];
        const char *args_ptr = NULL;

        if (!s->dispatch || hop == CHAT_MAX_TOOL_HOPS || !turn.data ||
                !find_tool_call(turn.data, name, sizeof name, &args_ptr))
        {
            if (reply) sbuf_cat(reply, visible); 
            sbuf_free(&turn);
            return 0;
        }

        char *args = strdup(args_ptr ? args_ptr : "");
        if (!args) { sbuf_free(&turn); return -1; }
        trim_line(args);

        sbuf_t result = {0};
        int trc = s->dispatch(name, args, &result, s->dispatch_user);

        if (result.len == 0)
        {
            sbuf_cat(&result, trc == 0 ? "(no output)" : "(tool failed)");
        }

        printf("\n[%s %s] -> %zu bytes\n", name, args, result.len);
        if (s->verbose) printf("%s\n", result.data);

        sbuf_t obs = {0};
        sbuf_cat(&obs, "Output of `");
        sbuf_cat(&obs, name);
        if (*args) { sbuf_cat(&obs, " "); sbuf_cat(&obs, args); }
        sbuf_cat(&obs, "`:\n");
        sbuf_cat(&obs, result.data);
        sbuf_cat(&obs, "\n\nDescribe this result for the user in plain prose. "
                        "Do not emit another TOOL: line.");


        int prc = msg_push(s, "user", obs.data);

        free(args);
        sbuf_free(&obs);
        sbuf_free(&result);
        sbuf_free(&turn);

        if (prc != 0) return -1;
    }
    return 0;
}
