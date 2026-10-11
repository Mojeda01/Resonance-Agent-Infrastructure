#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llama.h"
#include "server.h"

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s -d MODEL_DIR [-S SOCKET] [-c CTX] [-t THREADS]\n"
        "\n"
        "  -d DIR     directory scanned for .gguf models\n"
        "  -S PATH    unix socket path (default /run/agentinfra.sock)\n"
        "  -c CTX     context size (default 4096)\n"
        "  -t N       threads (default 4)\n",
        prog);
}

int main (int argc, char **argv)
{
    const char *model_dir = NULL;
    const char *sock_path = "/run/agentinfra.sock";
    int n_ctx     = 4096;
    int n_threads = 4;

    for (int i = 1; i < argc; i++)
    {
        if      (!strcmp(argv[i], "-d") && i + 1 < argc) model_dir = argv[++i];
        else if (!strcmp(argv[i], "-S") && i + 1 < argc) sock_path = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) n_ctx     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) n_threads = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    if (!model_dir)
    {
        fprintf(stderr, "error: -d MODEL_DIR is required\n");
        usage(argv[0]);
        return 1;
    }

    /* a client disconnecting mid-reply must not kill the process */
    signal(SIGPIPE, SIG_IGN);

    llama_backend_init();

    int rc = server_run(sock_path, model_dir, n_ctx, n_threads);

    llama_backend_free();
    return rc == 0 ? 0 : 1;
}
