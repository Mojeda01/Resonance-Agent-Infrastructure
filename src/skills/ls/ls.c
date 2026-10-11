#include "ls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#define LS_MAX_ENTRIES 200

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int skill_ls(const char *args, sbuf_t *out)
{
    const char *path = args;
    if (!path || !*path) path = ".";
    
    DIR *d = opendir(path);
    if (!d)
    {
        sbuf_cat(out, "cannot open '");
        sbuf_cat(out, path);
        sbuf_cat(out, "': ");
        sbuf_cat(out, strerror(errno));
        return 1;
    }

    char **names = NULL;
    size_t n = 0, cap = 0;
    int truncated = 0;

    struct dirent *e;
    while ((e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        if (n >= LS_MAX_ENTRIES) { truncated = 1; break; }

        if (n == cap)
        {
            size_t ncap = cap ? cap * 2 : 32;
            char **p = realloc(names, ncap * sizeof *p);
            if (!p) { truncated = 1; break; }
            names = p;
            cap = ncap;
        }
        
        names[n] = strdup(e->d_name);
        if (!names[n]) { truncated = 1; break; }
        n++;
    }

    closedir(d);

    qsort(names, n, sizeof *names, cmp_str);

    if (n == 0)
    {
        sbuf_cat(out, "(directory is empty)");
    }
    else {
        for (size_t i = 0; i < n; i++)
        {
            char full[4096];
            struct stat st;
            int is_dir = 0;

            if (snprintf(full, sizeof full, "%s/%s", path, names[i]) < (int)sizeof full && 
                    stat(full, &st) == 0 && S_ISDIR(st.st_mode))
            {
                is_dir = 1;
            }

            sbuf_cat(out, names[i]);
            if (is_dir) sbuf_cat(out, "/");
            sbuf_cat(out, "\n");
        }
    }

    if (truncated)
    {
        sbuf_cat(out, "... (listing truncated)\n");
    }

    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return 0;
}
