#ifndef _CUSTOM_STDDEF_H
#define _CUSTOM_STDDEF_H

typedef unsigned long size_t;
typedef long ptrdiff_t;

#ifndef NULL
#ifdef __cplusplus
#define NULL nullptr
#else
#define NULL ((void*)0)
#endif
#endif

#endif
