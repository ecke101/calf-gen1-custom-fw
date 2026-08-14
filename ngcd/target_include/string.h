#ifndef NGCD_TARGET_STRING_H
#define NGCD_TARGET_STRING_H

#include <stddef.h>

extern void *memchr(const void *memory, int character, size_t length);
extern int memcmp(const void *left, const void *right, size_t length);
extern void *memcpy(void *destination, const void *source, size_t length);
extern void *memmove(void *destination, const void *source, size_t length);
extern void *memset(void *destination, int value, size_t length);
extern char *strchr(const char *text, int character);
extern int strcmp(const char *left, const char *right);
extern size_t strcspn(const char *text, const char *reject);
extern size_t strlen(const char *text);
extern int strncmp(const char *left, const char *right, size_t length);
extern char *strstr(const char *text, const char *needle);

#endif
