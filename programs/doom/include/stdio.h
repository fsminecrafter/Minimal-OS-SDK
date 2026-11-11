/* Doom for Minimal-OS: the parts of <stdio.h> the engine needs, on top of
 * the SDK's stdio.h (libc/libc.c has the code). */
#ifndef DOOM_LIBC_STDIO_H
#define DOOM_LIBC_STDIO_H
#include_next <stdio.h>
#include <stdarg.h>
#include <stddef.h>

typedef struct doom_file FILE;
extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define BUFSIZ 1024
#define FILENAME_MAX 256

FILE* fopen(const char* path, const char* mode);
int fclose(FILE* f);
size_t fread(void* buf, size_t size, size_t count, FILE* f);
size_t fwrite(const void* buf, size_t size, size_t count, FILE* f);
int fseek(FILE* f, long offset, int whence);
long ftell(FILE* f);
int fflush(FILE* f);
int feof(FILE* f);
int ferror(FILE* f);
int fgetc(FILE* f);
char* fgets(char* s, int n, FILE* f);
int fputc(int c, FILE* f);
int fputs(const char* s, FILE* f);
int puts(const char* s);
int putchar(int c);
int remove(const char* path);
int rename(const char* from, const char* to);

int vsnprintf(char* out, size_t size, const char* fmt, va_list ap);
int snprintf(char* out, size_t size, const char* fmt, ...);
int vsprintf(char* out, const char* fmt, va_list ap);
int sprintf(char* out, const char* fmt, ...);
int vfprintf(FILE* f, const char* fmt, va_list ap);
int fprintf(FILE* f, const char* fmt, ...);
int sscanf(const char* s, const char* fmt, ...);
int fscanf(FILE* f, const char* fmt, ...);

/* The SDK's printf knows few conversions; the engine's messages use more. */
int doom_printf(const char* fmt, ...);
int doom_vprintf(const char* fmt, va_list ap);
#define printf doom_printf
#define vprintf doom_vprintf

#define getc fgetc
#define putc fputc
#endif
