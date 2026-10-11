#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "models.h"
#include "session.h"

static int ends_with_gguf(const char *s)
{
    size_t n = strlen(s);
    return n > 5 && strcasecmp(s + n - 5, ".gguf") == 0;
}

static int cmp_entry(const void *a, const void *b) {
    return strcmp(((const model_entry *)a)->name,
                  ((const model_entry *)b)->name);
}

int models_scan(const char *dir, model_list *out)
{
    DIR *d = opendir(dir);
    if (!d) { fprintf(stderr, "models: cannot open %s\n", dir); return -1; }

    out->count = 0;
    snprintf(out->dir, sizeof(out->dir), "%s", dir);

    struct dirent *e;
    while ((e = readdir(d)) && out->count < MODELS_MAX)
    {
        if (e->d_name[0] == '.' || !ends_with_gguf(e->d_name)) continue;

        model_entry *m = &out->entries[out->count];
        snprintf(m->path, sizeof(m->path), "%s/%s", dir, e->d_name);

        size_t n = strlen(e->d_name) - 5;
        if (n >= MODEL_NAME_MAX) n = MODEL_NAME_MAX - 1;
        memcpy(m->name, e->d_name, n);
        m->name[n] = '\0';

        struct stat st;
        m->bytes = (stat(m->path, &st) == 0) ? (long)st.st_size : 0;
        out->count++;
    }
    closedir(d);

    qsort(out->entries, out->count, sizeof(model_entry), cmp_entry);
    return out->count;
}

int models_find(const model_list *l, const char *name)
{
    for (int i = 0; i < l->count; i++)
    {
        if (strcmp(l->entries[i].name, name) == 0) return i;
    }
    return -1;
}

void model_unload(model_slot *s)
{
    if (!s->loaded) return;
    if (s->ctx) llama_free(s->ctx);
    if (s->model) llama_model_free(s->model);
    s->ctx = NULL;
    s->model = NULL;
    s->name[0] = '\0';
    s->loaded = 0;
    s->n_past = 0;
}

int model_load(model_slot *s, const model_list *l, 
                const char *name, int n_ctx, int n_threads)
{
    int idx = models_find(l, name);
    if (idx < 0) { fprintf(stderr, "models: no such model '%s'\n", name); return -1; }

    if (s->loaded && strcmp(s->name, name) == 0) return 0;
    model_unload(s);

    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;

    s->model = llama_model_load_from_file(l->entries[idx].path, mp);
    if (!s->model) { fprintf(stderr, "models: load failed: %s\n", l->entries[idx].path); return -1; }

    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = n_ctx;
    cp.n_seq_max = SESSIONS_MAX;
    cp.n_batch = 4096;
    cp.n_threads = n_threads;
    cp.n_threads_batch = n_threads;

    s->ctx = llama_init_from_model(s->model, cp);
    if (!s->ctx)
    {
        llama_model_free(s->model);
        s->model = NULL;
        fprintf(stderr, "models: context init failed\n");
        return -1;
    }

    snprintf(s->name, sizeof(s->name), "%s", name);
    s->n_ctx = n_ctx;
    s->n_threads = n_threads;
    s->loaded = 1;
    s->n_past = 0;
    return 0;
}
