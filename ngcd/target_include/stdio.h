#ifndef NGCD_TARGET_STDIO_H
#define NGCD_TARGET_STDIO_H

#include <stdarg.h>
#include <stddef.h>

typedef struct ngcd_target_file FILE;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

extern FILE *stderr;
extern int fclose(FILE *stream);
extern int ferror(FILE *stream);
extern char *fgets(char *output, int size, FILE *stream);
extern int fgetc(FILE *stream);
extern int fputc(int character, FILE *stream);
extern int fflush(FILE *stream);
extern int fileno(FILE *stream);
extern FILE *fopen(const char *path, const char *mode);
extern int fseek(FILE *stream, long offset, int origin);
extern long ftell(FILE *stream);
extern int fprintf(FILE *stream, const char *format, ...);
extern size_t fread(void *output, size_t size, size_t count, FILE *stream);
extern size_t fwrite(const void *input, size_t size, size_t count, FILE *stream);
extern void perror(const char *message);
extern int printf(const char *format, ...);
extern int puts(const char *text);
extern int rename(const char *old_path, const char *new_path);
extern int snprintf(char *output, size_t size, const char *format, ...);
extern int sscanf(const char *input, const char *format, ...);
extern int vsnprintf(char *output, size_t size, const char *format,
                     va_list arguments);

#endif
