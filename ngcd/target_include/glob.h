#ifndef CALF_TARGET_GLOB_H
#define CALF_TARGET_GLOB_H

#include <stddef.h>

typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
    int gl_flags;
    void (*gl_closedir)(void *);
    void *(*gl_readdir)(void *);
    void *(*gl_opendir)(const char *);
    int (*gl_lstat)(const char *, void *);
    int (*gl_stat)(const char *, void *);
} glob_t;

#define GLOB_NOSORT 4

extern int glob(const char *pattern, int flags,
                int (*error_function)(const char *, int), glob_t *matches);
extern void globfree(glob_t *matches);

#endif
