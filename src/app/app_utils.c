#include "app_utils.h"

void *app_memset(void *dst, int value, size_t size)
{
    unsigned char *d = (unsigned char *)dst;
    unsigned char v = (unsigned char)value;

    while(size > 0u)
    {
        *d = v;
        d++;
        size--;
    }

    return dst;
}

void *app_memcpy(void *dst, const void *src, size_t size)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    while(size > 0u)
    {
        *d = *s;
        d++;
        s++;
        size--;
    }

    return dst;
}

void *app_memmove(void *dst, const void *src, size_t size)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;

    if((d == s) || (size == 0u))
    {
        return dst;
    }

    if(d < s)
    {
        while(size > 0u)
        {
            *d = *s;
            d++;
            s++;
            size--;
        }
    }
    else
    {
        d += size;
        s += size;

        while(size > 0u)
        {
            d--;
            s--;
            *d = *s;
            size--;
        }
    }

    return dst;
}

int app_memcmp(const void *s1, const void *s2, size_t size)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;

    while(size > 0u)
    {
        if(*p1 != *p2)
        {
            return (int)*p1 - (int)*p2;
        }

        p1++;
        p2++;
        size--;
    }

    return 0;
}

void *app_memchr(const void *s, int value, size_t size)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char v = (unsigned char)value;

    while(size > 0u)
    {
        if(*p == v)
        {
            return (void *)p;
        }

        p++;
        size--;
    }

    return (void *)0;
}
