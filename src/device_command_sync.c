#include "device_command_sync.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_COMMAND_RESPONSE_MAX 1024
#define DEVICE_COMMAND_COMMAND_MAX 4096
#define DEVICE_COMMAND_QUOTED_MAX 512

#ifdef SIMULATE_NETWORK
static char simulated_response[DEVICE_COMMAND_RESPONSE_MAX];
static int simulated_ack_count = 0;
#endif

static void shell_quote(const char *input, char *output, size_t output_size)
{
    size_t out = 0;
    size_t i;

    if (output == NULL || output_size == 0)
    {
        return;
    }
    output[0] = '\0';

    if (out + 1 < output_size)
    {
        output[out++] = '\'';
    }

    for (i = 0; input != NULL && input[i] != '\0' && out + 5 < output_size; i++)
    {
        if (input[i] == '\'')
        {
            output[out++] = '\'';
            output[out++] = '\\';
            output[out++] = '\'';
            output[out++] = '\'';
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

static void build_base_url(const amtech_config_t *config, char *base, size_t base_size)
{
    size_t length;

    snprintf(base, base_size, "%s", config->backend_base_url);
    length = strlen(base);
    while (length > 0 && base[length - 1] == '/')
    {
        base[length - 1] = '\0';
        length--;
    }
}

static int extract_json_string(const char *json, const char *key, char *output, size_t output_size)
{
    char pattern[64];
    const char *cursor;
    const char *value_start;
    const char *value_end;
    size_t length;

    if (json == NULL || key == NULL || output == NULL || output_size == 0)
    {
        return -1;
    }
    output[0] = '\0';

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
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
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
    {
        cursor++;
    }

    if (strncmp(cursor, "null", 4) == 0)
    {
        return 1;
    }
    if (*cursor != '"')
    {
        return -1;
    }

    value_start = cursor + 1;
    value_end = value_start;
    while (*value_end != '\0' && *value_end != '"')
    {
        value_end++;
    }
    if (*value_end != '"')
    {
        return -1;
    }

    length = (size_t)(value_end - value_start);
    if (length >= output_size)
    {
        length = output_size - 1;
    }
    memcpy(output, value_start, length);
    output[length] = '\0';
    return 0;
}

static int parse_pending_command_json(const char *json, amtech_device_command_t *command)
{
    char type[32];
    int type_result;

    if (json == NULL || command == NULL)
    {
        return -1;
    }

    command->type = AMTECH_DEVICE_COMMAND_NONE;
    command->id[0] = '\0';

    type_result = extract_json_string(json, "pending_command", type, sizeof(type));
    if (type_result == 1)
    {
        return 0;
    }
    if (type_result != 0)
    {
        printf("Device command sync: invalid pending-command response: %s\n", json);
        return -1;
    }

    if (strcmp(type, "arm") == 0)
    {
        command->type = AMTECH_DEVICE_COMMAND_ARM;
    }
    else if (strcmp(type, "disarm") == 0)
    {
        command->type = AMTECH_DEVICE_COMMAND_DISARM;
    }
    else
    {
        printf("Device command sync: ignoring unknown pending command: %s\n", type);
        return 0;
    }

    if (extract_json_string(json, "pending_command_id", command->id, sizeof(command->id)) != 0 ||
        command->id[0] == '\0')
    {
        printf("Device command sync: pending command missing id\n");
        command->type = AMTECH_DEVICE_COMMAND_NONE;
        command->id[0] = '\0';
        return -1;
    }

    return 0;
}

#ifndef SIMULATE_NETWORK
static int run_https_command(const char *command, char *response, size_t response_size)
{
    FILE *pipe;
    size_t used = 0;
    int status;

    if (response == NULL || response_size == 0)
    {
        return -1;
    }
    response[0] = '\0';

    pipe = popen(command, "r");
    if (pipe == NULL)
    {
        printf("Device command sync: failed to start HTTPS client: %s\n", strerror(errno));
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
        printf("Device command sync: HTTPS client failed with status %d\n", status);
        return -1;
    }

    return 0;
}
#endif

int amtech_device_command_fetch(const amtech_config_t *config,
                                const char *shop_id,
                                amtech_device_command_t *command)
{
#ifdef SIMULATE_NETWORK
    (void)config;
    (void)shop_id;
    return parse_pending_command_json(simulated_response[0] ? simulated_response : "{\"ok\":true,\"pending_command\":null}", command);
#else
    char base[AMTECH_BACKEND_URL_MAX];
    char url[AMTECH_BACKEND_URL_MAX + AMTECH_DEVICE_COMMAND_SHOP_ID_MAX + 64];
    char quoted_url[DEVICE_COMMAND_QUOTED_MAX];
    char quoted_header[DEVICE_COMMAND_QUOTED_MAX];
    char quoted_token[DEVICE_COMMAND_QUOTED_MAX];
    char response[DEVICE_COMMAND_RESPONSE_MAX];
    char command_line[DEVICE_COMMAND_COMMAND_MAX];
    char header[AMTECH_DEVICE_CONFIG_TOKEN_MAX + 40];

    if (config == NULL || shop_id == NULL || command == NULL)
    {
        return -1;
    }

    build_base_url(config, base, sizeof(base));
    snprintf(url, sizeof(url), "%s/shop/%s/pending-command", base, shop_id);
    shell_quote(url, quoted_url, sizeof(quoted_url));

    if (config->device_config_token[0] != '\0')
    {
        snprintf(header, sizeof(header), "X-AMTECH-DEVICE-CONFIG-TOKEN: %s", config->device_config_token);
        shell_quote(header, quoted_header, sizeof(quoted_header));
        shell_quote(config->device_config_token, quoted_token, sizeof(quoted_token));
        snprintf(command_line,
                 sizeof(command_line),
                 "if command -v curl >/dev/null 2>&1; then "
                 "curl --fail --silent --show-error --max-time 8 -H %s %s; "
                 "elif command -v python3 >/dev/null 2>&1; then "
                 "python3 -c 'import ssl,sys,urllib.request; token=sys.argv.__getitem__(1); url=sys.argv.__getitem__(2); req=urllib.request.Request(url, headers={\"X-AMTECH-DEVICE-CONFIG-TOKEN\": token}); ctx=ssl._create_unverified_context(); print(urllib.request.urlopen(req, timeout=8, context=ctx).read().decode(), end=\"\")' %s %s; "
                 "else echo 'Device command sync: no HTTPS client available' >&2; exit 127; fi",
                 quoted_header,
                 quoted_url,
                 quoted_token,
                 quoted_url);
    }
    else
    {
        printf("Device command sync: DEVICE_CONFIG_TOKEN is unset; skipping command poll\n");
        command->type = AMTECH_DEVICE_COMMAND_NONE;
        command->id[0] = '\0';
        return 0;
    }

    if (run_https_command(command_line, response, sizeof(response)) != 0)
    {
        return -1;
    }

    return parse_pending_command_json(response, command);
#endif
}

int amtech_device_command_ack(const amtech_config_t *config,
                              const char *shop_id,
                              const amtech_device_command_t *command)
{
#ifdef SIMULATE_NETWORK
    (void)config;
    (void)shop_id;
    (void)command;
    simulated_ack_count++;
    return 0;
#else
    char base[AMTECH_BACKEND_URL_MAX];
    char url[AMTECH_BACKEND_URL_MAX + AMTECH_DEVICE_COMMAND_SHOP_ID_MAX + 80];
    char quoted_url[DEVICE_COMMAND_QUOTED_MAX];
    char quoted_header[DEVICE_COMMAND_QUOTED_MAX];
    char quoted_token[DEVICE_COMMAND_QUOTED_MAX];
    char quoted_body[DEVICE_COMMAND_QUOTED_MAX];
    char response[DEVICE_COMMAND_RESPONSE_MAX];
    char command_line[DEVICE_COMMAND_COMMAND_MAX];
    char header[AMTECH_DEVICE_CONFIG_TOKEN_MAX + 40];
    char body[AMTECH_DEVICE_COMMAND_ID_MAX + 32];

    if (config == NULL || shop_id == NULL || command == NULL || command->id[0] == '\0')
    {
        return -1;
    }

    build_base_url(config, base, sizeof(base));
    snprintf(url, sizeof(url), "%s/shop/%s/pending-command/ack", base, shop_id);
    snprintf(body, sizeof(body), "{\"pending_command_id\":\"%s\"}", command->id);
    snprintf(header, sizeof(header), "X-AMTECH-DEVICE-CONFIG-TOKEN: %s", config->device_config_token);
    shell_quote(url, quoted_url, sizeof(quoted_url));
    shell_quote(header, quoted_header, sizeof(quoted_header));
    shell_quote(config->device_config_token, quoted_token, sizeof(quoted_token));
    shell_quote(body, quoted_body, sizeof(quoted_body));

    snprintf(command_line,
             sizeof(command_line),
             "if command -v curl >/dev/null 2>&1; then "
             "curl --fail --silent --show-error --max-time 8 -X POST -H %s -H 'Content-Type: application/json' --data %s %s; "
             "elif command -v python3 >/dev/null 2>&1; then "
             "python3 -c 'import ssl,sys,urllib.request; token=sys.argv.__getitem__(1); url=sys.argv.__getitem__(2); body=sys.argv.__getitem__(3).encode(); req=urllib.request.Request(url, data=body, method=\"POST\", headers={\"X-AMTECH-DEVICE-CONFIG-TOKEN\": token, \"Content-Type\":\"application/json\"}); ctx=ssl._create_unverified_context(); print(urllib.request.urlopen(req, timeout=8, context=ctx).read().decode(), end=\"\")' %s %s %s; "
             "else echo 'Device command sync: no HTTPS client available' >&2; exit 127; fi",
             quoted_header,
             quoted_body,
             quoted_url,
             quoted_token,
             quoted_url,
             quoted_body);

    return run_https_command(command_line, response, sizeof(response));
#endif
}

#ifdef SIMULATE_NETWORK
void amtech_device_command_set_simulated_response(const char *json)
{
    snprintf(simulated_response, sizeof(simulated_response), "%s", json != NULL ? json : "");
}

int amtech_device_command_simulated_ack_count(void)
{
    return simulated_ack_count;
}

void amtech_device_command_simulated_reset(void)
{
    simulated_response[0] = '\0';
    simulated_ack_count = 0;
}
#endif
