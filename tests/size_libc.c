/* Size-only freestanding link support, not a replacement platform C library.
 * Compiled WITHOUT LTO so its cost can be reported separately from the module. */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)value;
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a;
    const unsigned char *y = b;
    while (n--) {
        if (*x != *y) {
            return *x - *y;
        }
        ++x;
        ++y;
    }
    return 0;
}
