#include "skills.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int is_dir(const char *path)
{
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f,0,SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t got = fread(buf,1,(size_t)sz,f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static char *join_path(const char *a, const char *b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    int need_sep = (na > 0 && a[na - 1] != '/');
    char *p = malloc(na + need_sep + nb + 1);
    if (!p) return NULL;

    memcpy(p,a,na);
    if (need_sep) p[na] = '/';
    memcpy(p+na+need_sep,b,nb);
    p[na+need_sep+nb] = '\0';
    return p;
}

void skills_init(skill_registry_t *r)
{
    memset(r,0,sizeof *r);
}

void skills_free(skill_registry_t *r)
{
    for (size_t i = 0; i < r->n; i++) free(r->v[i].prompt);
    free(r->v);
    free(r->base_prompt);
    free(r->dir);
    memset(r,0,sizeof *r);
}

int skills_register(skill_registry_t *r, const char *name, const char *md_path,
                    int (*run)(const char *args, sbuf_t *out))
{
    if (!name || !run) return -1;

    if (r->n == r->cap)
    {
        size_t cap = r->cap ? r->cap * 2 : 8;
        skill_t *v = realloc(r->v,cap * sizeof *v);
        if (!v) return -1;
        r->v = v;
        r->cap = cap;
    }

    r->v[r->n].name = name;
    r->v[r->n].md_path = md_path;
    r->v[r->n].run = run;
    r->v[r->n].prompt = NULL;
    r->n++;
    return 0;
}

static char *resolve_dir(const char *dir)
{
    if (dir)
    {
        if (!is_dir(dir))
        {
            fprintf(stderr, "skills: not a directory: %s\n", dir);
            return NULL;
        }
        return strdup(dir);
    }

    const char *env = getenv("AGENT_SKILLS_DIR");
    if (env && is_dir(env)) return strdup(env);

    static const char *candidates[] = { "src/skills", "skills", NULL};
    for (int i = 0; candidates[i]; i++)
    {
        if (is_dir(candidates[i])) return strdup(candidates[i]);
    }
    fprintf(stderr,
            "skills: could not locate skills directory "
            "(set AGENT_SKILLS_DIR or run from the project root)\n");
    return NULL;
}

int skills_load(skill_registry_t *r, const char *dir)
{
    r->dir = resolve_dir(dir);
    if (!r->dir) return -1;

    char *base_path = join_path(r->dir, "BASE.md");
    if (!r->dir) return -1;

    r->base_prompt = read_file(base_path);
    if (!r->base_prompt)
    {
        fprintf(stderr, "skills: cannot read %s\n", base_path);
        free(base_path);
        return -1;
    }
    free(base_path);

    for (size_t i = 0; i < r->n; i++)
    {
        if (!r->v[i].md_path) continue;
        char *p = join_path(r->dir, r->v[i].md_path);
        if (!p) return -1;

        r->v[i].prompt = read_file(p);
        if (!r->v[i].prompt)
        {
            fprintf(stderr, "skills: warning: cannot read %s "
                    "(skill '%s' will have no description)\n",
                    p, r->v[i].name); 
        }
        free(p);
    }
    return 0;
}

const char *skills_base_prompt(const skill_registry_t *r)
{
    return r->base_prompt;
}

const char *skills_prompt_for(const skill_registry_t *r, const char *name)
{
    const skill_t *s = skills_find(r, name);
    return s ? s->prompt : NULL;
}

void skills_list(const skill_registry_t *r, FILE *f)
{
    for (size_t i = 0; i < r->n; i++)
    {
        fprintf(f, "  %s%s\n", r->v[i].name,
                r->v[i].prompt ? "" : "  (no description loaded)");
    }
}

const skill_t *skills_find(const skill_registry_t *r, const char *name)
{
    for (size_t i = 0; i < r->n; i++)
    {
        if (strcmp(r->v[i].name, name) == 0) return &r->v[i];
    }
    return NULL;
}

int skills_dispatch(const char *name, const char *args, sbuf_t *out, void *user){
    const skill_registry_t *r = (const skill_registry_t *)user;
    const skill_t *s = skills_find(r,name);
    if (!s)
    {
        sbuf_cat(out, "no such tool: ");
        sbuf_cat(out, name);
        return 1;
    }
    return s->run(args, out);
}
