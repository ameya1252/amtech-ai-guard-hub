#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_LINE_MAX 512

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

static int parse_time_value(const char *key, const char *value, int *hour, int *minute)
{
    char *end = NULL;
    long parsed_hour;
    long parsed_minute;
    const char *separator;

    if (hour == NULL || minute == NULL)
    {
        return -1;
    }

    errno = 0;
    parsed_hour = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != ':')
    {
        printf("Config: ignoring invalid time for %s: %s\n", key, value);
        return -1;
    }

    separator = end + 1;
    errno = 0;
    parsed_minute = strtol(separator, &end, 10);
    if (errno != 0 || end == separator || *trim_whitespace(end) != '\0' ||
        parsed_hour < 0 || parsed_hour > 23 || parsed_minute < 0 || parsed_minute > 59)
    {
        printf("Config: ignoring invalid time for %s: %s\n", key, value);
        return -1;
    }

    *hour = (int)parsed_hour;
    *minute = (int)parsed_minute;
    return 0;
}

static void set_alert_contact(amtech_config_t *config, int index, const char *value)
{
    if (config == NULL || index < 0 || index >= AMTECH_ALERT_CONTACT_COUNT)
    {
        return;
    }

    snprintf(config->alert_contacts[index],
             sizeof(config->alert_contacts[index]),
             "%s",
             value);
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
    config->schedule_arm_hour = AMTECH_DEFAULT_SCHEDULE_ARM_HOUR;
    config->schedule_arm_minute = AMTECH_DEFAULT_SCHEDULE_ARM_MINUTE;
    config->schedule_disarm_hour = AMTECH_DEFAULT_SCHEDULE_DISARM_HOUR;
    config->schedule_disarm_minute = AMTECH_DEFAULT_SCHEDULE_DISARM_MINUTE;
    snprintf(config->modem_device, sizeof(config->modem_device), "%s", AMTECH_DEFAULT_MODEM_DEVICE);
    set_alert_contact(config, 0, AMTECH_DEFAULT_ALERT_CONTACT_1);
    set_alert_contact(config, 1, AMTECH_DEFAULT_ALERT_CONTACT_2);
    set_alert_contact(config, 2, AMTECH_DEFAULT_ALERT_CONTACT_3);
    config->camera_enabled = 0;
    config->camera_rtsp_url[0] = '\0';
    config->camera2_enabled = 0;
    config->camera2_rtsp_url[0] = '\0';
    snprintf(config->backend_base_url, sizeof(config->backend_base_url), "%s", AMTECH_DEFAULT_BACKEND_BASE_URL);
    config->device_config_token[0] = '\0';
    snprintf(config->shop_id, sizeof(config->shop_id), "%s", AMTECH_DEFAULT_SHOP_ID);
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
            printf("Config: %s not found, using defaults SHUTTER_COUNT=1 PANIC_ENABLED=1 SMOKE_ENABLED=0 SCHEDULE_ARM=%02d:%02d SCHEDULE_DISARM=%02d:%02d MODEM_DEVICE=%s ALERT_CONTACT_1=%s ALERT_CONTACT_2=%s ALERT_CONTACT_3=%s CAMERA_ENABLED=0 CAMERA_RTSP_URL=(disabled) CAMERA2_ENABLED=0 CAMERA2_RTSP_URL=(disabled) BACKEND_BASE_URL=%s DEVICE_CONFIG_TOKEN=(unset) SHOP_ID=%s\n",
                   path,
                   config->schedule_arm_hour,
                   config->schedule_arm_minute,
                   config->schedule_disarm_hour,
                   config->schedule_disarm_minute,
                   config->modem_device,
                   config->alert_contacts[0],
                   config->alert_contacts[1],
                   config->alert_contacts[2],
                   config->backend_base_url,
                   config->shop_id);
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
        else if (strcmp(key, "SCHEDULE_ARM") == 0)
        {
            parse_time_value(key, value, &config->schedule_arm_hour, &config->schedule_arm_minute);
        }
        else if (strcmp(key, "SCHEDULE_DISARM") == 0)
        {
            parse_time_value(key, value, &config->schedule_disarm_hour, &config->schedule_disarm_minute);
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
        else if (strcmp(key, "ALERT_CONTACT_1") == 0 ||
                 strcmp(key, "ALERT_CONTACT_2") == 0 ||
                 strcmp(key, "ALERT_CONTACT_3") == 0)
        {
            int contact_index = key[strlen("ALERT_CONTACT_")] - '1';

            if (value[0] == '\0' || strlen(value) >= AMTECH_ALERT_CONTACT_NUMBER_MAX)
            {
                printf("Config: invalid %s, keeping %s\n",
                       key,
                       config->alert_contacts[contact_index]);
                continue;
            }

            set_alert_contact(config, contact_index, value);
        }
        else if (strcmp(key, "ALERT_CONTACT_NUMBER") == 0)
        {
            printf("Config: ALERT_CONTACT_NUMBER is deprecated; use ALERT_CONTACT_1/2/3\n");
        }
        else if (strcmp(key, "CAMERA_ENABLED") == 0)
        {
            if (parse_int_value(key, value, &parsed_value) != 0)
            {
                continue;
            }

            config->camera_enabled = parsed_value ? 1 : 0;
        }
        else if (strcmp(key, "CAMERA_RTSP_URL") == 0)
        {
            if (strlen(value) >= sizeof(config->camera_rtsp_url))
            {
                printf("Config: CAMERA_RTSP_URL too long, keeping current value\n");
                continue;
            }

            snprintf(config->camera_rtsp_url, sizeof(config->camera_rtsp_url), "%s", value);
        }
        else if (strcmp(key, "CAMERA2_ENABLED") == 0)
        {
            if (parse_int_value(key, value, &parsed_value) != 0)
            {
                continue;
            }

            config->camera2_enabled = parsed_value ? 1 : 0;
        }
        else if (strcmp(key, "CAMERA2_RTSP_URL") == 0)
        {
            if (strlen(value) >= sizeof(config->camera2_rtsp_url))
            {
                printf("Config: CAMERA2_RTSP_URL too long, keeping current value\n");
                continue;
            }

            snprintf(config->camera2_rtsp_url, sizeof(config->camera2_rtsp_url), "%s", value);
        }
        else if (strcmp(key, "BACKEND_BASE_URL") == 0)
        {
            if (value[0] == '\0' || strlen(value) >= sizeof(config->backend_base_url))
            {
                printf("Config: invalid BACKEND_BASE_URL, keeping %s\n", config->backend_base_url);
                continue;
            }

            snprintf(config->backend_base_url, sizeof(config->backend_base_url), "%s", value);
        }
        else if (strcmp(key, "DEVICE_CONFIG_TOKEN") == 0)
        {
            if (strlen(value) >= sizeof(config->device_config_token))
            {
                printf("Config: DEVICE_CONFIG_TOKEN too long, keeping current value\n");
                continue;
            }

            snprintf(config->device_config_token, sizeof(config->device_config_token), "%s", value);
        }
        else if (strcmp(key, "SHOP_ID") == 0)
        {
            if (value[0] == '\0' || strlen(value) >= sizeof(config->shop_id))
            {
                printf("Config: invalid SHOP_ID, keeping %s\n", config->shop_id);
                continue;
            }

            snprintf(config->shop_id, sizeof(config->shop_id), "%s", value);
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

    printf("Config: SHUTTER_COUNT=%d PANIC_ENABLED=%d SMOKE_ENABLED=%d SCHEDULE_ARM=%02d:%02d SCHEDULE_DISARM=%02d:%02d MODEM_DEVICE=%s ALERT_CONTACT_1=%s ALERT_CONTACT_2=%s ALERT_CONTACT_3=%s CAMERA_ENABLED=%d CAMERA_RTSP_URL=%s CAMERA2_ENABLED=%d CAMERA2_RTSP_URL=%s BACKEND_BASE_URL=%s DEVICE_CONFIG_TOKEN=%s SHOP_ID=%s\n",
           config->shutter_count,
           config->panic_enabled,
           config->smoke_enabled,
           config->schedule_arm_hour,
           config->schedule_arm_minute,
           config->schedule_disarm_hour,
           config->schedule_disarm_minute,
           config->modem_device,
           config->alert_contacts[0],
           config->alert_contacts[1],
           config->alert_contacts[2],
           config->camera_enabled,
           config->camera_rtsp_url[0] != '\0' ? config->camera_rtsp_url : "(disabled)",
           config->camera2_enabled,
           config->camera2_rtsp_url[0] != '\0' ? config->camera2_rtsp_url : "(disabled)",
           config->backend_base_url,
           config->device_config_token[0] != '\0' ? "(set)" : "(unset)",
           config->shop_id);
    return 0;
}
