#include "chat.h"
#include "skills.h"
#include "skills/ls/ls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s -m MODEL.gguf [-c CTX] [-t THREADS] [-n PREDICT]\n"
        "          [-s SKILLS_DIR] [--skill NAME] [--input TEXT] [-v]\n"
        "\n"
        "  --skill NAME   activate a skill (one-shot mode, or initial skill in the REPL)\n"
        "  --input TEXT   run one task and exit; requires --skill\n"
        "\n"
        "REPL commands:\n"
        "  /skill NAME    switch the active skill (resets task context)\n"
        "  /skill         show the active skill\n"
        "  /skills        list available skills\n"
        "  /reset         clear task context, keep the system prompt warm\n"
        "  /exit          quit\n",
        prog);
}

int main (int argc, char **argv)
{
    
    chat_params_t p = chat_default_params();
    const char *skills_dir = NULL;
    const char *skill_name = NULL;
    const char *one_shot = NULL;

    for (int i = 1; i < argc; i++)
    {
        if      (!strcmp(argv[i], "-m") && i + 1 < argc) p.model_path = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) p.n_ctx      = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) p.n_threads  = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) skills_dir   = argv[++i];
        else if (!strcmp(argv[i], "--skill") && i + 1 < argc) skill_name = argv[++i];
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) one_shot  = argv[++i];
        else if (!strcmp(argv[i], "-v")) p.verbose = 1;
        else { usage(argv[0]); return 1; }
    }
    
    if (!p.model_path)
    {
        fprintf(stderr, "error: -m MODEL.gguf is required\n");
        usage(argv[0]);
        return 1;
    }

    if (one_shot && !skill_name)
    {
        fprintf(stderr, "error: --input requires --skill\n");
        return 1;
    }

    /* Registry */
    /* -------- */

    skill_registry_t reg;;
    skills_init(&reg);

    if (skills_register(&reg, "ls", "ls/SKILL.md", skill_ls) != 0)
    {
        fprintf(stderr, "error: failed to register skills\n");
        skills_free(&reg);
        return 1;
    }

    if (skills_load(&reg, skills_dir) != 0)
    {
        skills_free(&reg);
        return 1;
    }

    if (skill_name && !skills_find(&reg, skill_name))
    {
        fprintf(stderr, "error: no such skill: %s\navailable:\n", skill_name);
        skills_list(&reg, stderr);
        skills_free(&reg);
        return 1;
    }

    p.system_prompt = skills_base_prompt(&reg);
    if (!p.system_prompt)
    {
        fprintf(stderr, "error: BASE.md is empty\n");
        skills_free(&reg);
        return 1;
    }

    chat_session_t s;
    if (chat_open(&s, &p) != 0)
    {
        skills_free(&reg);
        return 1;
    }

    chat_set_dispatch(&s, skills_dispatch, &reg);

    if (one_shot)
    {
        sbuf_t reply = {0};
        int rc = chat_send(&s, skills_prompt_for(&reg, skill_name), one_shot,
                    p.n_predict, &reply);
                
        sbuf_free(&reply);
        chat_close(&s);
        skills_free(&reg);
        return rc == 0 ? 0 : 1;
    }

    /* REPL */
    /* ---- */

    fprintf(stderr, "ready (%zu skill%s from %s). /skills to list, /exit to quit.\n",
            reg.n, reg.n == 1 ? "" : "s", reg.dir);
    if (skill_name) fprintf(stderr, "active skill: %s\n", skill_name);
    fprintf(stderr, "\n");

    char line[4096];
    for (;;)
    {
        fputs("> ", stderr);
        fflush(stderr);

        if (!fgets(line, sizeof line, stdin)) break;

        size_t n = strlen(line);
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n == 0) continue;

        if (!strcmp(line, "/exit") || !strcmp(line, "/quit")) break;

        if (!strcmp(line, "/skills"))
        {
            skills_list(&reg, stderr);
            continue;
        }

        if (!strncmp(line, "/skill", 6))
        {
            const char *arg = line + 6;
            while (*arg == ' ' || *arg == '\t') arg++;

            if (!arg)
            {
                fprintf(stderr, "active skill: %s\n", skill_name ? skill_name : "(none)");
                continue;
            }
            if (!skills_find(&reg, arg))
            {
                fprintf(stderr, "no such skill: %s\navailable:\n", arg);
                skills_list(&reg, stderr);
                continue;
            }

            chat_reset(&s);

            const skill_t *sk = skills_find(&reg, arg);
            skill_name = sk->name;
            fprintf(stderr, "active skill: %s (context cleared)\n", skill_name);
            continue;
        }

         sbuf_t reply = {0};
         if (chat_send(&s, skill_name ? skills_prompt_for(&reg, skill_name) : NULL,
                     line, p.n_predict, &reply) != 0)
         {
            fprintf(stderr, "chat: send failed\n");
         }
         sbuf_free(&reply);
    }

    chat_close(&s);
    skills_free(&reg);
    
    return 0;
}
