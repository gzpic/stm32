#ifndef SPI_PARSE_NUMBER_H
#define SPI_PARSE_NUMBER_H
#include <errno.h>
#include <stdlib.h>

/* Decimal by default, hexadecimal only with an explicit 0x prefix. */
static inline int number(const char *s, unsigned long max, unsigned long *value)
{
    char *end;
    int base;
    if (!s || !value || *s < '0' || *s > '9') return 0;
    base = s[0] == '0' && (s[1] == 'x' || s[1] == 'X') ? 16 : 10;
    errno = 0;
    *value = strtoul(s, &end, base);
    return end != s && !errno && !*end && *value <= max;
}
#endif
