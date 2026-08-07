#include "sim_modem.h"

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIMULATE_MODEM
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#define SIM_MODEM_COMMAND_MAX 128
#define SIM_MODEM_RESPONSE_MAX 512
#define SIM_MODEM_CONTEXT_ID 1
#define SIM_MODEM_TIMEOUT_SHORT_MS 5000
#define SIM_MODEM_TIMEOUT_ATTACH_MS 45000
#define SIM_MODEM_TIMEOUT_ACTIVATE_MS 150000

static const char *sim_modem_config_path(void)
{
    const char *path = getenv("AMTECH_CONFIG_PATH");

    if (path != NULL && path[0] != '\0')
    {
        return path;
    }

    return AMTECH_DEFAULT_CONFIG_PATH;
}

int sim_modem_get_device_path(char *path_buffer, size_t buffer_size)
{
    amtech_config_t config;

    if (path_buffer == NULL || buffer_size == 0)
    {
        return -1;
    }

    amtech_config_set_defaults(&config);
    if (amtech_config_load(sim_modem_config_path(), &config) != 0)
    {
        printf("SIM modem: failed to load config, using default MODEM_DEVICE=%s\n",
               config.modem_device);
    }

    snprintf(path_buffer, buffer_size, "%s", config.modem_device);
    return 0;
}

