/* string.h -- zrt (freestanding ARM runtime for fpgagpud): the few string functions used. */
#ifndef ZRT_STRING_H
#define ZRT_STRING_H
#include <stddef.h>

void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
void  *memset(void *d, int c, size_t n);
int    memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strchr(const char *s, int c);

#ifndef ZRT_IMPL
/* -ffreestanding turns off the builtin versions; get them back (inline small fixed copies). */
#define memcpy(d, s, n)  __builtin_memcpy((d), (s), (n))
#define memmove(d, s, n) __builtin_memmove((d), (s), (n))
#define memset(d, c, n)  __builtin_memset((d), (c), (n))
#define memcmp(a, b, n)  __builtin_memcmp((a), (b), (n))
#define strlen(s)        __builtin_strlen((s))
#endif

#endif
