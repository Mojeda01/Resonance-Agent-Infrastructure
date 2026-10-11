#ifndef MODELS_H
#define MODELS_H

#include "llama.h"

#define MODELS_MAX 32
#define MODEL_NAME_MAX 128
#define MODEL_PATH_MAX 512

typedef struct {
    char name[MODEL_NAME_MAX];
    char path[MODEL_PATH_MAX];
    long bytes;
} model_entry;

typedef struct {
    model_entry entries[MODELS_MAX];
    int count;
    char dir[MODEL_PATH_MAX];
} model_list;

typedef struct {
    struct llama_model *model;
    struct llama_context *ctx;
    char name[MODEL_NAME_MAX];
    int n_ctx;
    int n_threads;
    int n_past;
    int loaded;
} model_slot;

int models_scan(const char *dir, model_list *out);
int models_find(const model_list *l, const char *name);
int model_load(model_slot *s, const model_list *l,
                const char *name, int n_ctx, int n_threads);
void model_unload(model_slot *s);

#endif
