#ifndef NGCD_TARGET_STDLIB_H
#define NGCD_TARGET_STDLIB_H

#include <stddef.h>

extern void *malloc(size_t size);
extern void *calloc(size_t count, size_t size);
extern void *realloc(void *memory, size_t size);
extern void free(void *memory);
extern int posix_memalign(void **memory, size_t alignment, size_t size);
extern long strtol(const char *text, char **end, int base);
extern long long strtoll(const char *text, char **end, int base);
extern double strtod(const char *text, char **end);
extern int system(const char *command);
extern char *getenv(const char *name);
extern int setenv(const char *name, const char *value, int overwrite);
extern int unsetenv(const char *name);
extern void qsort(void *base, size_t count, size_t size,
                  int (*compare)(const void *, const void *));

#endif
