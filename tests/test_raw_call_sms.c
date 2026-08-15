#include "sim_modem.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define RAW_CALL_SMS_DEVICE "/dev/ttyS5"
#define RAW_CALL_SMS_BAUD B115200
#define RAW_CALL_SMS_RESPONSE_SIZE 1024
#define RAW_CALL_SMS_SHORT_TIMEOUT_MS 3000
#define RAW_CALL_SMS_SMS_TIMEOUT_MS 60000
#define RAW_CALL_SMS_TARGET_NUMBER "+918550991121"
#define RAW_CALL_SMS_TEXT "AMTECH AI Guard Hub test SMS"

static int configure_serial(int fd)
{
    struct termios options;

    if (tcgetattr(fd, &options) != 0)
    {
        printf("tcgetattr failed: %s\n", strerror(errno));
        return -1;
    }

    cfmakeraw(&options);
    cfsetispeed(&options, RAW_CALL_SMS_BAUD);
    cfsetospeed(&options, RAW_CALL_SMS_BAUD);

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
        printf("tcsetattr failed: %s\n", strerror(errno));
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t length)
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

            printf("write failed: %s\n", strerror(errno));
            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}

static void print_visible_bytes(const char *label, const unsigned char *buffer, size_t length)
{
    size_t i;

    printf("%s received %lu byte(s):\n", label, (unsigned long)length);
    printf("text: ");
    for (i = 0; i < length; i++)
    {
        unsigned char ch = buffer[i];

        if (ch == '\r')
        {
            printf("\\r");
        }
        else if (ch == '\n')
        {
            printf("\\n");
        }
        else if (ch == 0x1A)
        {
            printf("\\x1A");
        }
        else if (ch >= 32 && ch <= 126)
        {
            putchar(ch);
        }
        else
        {
            printf("\\x%02X", ch);
        }
    }
    printf("\nhex: ");
    for (i = 0; i < length; i++)
    {
        printf("%02X ", buffer[i]);
    }
    printf("\n");
}

static int read_until(int fd,
                      const char *label,
                      const char *expected,
                      int timeout_ms,
                      unsigned char *response,
                      size_t response_size,
                      size_t *response_length)
{
    int remaining_ms = timeout_ms;

    *response_length = 0;

    while (remaining_ms >= 0 && *response_length + 1 < response_size)
    {
        struct pollfd pfd;
        int wait_ms = remaining_ms > 100 ? 100 : remaining_ms;
        int ready;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        ready = poll(&pfd, 1, wait_ms);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("%s poll failed: %s\n", label, strerror(errno));
            return -1;
        }

        if (ready > 0 && (pfd.revents & POLLIN))
        {
            ssize_t bytes_read = read(fd,
                                      response + *response_length,
                                      response_size - *response_length - 1);
            if (bytes_read < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                {
                    continue;
                }

                printf("%s read failed: %s\n", label, strerror(errno));
                return -1;
            }

            if (bytes_read > 0)
            {
                *response_length += (size_t)bytes_read;
                response[*response_length] = '\0';

                if (expected != NULL && strstr((const char *)response, expected) != NULL)
                {
                    return 0;
                }

                if (strstr((const char *)response, "\r\nERROR\r\n") != NULL ||
                    strstr((const char *)response, "+CMS ERROR:") != NULL ||
                    strstr((const char *)response, "+CME ERROR:") != NULL)
                {
                    return 1;
                }
            }
        }

        if (remaining_ms == 0)
        {
            break;
        }
        remaining_ms -= wait_ms;
    }

    return *response_length > 0 ? 1 : -1;
}

static int send_serial_command(int fd, const char *command, const char *expected, int timeout_ms)
{
    unsigned char response[RAW_CALL_SMS_RESPONSE_SIZE];
    char command_line[128];
    size_t response_length = 0;
    int result;

    snprintf(command_line, sizeof(command_line), "%s\r\n", command);
    printf("Sending: %s\\r\\n\n", command);

    if (write_all(fd, (const unsigned char *)command_line, strlen(command_line)) != 0)
    {
        return -1;
    }

    result = read_until(fd,
                        command,
                        expected,
                        timeout_ms,
                        response,
                        sizeof(response),
                        &response_length);

    if (response_length == 0)
    {
        printf("%s: no response received within %d ms\n", command, timeout_ms);
    }
    else
    {
        print_visible_bytes(command, response, response_length);
    }

    return result == 0 ? 0 : -1;
}

