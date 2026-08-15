#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 128

static char *trim_whitespace(char *text)
{
    char *end;

    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
    {
        text++;
    }

    if (*text == '\0')
    {
        return text;
    }

    end = text + strlen(text) - 1;
    while (end > text && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
    {
        *end = '\0';
        end--;
    }

    return text;
}

static int parse_int_value(const char *key, const char *value, int *out)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *trim_whitespace(end) != '\0')
    {
        printf("Config: ignoring invalid integer for %s: %s\n", key, value);
        return -1;
    }

    *out = (int)parsed;
    return 0;
}

void amtech_config_set_defaults(amtech_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    config->shutter_count = 1;
    config->panic_enabled = 1;
    config->smoke_enabled = 0;
    snprintf(config->modem_device, sizeof(config->modem_device), "%s", AMTECH_DEFAULT_MODEM_DEVICE);
}

int amtech_config_load(const char *path, amtech_config_t *config)
{
    char line[CONFIG_LINE_MAX];
    FILE *fp;

    if (config == NULL)
    {
        return -1;
    }

    amtech_config_set_defaults(config);

    fp = fopen(path, "r");
    if (fp == NULL)
    {
        if (errno == ENOENT)
        {
            printf("Config: %s not found, using defaults SHUTTER_COUNT=1 PANIC_ENABLED=1 SMOKE_ENABLED=0 MODEM_DEVICE=%s\n",
                   path,
                   config->modem_device);
            return 0;
        }

        printf("Config: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL)
    {
        char *key;
        char *value;
        char *separator;
        int parsed_value;

        key = trim_whitespace(line);
        if (key[0] == '\0' || key[0] == '#')
        {
            continue;
        }

        separator = strchr(key, '=');
        if (separator == NULL)
        {
            printf("Config: ignoring line without '=': %s\n", key);
            continue;
        }

        *separator = '\0';
        value = trim_whitespace(separator + 1);
        key = trim_whitespace(key);

        if (strcmp(key, "SHUTTER_COUNT") == 0)
        {
            if (parse_int_value(key, value, &parsed_value) != 0)
            {
                continue;
            }

            if (parsed_value < 1 || parsed_value > 2)
            {
                printf("Config: SHUTTER_COUNT must be 1 or 2, keeping %d\n", config->shutter_count);
                continue;
            }
            config->shutter_count = parsed_value;
        }
        else if (strcmp(key, "PANIC_ENABLED") == 0)
        {
            if (parse_int_value(key, value, &parsed_value) != 0)
            {
                continue;
            }

            config->panic_enabled = parsed_value ? 1 : 0;
        }
        else if (strcmp(key, "SMOKE_ENABLED") == 0)
        {
            if (parse_int_value(key, value, &parsed_value) != 0)
            {
                continue;
            }

            config->smoke_enabled = parsed_value ? 1 : 0;
        }
        else if (strcmp(key, "MODEM_DEVICE") == 0)
        {
            if (value[0] == '\0' || strlen(value) >= sizeof(config->modem_device))
            {
                printf("Config: invalid MODEM_DEVICE, keeping %s\n", config->modem_device);
                continue;
            }

            snprintf(config->modem_device, sizeof(config->modem_device), "%s", value);
        }
        else
        {
            printf("Config: ignoring unknown key %s\n", key);
        }
    }

    if (ferror(fp))
    {
        printf("Config: failed while reading %s\n", path);
        fclose(fp);
        return -1;
    }

    fclose(fp);

    printf("Config: SHUTTER_COUNT=%d PANIC_ENABLED=%d SMOKE_ENABLED=%d MODEM_DEVICE=%s\n",
           config->shutter_count,
           config->panic_enabled,
           config->smoke_enabled,
           config->modem_device);
    return 0;
}
