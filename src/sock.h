#ifndef SOCK_H
#define SOCK_H

#include <stdint.h>

#define MSG_LIST 1
#define MSG_LOAD 2
#define MSG_PROMPT 3
#define MSG_CANCEL 4 
#define MSG_RESET 5
#define MSG_TAIL 6
#define MSG_DROP 7

#define MSG_MODELS 16
#define MSG_STATUS 17
#define MSG_CHUNK 18
#define MSG_STATS 19
#define MSG_DONE 20
#define MSG_ERROR 21
#define MSG_FULL 22
#define MSG_TOOL_CALL 23
#define MSG_TOOL_RESULT 24
#define MSG_TOOL_ERROR 25

#define MSG_MAX (1u << 20)

int sock_listen(const char *path, int backlog);
int sock_accept(int lfd);
int msg_read(int fd, uint8_t *type, char *buf, uint32_t cap, uint32_t *len);
int msg_write(int fd, uint8_t type, const void *buf, uint32_t len);
int msg_writes(int fd, uint8_t type, const char *s);

#endif
