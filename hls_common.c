#include "hls_common.h"

#include <inttypes.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

char* ltrim(char *s)
{
    if(!s) return s;

    while(isspace(*s)) s++;

    return s;
}

char* rtrim(char* s)
{
    char* back;

    if(!s) return s;

    back = s + strlen(s);

    while((s <= --back) && isspace(*back));
    *(back + 1) = '\0';

    return s;
}

int64_t get_tick(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
