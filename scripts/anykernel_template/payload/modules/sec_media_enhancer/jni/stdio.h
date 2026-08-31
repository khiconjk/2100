#ifndef _CUSTOM_STDIO_H
#define _CUSTOM_STDIO_H
#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

int sscanf(const char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
