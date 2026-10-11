#ifndef AGENT_TOOLS_H
#define AGENT_TOOLS_H

#include <stddef.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} tbuf;

void tbuf_init(tbuf *b);
void tbuf_free(tbuf *b);
int tbuf_append(tbuf *b, const char *p, size_t n);
int tbuf_cat(tbuf *b, const char *s);

#define TOOL_MAX_OUT   (64 * 1024)
#define TOOL_EXEC_SECS 30
#define TOOL_MAX_HOPS 6

int tools_dispatch(const char *line, tbuf *out);
const char *tools_prompt(void);

#endif /* AGENT_TOOLS_H */
