#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llama.h"
#include "models.h"
#include "server.h"
#include "session.h"
#include "tools.h"

static char *g_base;

int agent_load_base(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "base: cannot open %s\n", path); return -1; }

    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return -1; }

    char *b = malloc((size_t)n + 1);
    if (!b) { fclose(f); return -1; }

    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[got] = '\0';

    free(g_base);
    {
        const char *tp = tools_prompt();
        size_t nb = strlen(b), nt = strlen(tp);
        char *m = malloc(nb + nt + 3);
        if (m) { snprintf(m, nb + nt + 3, "%s\n\n%s", b, tp); free(b); b = m; }
    }
    g_base = b;
    fprintf(stderr, "base: loaded %zu bytes from %s\n", got, path);
    return 0;
}

void agent_reset(model_slot *ms, session *s)
{
    if (!ms->loaded) return;
    llama_memory_seq_rm(llama_get_memory(ms->ctx), s->seq, -1, -1);
    s->n_past = 0;
}

static char *apply_template(const model_slot *ms, const char *prompt, int first_turn) {
    
    char *merged = NULL;
    if (first_turn && g_base)
    {
        size_t n = strlen(g_base) + strlen(prompt) + 4;
        merged = malloc(n);
        if (!merged) return NULL;
        snprintf(merged, n, "%s\n\n%s", g_base, prompt);
    }
    const char *user = merged ? merged : prompt;  

    static const char *PRE  = "<start_of_turn>user\n";
    static const char *POST = "<end_of_turn>\n<start_of_turn>model\n";

    size_t cap = strlen(PRE) + strlen(user) + strlen(POST) + 1;
    char *buf = malloc(cap);
    if (!buf) { free(merged); return NULL; }

    snprintf(buf, cap, "%s%s%s", PRE, user, POST);
    free(merged);

    return buf;
}

// <think> spans token boundaries, so filter on accumulated text.
typedef struct {
    tok_cb cb;
    void *ud;
    char hold[32];
    int hn;
    int in_think;
    tbuf *seen;
} think_filter;

static int tf_emit(think_filter *f, const char *p, int n)
{
   for (int i = 0; i < n; i++)
   {
       if (f->hn < (int)sizeof(f->hold) - 1) f->hold[f->hn++] = p[i];
       f->hold[f->hn] = '\0';

       const char *open = strstr(f->hold, "<think>");
       const char *close = strstr(f->hold, "</think>");
       if (!f->in_think && open) { f->in_think = 1; f->hn = 0; continue; }
       if ( f->in_think && close) { f->in_think = 0; f->hn = 0; continue; }

       if (f->hn >= 8 || (f->hold[0] != '<' && f->hn >= 1))
       {
            int keep = (f->hold[f->hn - 1] == '<') ? 1 : 0;
            int out  = f->hn - keep;
            if (!f->in_think && out > 0)
            {
                if (f->cb(f->hold, out, TOK_TEXT, f->ud) < 0) return -1; 
                if (f->seen) tbuf_append(f->seen, f->hold, (size_t)out);
            }
            if (keep) f->hold[0] = '<';
            f->hn = keep;
       }
   }
   return 0;
}

static int decode_seq(struct llama_context *ctx, llama_token *toks,
                int32_t n, int32_t seq, int32_t pos0, int last_logit)
{
    struct llama_batch b = llama_batch_init(n, 0, 1);
    b.n_tokens = n;
    for (int32_t i = 0; i < n; i++)
    {
        b.token[i] = toks[i];
        b.pos[i] = pos0 + i;
        b.n_seq_id[i] = 1;
        b.seq_id[i][0] = seq;
        b.logits[i] = 0;
    }
    if (last_logit) b.logits[n - 1] = 1;
    int rc = llama_decode(ctx, b);
    if (rc != 0)
    {
        fprintf(stderr, "decode failed rc=%d seq=%d pos=%d n=%d\n",
                rc, seq, pos0, n);
    }
    llama_batch_free(b);
    return rc;
}


