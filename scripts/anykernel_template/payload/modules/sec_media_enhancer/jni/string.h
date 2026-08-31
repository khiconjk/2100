#ifndef _CUSTOM_STRING_H
#define _CUSTOM_STRING_H
#include "stddef.h"

#ifdef __cplusplus
extern "C" {
#endif

int strcmp(const char *s1, const char *s2);
size_t strlen(const char *s);
char *strstr(const char *haystack, const char *needle);
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);

#ifdef __cplusplus
}
#endif

#endif
