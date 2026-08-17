#include "config_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_SYNC_LINE_MAX 512
#define CONFIG_SYNC_RESPONSE_MAX 8192
#define CONFIG_SYNC_COMMAND_MAX 8192
#define CONFIG_SYNC_QUOTED_URL_MAX 2048
#define CONFIG_SYNC_QUOTED_TOKEN_MAX 1024

static int parse_json_int_field(const char *json, const char *field, int *out)
{
    char pattern[64];
    const char *cursor;
    char *end = NULL;
    long parsed;

    if (json == NULL || field == NULL || out == NULL)
    {
        return -1;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    cursor = strstr(json, pattern);
    if (cursor == NULL)
    {
        return -1;
    }

    cursor = strchr(cursor + strlen(pattern), ':');
    if (cursor == NULL)
    {
        return -1;
    }
    cursor++;

    errno = 0;
    parsed = strtol(cursor, &end, 10);
    if (errno != 0 || end == cursor)
    {
        return -1;
    }

    *out = (int)parsed;
    return 0;
}

static int parse_json_string_field_after(const char *start,
                                         const char *field,
                                         char *out,
                                         size_t out_size)
{
    char pattern[64];
    const char *cursor;
    const char *value_start;
    const char *value_end;
    size_t length;

    if (start == NULL || field == NULL || out == NULL || out_size == 0)
    {
        return -1;
    }
    out[0] = '\0';

    snprintf(pattern, sizeof(pattern), "\"%s\"", field);
    cursor = strstr(start, pattern);
    if (cursor == NULL)
    {
        return -1;
    }

    cursor = strchr(cursor + strlen(pattern), ':');
    if (cursor == NULL)
    {
        return -1;
    }
    cursor++;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
    {
        cursor++;
    }
    if (*cursor != '"')
    {
        return -1;
    }
    value_start = cursor + 1;
    value_end = strchr(value_start, '"');
    if (value_end == NULL)
    {
        return -1;
    }

    length = (size_t)(value_end - value_start);
    if (length >= out_size)
    {
        length = out_size - 1;
    }

    memcpy(out, value_start, length);
    out[length] = '\0';
    return 0;
}

static int parse_contact_phone_for_slot(const char *json, int slot, char *out, size_t out_size)
{
    const char *cursor;

    if (json == NULL || out == NULL || out_size == 0)
    {
        return -1;
    }
    out[0] = '\0';

    cursor = json;
    while ((cursor = strstr(cursor, "\"slot\"")) != NULL)
    {
        const char *colon = strchr(cursor, ':');
        const char *object_start = cursor;
        const char *object_end;
        char *end = NULL;
        char contact_object[CONFIG_SYNC_LINE_MAX];
        long parsed_slot;
        size_t length;

        if (colon == NULL)
        {
            return -1;
        }

        parsed_slot = strtol(colon + 1, &end, 10);
        if (end == colon + 1)
        {
            cursor += 6;
            continue;
        }

        while (object_start > json && *object_start != '{')
        {
            object_start--;
        }
        object_end = strchr(cursor, '}');
        if (*object_start != '{' || object_end == NULL)
        {
            cursor += 6;
            continue;
        }

        if ((int)parsed_slot == slot)
        {
            length = (size_t)(object_end - object_start + 1);
            if (length >= sizeof(contact_object))
            {
                length = sizeof(contact_object) - 1;
            }
            memcpy(contact_object, object_start, length);
            contact_object[length] = '\0';
            return parse_json_string_field_after(contact_object, "phone", out, out_size);
        }

        cursor += 6;
    }
    return -1;
}

static int valid_time_parts(int hour, int minute)
{
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

int amtech_config_sync_parse_json(const char *json, amtech_config_t *config)
{
    int arm_hour;
    int arm_minute;
    int disarm_hour;
    int disarm_minute;
    char phone[AMTECH_ALERT_CONTACT_NUMBER_MAX];
    int i;

    if (json == NULL || config == NULL)
    {
        return -1;
    }

    if (parse_json_int_field(json, "arm_hour", &arm_hour) != 0 ||
        parse_json_int_field(json, "arm_minute", &arm_minute) != 0 ||
        parse_json_int_field(json, "disarm_hour", &disarm_hour) != 0 ||
        parse_json_int_field(json, "disarm_minute", &disarm_minute) != 0 ||
        !valid_time_parts(arm_hour, arm_minute) ||
        !valid_time_parts(disarm_hour, disarm_minute))
    {
        printf("Config sync: invalid or missing schedule in device-config response\n");
        return -1;
    }

    config->schedule_arm_hour = arm_hour;
    config->schedule_arm_minute = arm_minute;
    config->schedule_disarm_hour = disarm_hour;
    config->schedule_disarm_minute = disarm_minute;

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        if (parse_contact_phone_for_slot(json, i + 1, phone, sizeof(phone)) == 0 &&
            phone[0] != '\0')
        {
            snprintf(config->alert_contacts[i],
                     sizeof(config->alert_contacts[i]),
                     "%s",
                     phone);
        }
    }

    return 0;
}

static int key_is_synced(const char *key)
{
    return strcmp(key, "SCHEDULE_ARM") == 0 ||
           strcmp(key, "SCHEDULE_DISARM") == 0 ||
           strcmp(key, "ALERT_CONTACT_1") == 0 ||
           strcmp(key, "ALERT_CONTACT_2") == 0 ||
           strcmp(key, "ALERT_CONTACT_3") == 0;
}

static void synced_value_for_key(const char *key, const amtech_config_t *config, char *value, size_t value_size)
{
    if (strcmp(key, "SCHEDULE_ARM") == 0)
    {
        snprintf(value, value_size, "%02d:%02d", config->schedule_arm_hour, config->schedule_arm_minute);
    }
    else if (strcmp(key, "SCHEDULE_DISARM") == 0)
    {
        snprintf(value, value_size, "%02d:%02d", config->schedule_disarm_hour, config->schedule_disarm_minute);
    }
    else if (strcmp(key, "ALERT_CONTACT_1") == 0)
    {
        snprintf(value, value_size, "%s", config->alert_contacts[0]);
    }
    else if (strcmp(key, "ALERT_CONTACT_2") == 0)
    {
        snprintf(value, value_size, "%s", config->alert_contacts[1]);
    }
    else if (strcmp(key, "ALERT_CONTACT_3") == 0)
    {
        snprintf(value, value_size, "%s", config->alert_contacts[2]);
    }
    else
    {
        value[0] = '\0';
    }
}

static char *trim_leading(char *text)
{
    while (*text == ' ' || *text == '\t')
    {
        text++;
    }
    return text;
}

static void extract_key_from_line(const char *line, char *key, size_t key_size)
{
    char local[CONFIG_SYNC_LINE_MAX];
    char *start;
    char *separator;
    size_t length;

    key[0] = '\0';
    snprintf(local, sizeof(local), "%s", line);
    start = trim_leading(local);
    if (*start == '\0' || *start == '\n' || *start == '\r' || *start == '#')
    {
        return;
    }

    separator = strchr(start, '=');
    if (separator == NULL)
    {
        return;
    }
    *separator = '\0';
    length = strlen(start);
    while (length > 0 && (start[length - 1] == ' ' || start[length - 1] == '\t'))
    {
        start[length - 1] = '\0';
        length--;
    }

    snprintf(key, key_size, "%s", start);
}

int amtech_config_sync_update_file(const char *path, const amtech_config_t *config)
{
    char tmp_path[256];
    char line[CONFIG_SYNC_LINE_MAX];
    int seen_arm = 0;
    int seen_disarm = 0;
    int seen_contacts[AMTECH_ALERT_CONTACT_COUNT] = {0, 0, 0};
    int changed = 0;
    FILE *in;
    FILE *out;
    int read_missing = 0;
    int i;

    if (path == NULL || path[0] == '\0' || config == NULL)
    {
        return -1;
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    in = fopen(path, "r");
    if (in == NULL)
    {
        if (errno == ENOENT)
        {
            read_missing = 1;
        }
        else
        {
            printf("Config sync: failed to open %s for reading: %s\n", path, strerror(errno));
            return -1;
        }
    }

    out = fopen(tmp_path, "w");
    if (out == NULL)
    {
        if (in != NULL)
        {
            fclose(in);
        }
        printf("Config sync: failed to open %s for writing: %s\n", tmp_path, strerror(errno));
        return -1;
    }

    while (in != NULL && fgets(line, sizeof(line), in) != NULL)
    {
        char key[64];
        char value[AMTECH_CAMERA_RTSP_URL_MAX];

        extract_key_from_line(line, key, sizeof(key));
        if (key_is_synced(key))
        {
            synced_value_for_key(key, config, value, sizeof(value));
            fprintf(out, "%s=%s\n", key, value);
            if (strstr(line, value) == NULL)
            {
                changed = 1;
            }

            if (strcmp(key, "SCHEDULE_ARM") == 0)
            {
                seen_arm = 1;
            }
            else if (strcmp(key, "SCHEDULE_DISARM") == 0)
            {
                seen_disarm = 1;
            }
            else if (strcmp(key, "ALERT_CONTACT_1") == 0)
            {
                seen_contacts[0] = 1;
            }
            else if (strcmp(key, "ALERT_CONTACT_2") == 0)
            {
                seen_contacts[1] = 1;
            }
            else if (strcmp(key, "ALERT_CONTACT_3") == 0)
            {
                seen_contacts[2] = 1;
            }
            continue;
        }

        fputs(line, out);
    }

    if (in != NULL)
    {
        if (ferror(in))
        {
            printf("Config sync: failed while reading %s\n", path);
            fclose(in);
            fclose(out);
            remove(tmp_path);
            return -1;
        }
        fclose(in);
    }

    if (!seen_arm)
    {
        fprintf(out, "SCHEDULE_ARM=%02d:%02d\n", config->schedule_arm_hour, config->schedule_arm_minute);
        changed = 1;
    }
    if (!seen_disarm)
    {
        fprintf(out, "SCHEDULE_DISARM=%02d:%02d\n", config->schedule_disarm_hour, config->schedule_disarm_minute);
        changed = 1;
    }
    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        if (!seen_contacts[i])
        {
            fprintf(out, "ALERT_CONTACT_%d=%s\n", i + 1, config->alert_contacts[i]);
            changed = 1;
        }
    }

    if (fclose(out) != 0)
    {
        printf("Config sync: failed to close %s: %s\n", tmp_path, strerror(errno));
        remove(tmp_path);
        return -1;
    }

    if (!changed && !read_missing)
    {
        remove(tmp_path);
        return 0;
    }

    if (rename(tmp_path, path) != 0)
    {
        printf("Config sync: failed to replace %s atomically: %s\n", path, strerror(errno));
        remove(tmp_path);
        return -1;
    }

    printf("Config sync: updated %s schedule/contact keys only\n", path);
    return 1;
}

#ifdef SIMULATE_NETWORK
static char simulated_response[CONFIG_SYNC_RESPONSE_MAX];

void amtech_config_sync_set_simulated_response(const char *json)
{
    snprintf(simulated_response, sizeof(simulated_response), "%s", json != NULL ? json : "");
}

static int fetch_device_config_json(const amtech_config_t *config,
                                    const char *shop_id,
                                    char *response,
                                    size_t response_size)
{
    (void)config;
    (void)shop_id;

    if (response == NULL || response_size == 0)
    {
        return -1;
    }
    if (simulated_response[0] == '\0')
    {
        return 0;
    }
    snprintf(response, response_size, "%s", simulated_response);
    return 1;
}
#else
static void shell_quote(const char *input, char *output, size_t output_size)
{
    size_t out = 0;
    size_t i;

    if (output_size == 0)
    {
        return;
    }
    output[out++] = '\'';
    for (i = 0; input != NULL && input[i] != '\0' && out + 5 < output_size; i++)
    {
        if (input[i] == '\'')
        {
            memcpy(output + out, "'\\''", 4);
            out += 4;
        }
        else
        {
            output[out++] = input[i];
        }
    }
    if (out + 1 < output_size)
    {
        output[out++] = '\'';
    }
    output[out] = '\0';
}

static void build_device_config_url(const amtech_config_t *config,
                                    const char *shop_id,
                                    char *url,
                                    size_t url_size)
{
    char base[AMTECH_BACKEND_URL_MAX];
    size_t length;

    snprintf(base, sizeof(base), "%s", config->backend_base_url);
    length = strlen(base);
    while (length > 0 && base[length - 1] == '/')
    {
        base[length - 1] = '\0';
        length--;
    }

    snprintf(url, url_size, "%s/shop/%s/device-config", base, shop_id);
}

static int fetch_device_config_json(const amtech_config_t *config,
                                    const char *shop_id,
                                    char *response,
                                    size_t response_size)
{
    char url[AMTECH_BACKEND_URL_MAX + AMTECH_SHOP_ID_MAX + 32];
    char quoted_url[CONFIG_SYNC_QUOTED_URL_MAX];
    char quoted_header[CONFIG_SYNC_QUOTED_TOKEN_MAX];
    char quoted_token[CONFIG_SYNC_QUOTED_TOKEN_MAX];
    char command[CONFIG_SYNC_COMMAND_MAX];
    FILE *pipe;
    size_t used = 0;
    int status;

    if (config == NULL || shop_id == NULL || response == NULL || response_size == 0)
    {
        return -1;
    }
    response[0] = '\0';

    build_device_config_url(config, shop_id, url, sizeof(url));
    shell_quote(url, quoted_url, sizeof(quoted_url));
    if (config->device_config_token[0] != '\0')
    {
        char header[AMTECH_DEVICE_CONFIG_TOKEN_MAX + 40];
        snprintf(header, sizeof(header), "X-AMTECH-DEVICE-CONFIG-TOKEN: %s", config->device_config_token);
        shell_quote(header, quoted_header, sizeof(quoted_header));
        shell_quote(config->device_config_token, quoted_token, sizeof(quoted_token));
        snprintf(command,
                 sizeof(command),
                 "if command -v curl >/dev/null 2>&1; then "
                 "curl --fail --silent --show-error --max-time 15 -H %s %s; "
                 "elif command -v python3 >/dev/null 2>&1; then "
                 "python3 -c 'import ssl,sys,urllib.request; token=sys.argv.__getitem__(1); url=sys.argv.__getitem__(2); req=urllib.request.Request(url, headers={\"X-AMTECH-DEVICE-CONFIG-TOKEN\": token}); ctx=ssl._create_unverified_context(); print(urllib.request.urlopen(req, timeout=15, context=ctx).read().decode(), end=\"\")' %s %s; "
                 "else echo 'Config sync: no HTTPS client available' >&2; exit 127; fi",
                 quoted_header,
                 quoted_url,
                 quoted_token,
                 quoted_url);
    }
    else
    {
        snprintf(command,
                 sizeof(command),
                 "if command -v curl >/dev/null 2>&1; then "
                 "curl --fail --silent --show-error --max-time 15 %s; "
                 "elif command -v python3 >/dev/null 2>&1; then "
                 "python3 -c 'import ssl,sys,urllib.request; url=sys.argv.__getitem__(1); ctx=ssl._create_unverified_context(); print(urllib.request.urlopen(url, timeout=15, context=ctx).read().decode(), end=\"\")' %s; "
                 "else echo 'Config sync: no HTTPS client available' >&2; exit 127; fi",
                 quoted_url,
                 quoted_url);
    }

    pipe = popen(command, "r");
    if (pipe == NULL)
    {
        printf("Config sync: failed to start HTTPS client: %s\n", strerror(errno));
        return -1;
    }

    while (used + 1 < response_size)
    {
        size_t n = fread(response + used, 1, response_size - used - 1, pipe);
        used += n;
        if (n == 0)
        {
            break;
        }
    }
    response[used] = '\0';

    status = pclose(pipe);
    if (status != 0)
    {
        printf("Config sync: HTTPS client failed for %s with status %d\n", url, status);
        return -1;
    }

    return used > 0 ? 1 : 0;
}
#endif

int amtech_config_sync_poll(const char *config_path, const char *shop_id, amtech_config_t *config)
{
    char response[CONFIG_SYNC_RESPONSE_MAX];
    amtech_config_t updated_config;
    int fetch_result;
    int update_result;

    if (config_path == NULL || shop_id == NULL || config == NULL)
    {
        return -1;
    }

#ifndef SIMULATE_NETWORK
    if (config->device_config_token[0] == '\0')
    {
        return 0;
    }
#endif

    fetch_result = fetch_device_config_json(config, shop_id, response, sizeof(response));
    if (fetch_result <= 0)
    {
        return fetch_result;
    }

    updated_config = *config;
    if (amtech_config_sync_parse_json(response, &updated_config) != 0)
    {
        return -1;
    }

    if (updated_config.schedule_arm_hour == config->schedule_arm_hour &&
        updated_config.schedule_arm_minute == config->schedule_arm_minute &&
        updated_config.schedule_disarm_hour == config->schedule_disarm_hour &&
        updated_config.schedule_disarm_minute == config->schedule_disarm_minute &&
        strcmp(updated_config.alert_contacts[0], config->alert_contacts[0]) == 0 &&
        strcmp(updated_config.alert_contacts[1], config->alert_contacts[1]) == 0 &&
        strcmp(updated_config.alert_contacts[2], config->alert_contacts[2]) == 0)
    {
        printf("Config sync: backend config matches local schedule/contact keys\n");
        return 0;
    }

    update_result = amtech_config_sync_update_file(config_path, &updated_config);
    if (update_result < 0)
    {
        return -1;
    }

    *config = updated_config;
    printf("Config sync: applied schedule %02d:%02d-%02d:%02d and alert contacts from backend\n",
           config->schedule_arm_hour,
           config->schedule_arm_minute,
           config->schedule_disarm_hour,
           config->schedule_disarm_minute);
    return 1;
}