int agent_generate(model_slot *ms, session *s, const char *prompt,
            tok_cb cb, void *ud)
{
    if (!ms->loaded) return -1;

    const struct llama_vocab *vocab = llama_model_get_vocab(ms->model); 

    /* No template in the gguf — fall back to the raw text. */
    char *formatted = apply_template(ms, prompt, s->n_past == 0); 
    const char *text = formatted ? formatted : prompt;

    int32_t plen = (int32_t)strlen(text);
    int32_t cap  = plen + 8;

    llama_token *toks = malloc((size_t)cap * sizeof(llama_token));
    if (!toks) { free(formatted); return -1; }

    /* BOS only on the first turn; parse_special so the turn markers
       become real control tokens instead of literal text */
    bool add_bos = (s->n_past == 0);

    int32_t n = llama_tokenize(vocab, text, plen, toks, cap, add_bos, true);
    if (n < 0) {
        cap = -n;
        llama_token *t2 = realloc(toks, (size_t)cap * sizeof(llama_token));
        if (!t2) { free(toks); free(formatted); return -1; }
        toks = t2;
        n = llama_tokenize(vocab, text, plen, toks, cap, add_bos, true);
    }
    free(formatted);

    if (n <= 0) { free(toks); return -1; }
    if (s->n_past + n >= ms->n_ctx - 1) { free(toks); return -2; } 

    
    if (decode_seq(ms->ctx, toks, n, s->seq, s->n_past, 1) != 0)
    {
        free(toks); return -1;
    }
    free(toks);
    s->n_past += n;

    struct llama_sampler *smpl =
        llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(64));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(smpl,
            llama_sampler_init_penalties(256, 1.10f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(1.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    int  rc = 0;
    char piece[256];
    tbuf seen;
    tbuf_init(&seen);
    think_filter tf = { cb, ud, {0}, 0, 0, &seen };
    char *tool_call = NULL;
    int hops = 0;

    while (s->n_past < ms->n_ctx - 1) {
        llama_token tok = llama_sampler_sample(smpl, ms->ctx, -1);
        if (llama_vocab_is_eog(vocab, tok))
        {
            if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 0) == 0)
            {
                s->n_past++;
            }
            break;
        }

        llama_sampler_accept(smpl, tok);

        int32_t pn = llama_token_to_piece(vocab, tok, piece,
                                          (int32_t)sizeof(piece), 0, false);
        if (pn < 0) { rc = -1; break; }

        if (tf_emit(&tf, piece, (int)pn) < 0) break; 
        
        if (seen.data)
        {
            char *m = strstr(seen.data, "TOOL:");
            char *e = m ? strstr(m, "\nEND") : NULL;
            if (e)
            {
                size_t n_call = (size_t)(e - (m + 5));
                tool_call = malloc(n_call + 1);
                if (tool_call)
                {
                    memcpy(tool_call, m + 5, n_call);
                    tool_call[n_call] = '\0';
                }
                if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 1) == 0)
                {
                    s->n_past++;
                }
                break;
            }
        }

        if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 1) != 0)
        {
            rc = -1; break;
        }
        s->n_past++;
    }

    if (!tf.in_think && tf.hn > 0) cb(tf.hold, tf.hn, TOK_TEXT, ud); 
    
    fprintf(stderr, "=== seen [%s] ===\n", seen.data ? seen.data : "(null)");

    while (tool_call && hops < TOOL_MAX_HOPS && rc == 0)
    {
        hops++;
        fprintf(stderr, "=== hop %d call [%s] ===\n", hops, tool_call);
        cb(tool_call, (int)strlen(tool_call), TOK_TOOL_CALL, ud);

        tbuf res;
        tbuf_init(&res);
        int unknown = (tools_dispatch(tool_call, &res) < 0);
        if (unknown)
        {
            tbuf_cat(&res, "error: unknown tool\n");
        }
        if (res.data)
        {
            cb(res.data, (int)res.len,
                    unknown ? TOK_TOOL_ERROR : TOK_TOOL_RESULT, ud);
        }
        free(tool_call);
        tool_call = NULL;

        size_t rlen = res.data ? res.len : 0;
        char *feed = malloc(rlen + 64);
        if (!feed) { tbuf_free(&res); rc = -1; break; }
        snprintf(feed, rlen + 64, "<tool_result>\n%s\n</tool_result>",
                res.data ? res.data : "");
        tbuf_free(&res);

        char *ft = apply_template(ms, feed, 0);
        const char *ftext = ft ? ft : feed;

        int32_t flen = (int32_t)strlen(ftext);
        int32_t fcap = flen + 8;
        llama_token *ftoks = malloc((size_t)fcap * sizeof(llama_token));
        if (!ftoks) { free(ft); free(feed); rc = -1; break; }

        int32_t fn = llama_tokenize(vocab, ftext, flen, ftoks, fcap, false, true);
        if (fn < 0)
        {
            fcap = -fn;
            llama_token *t2 = realloc(ftoks, (size_t)fcap * sizeof(llama_token));
            if (!t2) { free(ftoks); free(ft); free(feed); rc = -1; break; }
            ftoks = t2;
            fn = llama_tokenize(vocab, ftext, flen, ftoks, fcap, false, true);
        }
        free(ft); free(feed);

        if (fn <= 0 || s->n_past + fn >= ms->n_ctx - 1)
        {
            free(ftoks); rc = (fn <= 0) ? -1 : -2; break;
        }

        if (decode_seq(ms->ctx, ftoks, fn, s->seq, s->n_past, 1) != 0)
        {
            free(ftoks); rc = -1; break;
        }

        free(ftoks);
        s->n_past += fn;

        tbuf_free(&seen);
        tbuf_init(&seen);
        tf.hn = 0; tf.in_think = 0;

        while (s->n_past < ms->n_ctx - 1)
        {
            llama_token tok = llama_sampler_sample(smpl, ms->ctx, -1);
            if (llama_vocab_is_eog(vocab, tok))
            {
                if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 0) == 0)
                {
                    s->n_past++;
                }
                break;
            }
            llama_sampler_accept(smpl, tok);

            int32_t pn = llama_token_to_piece(vocab, tok, piece,
                            (int32_t)sizeof(piece), 0, false);
            if (pn < 0) { rc = -1; break; }
            if (tf_emit(&tf, piece, (int)pn) < 0) break;

            if (seen.data)
            {
                char *m = strstr(seen.data, "TOOL:");
                char *e = m ? strstr(m, "\nEND") : NULL;
                if (e)
                {
                    size_t n_call = (size_t)(e - (m + 5));
                    tool_call = malloc(n_call + 1);
                    if (tool_call)
                    {
                        memcpy(tool_call, m + 5, n_call);
                        tool_call[n_call] = '\0';
                    }
                    if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 1) == 0)
                    {
                        s->n_past++;
                    }
                    break;
                }
                // model left its own turn, do not let it write ours.
                if (strstr(seen.data, "<start_of_turn>") || strstr(seen.data, "<|channel"))
                {
                    if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 1) == 0)
                    {
                        s->n_past++;
                    }
                    break;
                }
                if (!m)
                {
                    char *b = strstr(seen.data, "\nEND");
                    if (b && (b[4] == '\0' || b[4] == '\n'))
                    {
                        if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 1) == 0)
                        {
                            s->n_past++;
                        }
                        break;
                    }
                }
            }
            if (decode_seq(ms->ctx, &tok, 1, s->seq, s->n_past, 1) != 0)
            {
                rc = -1; break;
            }
            s->n_past++;
        }
        if (!tf.in_think && tf.hn > 0) { cb(tf.hold, tf.hn, TOK_TEXT, ud); tf.hn = 0; } 
        fprintf(stderr, "=== hop %d resumed [%s] ===\n", hops,
                seen.data ? seen.data : "(null)");
    }
    free(tool_call);
    tbuf_free(&seen);
    llama_sampler_free(smpl);
    return rc;
}
