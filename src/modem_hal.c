#include "modem_hal.h"

#include "config.h"
#include "modem_state.h"
#include "sim_modem.h"

#include <stdio.h>
#include <string.h>

#ifndef SIMULATE_MODEM
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

#define MODEM_HAL_RESPONSE_SIZE 1024
#define MODEM_HAL_COMMAND_SIZE 128
#define MODEM_HAL_SMS_PAYLOAD_SIZE 256
#define MODEM_HAL_SHORT_TIMEOUT_MS 3000
#define MODEM_HAL_SMS_TIMEOUT_MS 60000
#define MODEM_HAL_CALL_MAX_DURATION_MS 45000U
#define MODEM_HAL_CALL_STATUS_POLL_MS 5000U

static int voice_call_active = 0;
static unsigned int voice_call_elapsed_ms = 0;
static unsigned int voice_call_status_elapsed_ms = 0;

#ifdef SIMULATE_MODEM
static int simulated_sms_count = 0;
static int simulated_call_count = 0;
static int simulated_hangup_count = 0;
static char simulated_last_sms_number[AMTECH_ALERT_CONTACT_NUMBER_MAX];
static char simulated_last_sms_message[MODEM_HAL_SMS_PAYLOAD_SIZE];
static char simulated_last_call_number[AMTECH_ALERT_CONTACT_NUMBER_MAX];
#endif

int modem_get_registration_status(void)
{
    return (int)modem_state_get_current();
}

int modem_register_network(void)
{
    int tick_result;

    if (modem_state_is_registered())
    {
        return 1;
    }

    tick_result = modem_state_tick();
    if (tick_result != 0)
    {
        return -1;
    }

    return modem_state_is_registered() ? 1 : 0;
}

#ifndef SIMULATE_MODEM
static int configure_serial_port(int fd)
{
    struct termios options;

    if (tcgetattr(fd, &options) != 0)
    {
        printf("Modem HAL: tcgetattr failed: %s\n", strerror(errno));
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
        printf("Modem HAL: tcsetattr failed: %s\n", strerror(errno));
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return 0;
}

static int open_modem_serial(void)
{
    char device_path[AMTECH_MODEM_DEVICE_MAX];
    int fd;

    if (sim_modem_get_device_path(device_path, sizeof(device_path)) != 0)
    {
        return -1;
    }

    fd = open(device_path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        printf("Modem HAL: failed to open %s: %s\n", device_path, strerror(errno));
        return -1;
    }

    if (configure_serial_port(fd) != 0)
    {
        close(fd);
        return -1;
    }

    return fd;
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

            printf("Modem HAL: write failed: %s\n", strerror(errno));
            return -1;
        }

        written += (size_t)result;
    }

    return 0;
}

static int read_until(int fd,
                      const char *label,
                      const char *expected,
                      int timeout_ms,
                      char *response,
                      size_t response_size)
{
    size_t response_length = 0;
    int remaining_ms = timeout_ms;

    if (response == NULL || response_size == 0)
    {
        return -1;
    }

    response[0] = '\0';

    while (remaining_ms >= 0 && response_length + 1 < response_size)
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

            printf("Modem HAL: poll failed while waiting for %s: %s\n", label, strerror(errno));
            return -1;
        }

        if (ready > 0 && (pfd.revents & POLLIN))
        {
            ssize_t bytes_read = read(fd, response + response_length, response_size - response_length - 1);
            if (bytes_read < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                {
                    continue;
                }

                printf("Modem HAL: read failed while waiting for %s: %s\n", label, strerror(errno));
                return -1;
            }

            if (bytes_read > 0)
            {
                response_length += (size_t)bytes_read;
                response[response_length] = '\0';

                if (expected != NULL && strstr(response, expected) != NULL)
                {
                    return 0;
                }

                if (strstr(response, "\r\nERROR\r\n") != NULL ||
                    strstr(response, "+CMS ERROR:") != NULL ||
                    strstr(response, "+CME ERROR:") != NULL)
                {
                    printf("Modem HAL: error response while waiting for %s: %s\n", label, response);
                    return -1;
                }
            }
        }

        if (remaining_ms == 0)
        {
            break;
        }
        remaining_ms -= wait_ms;
    }

    printf("Modem HAL: timeout waiting for %s after %d ms, response: %s\n",
           label,
           timeout_ms,
           response[0] != '\0' ? response : "(empty)");
    return -1;
}

