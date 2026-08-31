#ifndef _CUSTOM_UNISTD_H
#define _CUSTOM_UNISTD_H
#include "stddef.h"

typedef long ssize_t;

#ifdef __cplusplus
extern "C" {
#endif

ssize_t read(int fd, void *buf, size_t count);
int close(int fd);

#ifdef __cplusplus
}
#endif

#endif
