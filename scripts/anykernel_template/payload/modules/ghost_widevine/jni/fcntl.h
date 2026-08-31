#ifndef _CUSTOM_FCNTL_H
#define _CUSTOM_FCNTL_H

#define O_RDONLY 0

#ifdef __cplusplus
extern "C" {
#endif

int open(const char *pathname, int flags, ...);

#ifdef __cplusplus
}
#endif

#endif