#ifdef SIMULATE_MODEM
static const char *simulated_response_for_command(const char *command)
{
    const char *fail_command = getenv("AMTECH_SIM_MODEM_FAIL_COMMAND");
    const char *operator_name = getenv("AMTECH_SIM_MODEM_OPERATOR");
    static char cops_response[128];

    if (fail_command != NULL && strcmp(command, fail_command) == 0)
    {
        return "\r\nERROR\r\n";
    }

    if (strcmp(command, "AT+CSQ") == 0)
    {
        return "\r\n+CSQ: 20,99\r\n\r\nOK\r\n";
    }

    if (strcmp(command, "AT+CREG?") == 0)
    {
        return "\r\n+CREG: 0,1\r\n\r\nOK\r\n";
    }

    if (strcmp(command, "AT+CPIN?") == 0)
    {
        return "\r\n+CPIN: READY\r\n\r\nOK\r\n";
    }

    if (strcmp(command, "AT+CEREG?") == 0)
    {
        return "\r\n+CEREG: 0,1\r\n\r\nOK\r\n";
    }

    if (strcmp(command, "AT+COPS?") == 0)
    {
        if (operator_name == NULL || operator_name[0] == '\0')
        {
            operator_name = "405864";
        }

        if (strspn(operator_name, "0123456789") == strlen(operator_name))
        {
            snprintf(cops_response,
                     sizeof(cops_response),
                     "\r\n+COPS: 0,2,\"%s\",7\r\n\r\nOK\r\n",
                     operator_name);
            return cops_response;
        }

        snprintf(cops_response,
                 sizeof(cops_response),
                 "\r\n+COPS: 0,0,\"%s\",7\r\n\r\nOK\r\n",
                 operator_name);
        return cops_response;
    }

    if (strcmp(command, "AT+CGPADDR=1") == 0)
    {
        return "\r\n+CGPADDR: 1,10.83.214.110\r\n\r\nOK\r\n";
    }

    return "\r\nOK\r\n";
}
#else
static int configure_serial_port(int fd)
{
    struct termios options;

    if (tcgetattr(fd, &options) != 0)
    {
        printf("SIM modem: tcgetattr failed: %s\n", strerror(errno));
        return -1;
    }

    cfmakeraw(&options);
    cfsetispeed(&options, B115200);
    cfsetospeed(&options, B115200);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CRTSCTS;
    options.c_iflag &= ~(IXON | IXOFF | IXANY);
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &options) != 0)
    {
        printf("SIM modem: tcsetattr failed: %s\n", strerror(errno));
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return 0;
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t written = 0;

    while (written < length)
    {
        ssize_t result = write(fd, data + written, length - written);
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("SIM modem: write failed: %s\n", strerror(errno));
            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}
#endif

static int response_contains_ok(const char *response)
{
    return response != NULL && strstr(response, "OK") != NULL && strstr(response, "ERROR") == NULL;
}

static int apn_is_safe(const char *apn)
{
    const char *cursor;

    if (apn == NULL || apn[0] == '\0')
    {
        return 0;
    }

    for (cursor = apn; *cursor != '\0'; cursor++)
    {
        if (*cursor == '"' || *cursor == '\r' || *cursor == '\n')
        {
            return 0;
        }
    }

    return 1;
}

static int pdp_type_is_safe(const char *pdp_type)
{
    if (pdp_type == NULL)
    {
        return 0;
    }

    return strcmp(pdp_type, "IP") == 0 ||
           strcmp(pdp_type, "IPV6") == 0 ||
           strcmp(pdp_type, "IPV4V6") == 0;
}

static int send_expect_ok(const char *command, int timeout_ms)
{
    char response[SIM_MODEM_RESPONSE_MAX];

    if (sim_modem_send_at(command, response, sizeof(response), timeout_ms) != 0)
    {
        printf("SIM modem: command failed: %s\n", command);
        return -1;
    }

    if (!response_contains_ok(response))
    {
        printf("SIM modem: command did not return OK: %s\n", command);
        printf("SIM modem response: %s\n", response);
        return -1;
    }

    return 0;
}

int sim_modem_configure_pdp_context(int context_id, const char *pdp_type, const char *apn)
{
    char command[SIM_MODEM_COMMAND_MAX];

    if (context_id <= 0 || !pdp_type_is_safe(pdp_type) || !apn_is_safe(apn))
    {
        printf("SIM modem: invalid PDP context configuration\n");
        return -1;
    }

    snprintf(command, sizeof(command), "AT+CGDCONT=%d,\"%s\",\"%s\"", context_id, pdp_type, apn);
    return send_expect_ok(command, SIM_MODEM_TIMEOUT_SHORT_MS);
}

static int extract_ip_from_cgpaddr(const char *response, char *ip_buffer, size_t buffer_size)
{
    const char *line;
    const char *comma;
    const char *cursor;
    size_t length = 0;
    int saw_dot = 0;

    if (response == NULL || ip_buffer == NULL || buffer_size == 0)
    {
        return -1;
    }

    ip_buffer[0] = '\0';

    line = strstr(response, "+CGPADDR:");
    if (line == NULL)
    {
        return -1;
    }

    comma = strchr(line, ',');
    if (comma == NULL)
    {
        return -1;
    }

    cursor = comma + 1;
    while (*cursor == ' ' || *cursor == '"')
    {
        cursor++;
    }

    while (*cursor != '\0' &&
           *cursor != '\r' &&
           *cursor != '\n' &&
           *cursor != ',' &&
           *cursor != '"' &&
           length + 1 < buffer_size)
    {
        if (*cursor == '.')
        {
            saw_dot = 1;
        }

        ip_buffer[length] = *cursor;
        length++;
        cursor++;
    }

    ip_buffer[length] = '\0';

    if (length == 0 || !saw_dot)
    {
        ip_buffer[0] = '\0';
        return -1;
    }

    return 0;
}

int sim_modem_send_at(const char *command,
                      char *response_buffer,
                      size_t buffer_size,
                      int timeout_ms)
{
    char device_path[AMTECH_MODEM_DEVICE_MAX];
#ifndef SIMULATE_MODEM
    char command_line[SIM_MODEM_COMMAND_MAX];
    int fd;
    struct pollfd poll_fd;
    size_t response_length = 0;
    int remaining_timeout_ms = timeout_ms;
#endif

    if (command == NULL || response_buffer == NULL || buffer_size == 0 || timeout_ms < 0)
    {
        return -1;
    }

    response_buffer[0] = '\0';

    if (sim_modem_get_device_path(device_path, sizeof(device_path)) != 0)
    {
        return -1;
    }

#ifdef SIMULATE_MODEM
    printf("SIM modem: would send AT command to %s: %s\n", device_path, command);
    snprintf(response_buffer, buffer_size, "%s", simulated_response_for_command(command));
    return 0;
#else
    /*
     * Hardware spec note: serial handling on the configured modem UART must eventually run
     * in an isolated background thread so AT command latency cannot delay the
     * real-time GPIO interrupt loop. This basic blocking helper is intentionally
     * standalone for first bring-up testing only.
     */
    fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        printf("SIM modem: failed to open %s: %s\n", device_path, strerror(errno));
        return -1;
    }

    if (configure_serial_port(fd) != 0)
    {
        close(fd);
        return -1;
    }

    snprintf(command_line, sizeof(command_line), "%s\r\n", command);
    if (write_all(fd, command_line, strlen(command_line)) != 0)
    {
        close(fd);
        return -1;
    }

    poll_fd.fd = fd;
    poll_fd.events = POLLIN;
    poll_fd.revents = 0;

    while (remaining_timeout_ms >= 0 && response_length + 1 < buffer_size)
    {
        int wait_ms = remaining_timeout_ms > 100 ? 100 : remaining_timeout_ms;
        int ready = poll(&poll_fd, 1, wait_ms);

        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("SIM modem: poll failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }

        if (ready > 0 && (poll_fd.revents & POLLIN))
        {
            ssize_t bytes_read = read(fd,
                                      response_buffer + response_length,
                                      buffer_size - response_length - 1);
            if (bytes_read < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                {
                    continue;
                }

                printf("SIM modem: read failed: %s\n", strerror(errno));
                close(fd);
                return -1;
            }

            if (bytes_read > 0)
            {
                response_length += (size_t)bytes_read;
                response_buffer[response_length] = '\0';

                if (strstr(response_buffer, "\r\nOK\r\n") != NULL ||
                    strstr(response_buffer, "\r\nERROR\r\n") != NULL)
                {
                    break;
                }
            }
        }

        if (remaining_timeout_ms == 0)
        {
            break;
        }
        remaining_timeout_ms -= wait_ms;
    }

    close(fd);
    return response_length > 0 ? 0 : -1;
#endif
}

int sim_modem_connect_data_profile(const char *apn, const char *pdp_type)
{
    char command[SIM_MODEM_COMMAND_MAX];
    char ip_address[64];

    if (!apn_is_safe(apn) || !pdp_type_is_safe(pdp_type))
    {
        printf("SIM modem: invalid APN or PDP type\n");
        return -1;
    }

    /*
     * Standard 3GPP/SIMCom SIM767XX packet-domain sequence:
     * - AT+CGATT=1 attaches to packet service.
     * - AT+CGDCONT defines PDP context 1 with the caller-supplied PDP type/APN.
     * - AT+CGACT=1,1 activates PDP context 1.
     * - AT+CGPADDR=1 confirms an assigned address.
     *
     * Carrier-specific APN strings and exact attach/activation timing still
     * need validation on the physical SIM7672 module and Indian SIMs.
     */
    if (send_expect_ok("AT", SIM_MODEM_TIMEOUT_SHORT_MS) != 0)
    {
        return -1;
    }

    if (send_expect_ok("AT+CGATT=1", SIM_MODEM_TIMEOUT_ATTACH_MS) != 0)
    {
        return -1;
    }

    if (sim_modem_configure_pdp_context(SIM_MODEM_CONTEXT_ID, pdp_type, apn) != 0)
    {
        return -1;
    }

    snprintf(command, sizeof(command), "AT+CGACT=1,%d", SIM_MODEM_CONTEXT_ID);
    if (send_expect_ok(command, SIM_MODEM_TIMEOUT_ACTIVATE_MS) != 0)
    {
        return -1;
    }

    if (sim_modem_get_ip(ip_address, sizeof(ip_address)) != 0)
    {
        return -1;
    }

    printf("SIM modem: data context active, IP=%s\n", ip_address);
    return 0;
}

int sim_modem_connect_data(const char *apn)
{
    return sim_modem_connect_data_profile(apn, "IP");
}

int sim_modem_get_ip(char *ip_buffer, size_t buffer_size)
{
    char response[SIM_MODEM_RESPONSE_MAX];

    if (ip_buffer == NULL || buffer_size == 0)
    {
        return -1;
    }

    ip_buffer[0] = '\0';

    if (sim_modem_send_at("AT+CGPADDR=1", response, sizeof(response), SIM_MODEM_TIMEOUT_SHORT_MS) != 0)
    {
        return -1;
    }

    if (!response_contains_ok(response))
    {
        printf("SIM modem: AT+CGPADDR did not return OK\n");
        printf("SIM modem response: %s\n", response);
        return -1;
    }

    if (extract_ip_from_cgpaddr(response, ip_buffer, buffer_size) != 0)
    {
        printf("SIM modem: no IP address found in AT+CGPADDR response\n");
        printf("SIM modem response: %s\n", response);
        return -1;
    }

    return 0;
}
