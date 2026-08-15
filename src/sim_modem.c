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
    printf("SIM modem: writing AT command to %s: %s\n", device_path, command);
    fflush(stdout);

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
    if (response_length == 0)
    {
        printf("SIM modem: timeout waiting for response to %s after %d ms\n", command, timeout_ms);
        fflush(stdout);
    }

    return response_length > 0 ? 0 : -1;
#endif
}
