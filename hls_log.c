#include "hls_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <pthread.h>

static LOG_LEVEL_e gLogLevel = LOG_LEVEL_TRACE;

void HLS_LOG_SetLevel(LOG_LEVEL_e eLevel)
{
    gLogLevel = eLevel;
}

void HLS_LOG_Print(int priority, const char* color, const char *fmt, ...)
{
    va_list ap;

    if(priority > gLogLevel)
        return;

    if(color)
        fputs(color, stdout);

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);

    if(color)
        fputs(ANSI_COLOR_RESET, stdout);

    fflush(stdout);
}

#define ISPRINTABLE(c)  ((c)>=32 && (c)<=126)

void HLS_LOG_Dump(int priority, const void* ptr, int size)
{
    char buffer[1024];
    int ii, n;
    int offset = 0;
    const unsigned char* data = (const unsigned char*)ptr;

    if(priority > gLogLevel)
        return;

    while(offset < size)
    {
        char* p = buffer;
        int remain = size - offset;

        n = sprintf(p, "0x%04x  ", offset);
        p += n;

        if(remain > 16)
            remain = 16;

        for(ii = 0; ii < 16; ii++)
        {
            if(ii == 8)
                strcpy(p++, " ");

            if(offset + ii < size)
               n = sprintf(p, "%02x ", data[offset + ii]);
            else
               n = sprintf(p, "   ");

            p += n;
        }

        strcpy(p++, " ");
        for(ii = 0; ii < remain; ii++)
        {
            if(ISPRINTABLE(data[offset + ii]))
                sprintf(p++, "%c", data[offset + ii]);
            else 
                strcpy(p++, ".");
        }
        strcpy(p++, "\n");

        offset += 16;

        fputs(buffer, stdout);
    }

    fflush(stdout);
}
