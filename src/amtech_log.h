#ifndef AMTECH_LOG_H
#define AMTECH_LOG_H

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static inline void amtech_logf(const char *component, const char *fmt, ...)
{
    char timestamp[32];
    time_t now;
    struct tm tm_now;
    va_list args;

    now = time(NULL);
    if (localtime_r(&now, &tm_now) != NULL)
    {
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);
    }
    else
    {
        snprintf(timestamp, sizeof(timestamp), "time-unavailable");
    }

    printf("[%s] %s: ", timestamp, component != NULL ? component : "AMTECH");
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

#endif
