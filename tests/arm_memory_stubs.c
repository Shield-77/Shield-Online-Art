/* Minimal libc stubs for isolated ARM/Unicorn tests only, never linked into CIA. */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t count) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    for (size_t i = 0; i < count; i++) out[i] = in[i];
    return dst;
}

void *memset(void *dst, int value, size_t count) {
    unsigned char *out = dst;
    for (size_t i = 0; i < count; i++) out[i] = (unsigned char)value;
    return dst;
}

size_t strlen(const char *text) {
    size_t n = 0;
    while (text[n]) n++;
    return n;
}
size_t strnlen(const char *text, size_t limit) {
    size_t n = 0;
    while (n < limit && text[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int memcmp(const void *a, const void *b, size_t count) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < count; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

void *memmove(void *dst, const void *src, size_t count) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    if (out < in) for (size_t i = 0; i < count; i++) out[i] = in[i];
    else while (count) { count--; out[count] = in[count]; }
    return dst;
}

char *strchr(const char *s, int c) {
    do { if (*s == (char)c) return (char *)s; } while (*s++);
    return NULL;
}

int abs(int n) { return n < 0 ? -n : n; }
long labs(long n) { return n < 0 ? -n : n; }