static int run_voice_call_test(int fd)
{
    char dial_command[64];

    snprintf(dial_command, sizeof(dial_command), "ATD%s;", RAW_CALL_SMS_TARGET_NUMBER);
    printf("\nTest 1: voice call to %s\n", RAW_CALL_SMS_TARGET_NUMBER);

    if (send_serial_command(fd, dial_command, "OK", RAW_CALL_SMS_SHORT_TIMEOUT_MS) != 0)
    {
        printf("Voice call dial command did not return OK\n");
    }

    printf("Waiting 5 seconds before hangup...\n");
    sleep(5);

    printf("Sending ATH to hang up\n");
    return send_serial_command(fd, "ATH", "OK", RAW_CALL_SMS_SHORT_TIMEOUT_MS);
}

static int run_sms_test(int fd)
{
    unsigned char response[RAW_CALL_SMS_RESPONSE_SIZE];
    char cmgs_command[64];
    unsigned char sms_payload[256];
    size_t response_length = 0;
    size_t payload_length;
    int result;

    printf("\nTest 2: SMS to %s\n", RAW_CALL_SMS_TARGET_NUMBER);

    if (send_serial_command(fd, "AT+CMGF=1", "OK", RAW_CALL_SMS_SHORT_TIMEOUT_MS) != 0)
    {
        printf("Failed to set SMS text mode\n");
        return -1;
    }

    snprintf(cmgs_command, sizeof(cmgs_command), "AT+CMGS=\"%s\"\r\n", RAW_CALL_SMS_TARGET_NUMBER);
    printf("Sending: AT+CMGS=\"%s\"\\r\\n and waiting for > prompt\n", RAW_CALL_SMS_TARGET_NUMBER);

    if (write_all(fd, (const unsigned char *)cmgs_command, strlen(cmgs_command)) != 0)
    {
        return -1;
    }

    result = read_until(fd,
                        "AT+CMGS prompt",
                        ">",
                        RAW_CALL_SMS_SHORT_TIMEOUT_MS,
                        response,
                        sizeof(response),
                        &response_length);
    if (response_length == 0)
    {
        printf("AT+CMGS prompt: no response received within %d ms\n", RAW_CALL_SMS_SHORT_TIMEOUT_MS);
    }
    else
    {
        print_visible_bytes("AT+CMGS prompt", response, response_length);
    }

    if (result != 0)
    {
        printf("Did not receive SMS > prompt; not sending message body\n");
        return -1;
    }

    payload_length = (size_t)snprintf((char *)sms_payload,
                                      sizeof(sms_payload),
                                      "%s%c",
                                      RAW_CALL_SMS_TEXT,
                                      0x1A);
    printf("Sending SMS text followed by Ctrl+Z: %s\\x1A\n", RAW_CALL_SMS_TEXT);
    if (write_all(fd, sms_payload, payload_length) != 0)
    {
        return -1;
    }

    result = read_until(fd,
                        "SMS final response",
                        "OK",
                        RAW_CALL_SMS_SMS_TIMEOUT_MS,
                        response,
                        sizeof(response),
                        &response_length);
    if (response_length == 0)
    {
        printf("SMS final response: no response received within %d ms\n", RAW_CALL_SMS_SMS_TIMEOUT_MS);
    }
    else
    {
        print_visible_bytes("SMS final response", response, response_length);
    }

    return result == 0 ? 0 : -1;
}

int main(void)
{
    char response[RAW_CALL_SMS_RESPONSE_SIZE];
    int fd;
    int failures = 0;

    printf("Raw call/SMS diagnostic using %s, 115200 8N1, no flow control\n", RAW_CALL_SMS_DEVICE);
    printf("Target number: %s\n", RAW_CALL_SMS_TARGET_NUMBER);

    if (sim_modem_send_at("AT", response, sizeof(response), RAW_CALL_SMS_SHORT_TIMEOUT_MS) != 0)
    {
        printf("Initial AT via sim_modem_send_at failed\n");
        failures++;
    }
    else
    {
        printf("Initial AT response: %s\n", response);
    }

    if (sim_modem_send_at("AT+CEREG?", response, sizeof(response), RAW_CALL_SMS_SHORT_TIMEOUT_MS) != 0)
    {
        printf("Initial AT+CEREG? via sim_modem_send_at failed\n");
        failures++;
    }
    else
    {
        printf("Initial AT+CEREG? response: %s\n", response);
    }

    fd = open(RAW_CALL_SMS_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        printf("open %s failed: %s\n", RAW_CALL_SMS_DEVICE, strerror(errno));
        return 1;
    }

    if (configure_serial(fd) != 0)
    {
        close(fd);
        return 1;
    }

    if (run_voice_call_test(fd) != 0)
    {
        failures++;
    }

    if (run_sms_test(fd) != 0)
    {
        failures++;
    }

    close(fd);

    printf("\nRaw call/SMS diagnostic complete: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