static int send_serial_command(int fd, const char *command, const char *expected, int timeout_ms)
{
    char command_line[MODEM_HAL_COMMAND_SIZE];
    char response[MODEM_HAL_RESPONSE_SIZE];

    snprintf(command_line, sizeof(command_line), "%s\r\n", command);
    if (write_all(fd, (const unsigned char *)command_line, strlen(command_line)) != 0)
    {
        return -1;
    }

    return read_until(fd, command, expected, timeout_ms, response, sizeof(response));
}
#endif

int modem_send_sms(const char *number, const char *message)
{
    if (number == NULL || number[0] == '\0' || message == NULL || message[0] == '\0')
    {
        printf("Modem HAL: SMS number and message are required\n");
        return -1;
    }

#ifdef SIMULATE_MODEM
    printf("Modem HAL: would send SMS to %s: %s\n", number, message);
    snprintf(simulated_last_sms_number, sizeof(simulated_last_sms_number), "%s", number);
    snprintf(simulated_last_sms_message, sizeof(simulated_last_sms_message), "%s", message);
    simulated_sms_count++;
    return 0;
#else
    int fd;
    char cmgs_command[MODEM_HAL_COMMAND_SIZE];
    char response[MODEM_HAL_RESPONSE_SIZE];
    unsigned char payload[MODEM_HAL_SMS_PAYLOAD_SIZE];
    int payload_length;
    int result = -1;

    fd = open_modem_serial();
    if (fd < 0)
    {
        return -1;
    }

    if (send_serial_command(fd, "AT+CMGF=1", "OK", MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
    {
        goto done;
    }

    snprintf(cmgs_command, sizeof(cmgs_command), "AT+CMGS=\"%s\"\r\n", number);
    if (write_all(fd, (const unsigned char *)cmgs_command, strlen(cmgs_command)) != 0)
    {
        goto done;
    }

    if (read_until(fd,
                   "AT+CMGS prompt",
                   ">",
                   MODEM_HAL_SHORT_TIMEOUT_MS,
                   response,
                   sizeof(response)) != 0)
    {
        goto done;
    }

    payload_length = snprintf((char *)payload, sizeof(payload), "%s%c", message, 0x1A);
    if (payload_length < 0 || (size_t)payload_length >= sizeof(payload))
    {
        printf("Modem HAL: SMS message too large\n");
        goto done;
    }

    if (write_all(fd, payload, (size_t)payload_length) != 0)
    {
        goto done;
    }

    if (read_until(fd,
                   "SMS final response",
                   "+CMGS:",
                   MODEM_HAL_SMS_TIMEOUT_MS,
                   response,
                   sizeof(response)) != 0)
    {
        goto done;
    }

    if (strstr(response, "OK") == NULL)
    {
        if (read_until(fd,
                       "SMS OK",
                       "OK",
                       MODEM_HAL_SHORT_TIMEOUT_MS,
                       response,
                       sizeof(response)) != 0)
        {
            goto done;
        }
    }

    printf("Modem HAL: SMS sent to %s\n", number);
    result = 0;

done:
    close(fd);
    return result;
#endif
}

int modem_make_voice_call(const char *number)
{
    char command[MODEM_HAL_COMMAND_SIZE];
#ifndef SIMULATE_MODEM
    char response[MODEM_HAL_RESPONSE_SIZE];
#endif

    if (number == NULL || number[0] == '\0')
    {
        printf("Modem HAL: voice call number is required\n");
        return -1;
    }

    if (voice_call_active)
    {
        printf("Modem HAL: voice call already active, skipping new call to %s\n", number);
        return 0;
    }

    snprintf(command, sizeof(command), "ATD%s;", number);

#ifdef SIMULATE_MODEM
    printf("Modem HAL: would start voice call to %s with %s\n", number, command);
    snprintf(simulated_last_call_number, sizeof(simulated_last_call_number), "%s", number);
    simulated_call_count++;
#else
    if (sim_modem_send_at(command, response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
    {
        return -1;
    }

    if (strstr(response, "ERROR") != NULL)
    {
        printf("Modem HAL: voice call command failed: %s\n", response);
        return -1;
    }

    printf("Modem HAL: voice call started to %s\n", number);
#endif

    voice_call_active = 1;
    voice_call_elapsed_ms = 0;
    voice_call_status_elapsed_ms = 0;
    return 0;
}

void modem_hal_tick(unsigned int elapsed_ms)
{
#ifndef SIMULATE_MODEM
    char response[MODEM_HAL_RESPONSE_SIZE];
#endif

    if (!voice_call_active || elapsed_ms == 0)
    {
        return;
    }

    if (elapsed_ms > MODEM_HAL_CALL_MAX_DURATION_MS - voice_call_elapsed_ms)
    {
        voice_call_elapsed_ms = MODEM_HAL_CALL_MAX_DURATION_MS;
    }
    else
    {
        voice_call_elapsed_ms += elapsed_ms;
    }

    if (voice_call_elapsed_ms >= MODEM_HAL_CALL_MAX_DURATION_MS)
    {
#ifdef SIMULATE_MODEM
        printf("Modem HAL: would send ATH after %u ms safety timeout\n",
               MODEM_HAL_CALL_MAX_DURATION_MS);
        simulated_hangup_count++;
#else
        if (sim_modem_send_at("ATH", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
        {
            printf("Modem HAL: warning: ATH failed after call safety timeout\n");
        }
#endif
        voice_call_active = 0;
        voice_call_elapsed_ms = 0;
        voice_call_status_elapsed_ms = 0;
        return;
    }

    if (elapsed_ms > MODEM_HAL_CALL_STATUS_POLL_MS - voice_call_status_elapsed_ms)
    {
        voice_call_status_elapsed_ms = MODEM_HAL_CALL_STATUS_POLL_MS;
    }
    else
    {
        voice_call_status_elapsed_ms += elapsed_ms;
    }

    if (voice_call_status_elapsed_ms < MODEM_HAL_CALL_STATUS_POLL_MS)
    {
        return;
    }
    voice_call_status_elapsed_ms = 0;

#ifdef SIMULATE_MODEM
    printf("Modem HAL: would poll voice call status with AT+CLCC\n");
#else
    if (sim_modem_send_at("AT+CLCC", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
    {
        return;
    }

    if (strstr(response, "+CLCC:") == NULL)
    {
        printf("Modem HAL: voice call no longer active\n");
        voice_call_active = 0;
        voice_call_elapsed_ms = 0;
        voice_call_status_elapsed_ms = 0;
    }
#endif
}

int modem_voice_call_is_active(void)
{
    return voice_call_active;
}

#ifdef SIMULATE_MODEM
int modem_get_simulated_sms_count(void)
{
    return simulated_sms_count;
}

int modem_get_simulated_call_count(void)
{
    return simulated_call_count;
}

int modem_get_simulated_hangup_count(void)
{
    return simulated_hangup_count;
}

const char *modem_get_simulated_last_sms_number(void)
{
    return simulated_last_sms_number;
}

const char *modem_get_simulated_last_sms_message(void)
{
    return simulated_last_sms_message;
}

const char *modem_get_simulated_last_call_number(void)
{
    return simulated_last_call_number;
}

void modem_reset_simulated_state(void)
{
    simulated_sms_count = 0;
    simulated_call_count = 0;
    simulated_hangup_count = 0;
    simulated_last_sms_number[0] = '\0';
    simulated_last_sms_message[0] = '\0';
    simulated_last_call_number[0] = '\0';
    voice_call_active = 0;
    voice_call_elapsed_ms = 0;
    voice_call_status_elapsed_ms = 0;
}
#endif
