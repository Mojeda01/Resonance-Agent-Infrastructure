#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "tools.h"

void tbuf_init(tbuf *b) { b->data = NULL; b->len = 0; b->cap = 0; }

void tbuf_free(tbuf *b)
{
    free(b->data);
    tbuf_init(b);
}

int tbuf_append(tbuf *b, const char *p, size_t n)
{
    if (b->len + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *d = realloc(b->data, cap);
        if (!d) return -1;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

int tbuf_cat(tbuf *b, const char *s) { return tbuf_append(b, s, strlen(s)); }

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t'))
    {
        s[--n] = '\0';
    }
}

static int t_read_file(const char *args, tbuf *out)
{
    char path[4096];
    snprintf(path, sizeof path, "%s", args);
    trim(path);
    if (!path[0]) return tbuf_cat(out, "error: read_file needs a path\n");

    FILE *f = fopen(path, "rb");
    if (!f)
    {
        tbuf_cat(out, "error: cannot open ");
        tbuf_cat(out, path);
        tbuf_cat(out, ": ");
        tbuf_cat(out, strerror(errno));
        return tbuf_cat(out, "\n");
    }

    char chunk[8192];
    size_t total = 0, got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
    {
        if (total + got > TOOL_MAX_OUT)
        {
            tbuf_append(out, chunk, TOOL_MAX_OUT - total);
            tbuf_cat(out, "\n[truncated]\n");
            total = TOOL_MAX_OUT;
            break;
        }
        tbuf_append(out, chunk, got);
        total += got;
    }
    fclose(f);
    if (total == 0) tbuf_cat(out, "[empty file]\n");
    return 0;
}

static int t_write_file(const char *args, tbuf *out)
{
    const char *nl = strchr(args, '\n');
    if (!nl) return tbuf_cat(out, "error: write_file needs <path>\\n<content>\n");

    char path[4096];
    size_t plen = (size_t)(nl - args);
    if (plen >= sizeof path) return tbuf_cat(out, "error: path too long\n");
    memcpy(path, args, plen);
    path[plen] = '\0';
    trim(path);

    const char *body = nl + 1;
    size_t blen = strlen(body);

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        tbuf_cat(out, "error: cannot write ");
        tbuf_cat(out, path);
        tbuf_cat(out, ": ");
        tbuf_cat(out, strerror(errno));
        return tbuf_cat(out, "\n");
    }
    size_t wrote = fwrite(body, 1, blen, f);
    fclose(f);

    char msg[4200];
    snprintf(msg, sizeof msg, "wrote %zu bytes to %s\n", wrote, path);
    return tbuf_cat(out, msg);
}

static int t_exec(const char *args, tbuf *out)
{
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "%s", args);
    trim(cmd);
    if (!cmd[0]) return tbuf_cat(out, "error: exec needs a command\n");

    int fds[2];
    if (pipe(fds) < 0) return tbuf_cat(out, "error: pipe failed\n");

    pid_t pid = fork();
    if (pid < 0)
    {
        close(fds[0]); close(fds[1]);
        return tbuf_cat(out, "error: fork failed\n");
    }

    if (pid == 0)
    {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(fds[1]);
    setpgid(pid, pid);

    struct pollfd pf = { .fd = fds[0], .events = POLLIN };
    time_t deadline = time(NULL) + TOOL_EXEC_SECS;
    size_t total = 0;
    int timed_out = 0;

    for (;;)
    {
        int left = (int)(deadline - time(NULL));
        if (left <= 0) { timed_out = 1; break; }
        int r = poll(&pf, 1, left * 1000);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) { timed_out = 1; break; }
        char chunk[4096];
        ssize_t n = read(fds[0], chunk, sizeof chunk);
        if (n <= 0) break;
        if (total + (size_t)n > TOOL_MAX_OUT)
        {
            tbuf_append(out, chunk, TOOL_MAX_OUT - total);
            tbuf_cat(out, "\n[output truncated]\n");
            total = TOOL_MAX_OUT;
            break;
        }
        tbuf_append(out, chunk, (size_t)n);
        total += (size_t)n;
    }
    if (timed_out) kill(-pid, SIGKILL);
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    char tail[128];
    if (timed_out)
    {
        snprintf(tail, sizeof tail, "\n[killed after %d s]\n", TOOL_EXEC_SECS);
    }
    else if (WIFEXITED(status))
    {
        snprintf(tail, sizeof tail, "\n[exit %d]\n", WEXITSTATUS(status));
    }
    else 
    {
        snprintf(tail, sizeof tail, "\n[signal %d]\n", WTERMSIG(status));
    }
    if (total == 0) tbuf_cat(out, "[no output]");
    return tbuf_cat(out, tail);
}

int tools_dispatch(const char *line, tbuf *out)
{
    while (*line == ' ' || *line == '\t') line++;
    struct { const char *name; int (*fn)(const char *, tbuf *); } tab[] =
    {
        { "read_file",  t_read_file  },
        { "write_file", t_write_file },
        { "exec",       t_exec       },
    };

    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
    {
        size_t n = strlen(tab[i].name);
        if (strncmp(line, tab[i].name, n) == 0 &&
                (line[n] == ' ' || line[n] == '\n' || line[n] == '\0'))
        {
            const char *args = line + n;
            while (*args == ' ') args++;
            return tab[i].fn(args, out);
        }
    }
    return -1;
}

const char *tools_prompt(void)
{
    return
    "You have tools. To use one, emit a line starting with TOOL: and then stop.\n"
    "End every tool call with a line containing only END.\n"
    "The result is returned to you, then you continue.\n"
    "\n"
    "TOOL: read_file <path>\n"
    "END\n"
    "\n"
    "TOOL: exec <shell command>\n"
    "END\n"
    "\n"
    "TOOL: write_file <path>\n"
    "<file content on the following lines>\n"
    "END\n"
    "\n"
    "The line after the path is the FIRST LINE OF THE FILE. For a Python\n"
    "script that is code — an import or a statement. Never a title,\n"
    "filename, description or heading. Write the complete file, ready to\n"
    "run, including every import. No code fences.\n"
    "Emit one tool call at a time. Do not guess file contents: read them.\n";
}
