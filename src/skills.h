#ifndef AGENT_SKILLS_H
#define AGENT_SKILLS_H

#include "chat.h"

#include <stdio.h>

typedef struct {
    const char *name;
    const char *md_path;
    int (*run)(const char *args, sbuf_t *out);
    char *prompt;
} skill_t;

typedef struct {
    skill_t *v;
    size_t n;
    size_t cap;
    char *base_prompt;
    char *dir;
} skill_registry_t;

void skills_init(skill_registry_t *r);
void skills_free(skill_registry_t *r);

int skills_register(skill_registry_t *r, const char *name, const char *md_path, 
                    int (*run)(const char *args, sbuf_t *out));
int skills_load(skill_registry_t *r, const char *dir);

const char *skills_base_prompt(const skill_registry_t *r);
const char *skills_prompt_for(const skill_registry_t *r, const char *name);
void skills_list(const skill_registry_t *r, FILE *f);
const skill_t *skills_find(const skill_registry_t *r, const char *name);

int skills_dispatch(const char *name, const char *args, sbuf_t *out, void *user);

#endif /* AGENT_SKILLS_H */
