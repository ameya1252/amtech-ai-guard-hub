#include "modem_hal.h"

#include "config.h"
#include "modem_state.h"
#include "sim_modem.h"

#include <stdio.h>
#include <string.h>

#include <pthread.h>

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
#define MODEM_HAL_SIMULATED_HISTORY_MAX 32
#define MODEM_HAL_SIMULATED_STATUS_MAX 64
#define MODEM_HAL_SIMULATED_INBOX_MAX 8
#define MODEM_HAL_SHORT_TIMEOUT_MS 3000
#define MODEM_HAL_SMS_READ_TIMEOUT_MS 5000
#define MODEM_HAL_SMS_TIMEOUT_MS 60000
#define MODEM_HAL_CALL_MAX_DURATION_MS 45000U
#define MODEM_HAL_CALL_STATUS_POLL_MS 1000U

static pthread_mutex_t modem_hal_mutex = PTHREAD_MUTEX_INITIALIZER;
static int voice_call_active = 0;
static unsigned int voice_call_elapsed_ms = 0;
static unsigned int voice_call_status_elapsed_ms = 0;
static modem_call_status_t voice_call_status = MODEM_CALL_STATUS_IDLE;

#ifdef SIMULATE_MODEM
static int simulated_sms_count = 0;
static int simulated_call_count = 0;
static int simulated_hangup_count = 0;
static char simulated_last_sms_number[AMTECH_ALERT_CONTACT_NUMBER_MAX];
static char simulated_last_sms_message[MODEM_HAL_SMS_PAYLOAD_SIZE];
static char simulated_last_call_number[AMTECH_ALERT_CONTACT_NUMBER_MAX];
static char simulated_sms_numbers[MODEM_HAL_SIMULATED_HISTORY_MAX][AMTECH_ALERT_CONTACT_NUMBER_MAX];
static char simulated_call_numbers[MODEM_HAL_SIMULATED_HISTORY_MAX][AMTECH_ALERT_CONTACT_NUMBER_MAX];
static modem_call_status_t simulated_status_sequence[MODEM_HAL_SIMULATED_STATUS_MAX];
static int simulated_status_sequence_count = 0;
static int simulated_status_sequence_index = 0;
static int simulated_call_start_results[MODEM_HAL_SIMULATED_HISTORY_MAX];
static int simulated_call_start_result_count = 0;
static int simulated_call_start_result_index = 0;
static int simulated_sms_send_results[MODEM_HAL_SIMULATED_HISTORY_MAX];
static int simulated_sms_send_result_count = 0;
static int simulated_sms_send_result_index = 0;
static modem_incoming_sms_t simulated_inbox[MODEM_HAL_SIMULATED_INBOX_MAX];
static int simulated_inbox_count = 0;
static int simulated_next_sms_index = 1;
static int simulated_deleted_sms_count = 0;
static int simulated_sms_receive_init_count = 0;
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

static modem_call_status_t parse_clcc_status(const char *response)
{
    const char *line;
    int id;
    int dir;
    int stat;
    int mode;
    int mpty;

    if (response == NULL || strstr(response, "+CLCC:") == NULL)
    {
        return MODEM_CALL_STATUS_ENDED;
    }

    line = strstr(response, "+CLCC:");
    if (sscanf(line, "+CLCC: %d,%d,%d,%d,%d", &id, &dir, &stat, &mode, &mpty) != 5)
    {
        return MODEM_CALL_STATUS_FAILED;
    }

    switch (stat)
    {
    case 0:
        return MODEM_CALL_STATUS_ACTIVE;
    case 2:
        return MODEM_CALL_STATUS_DIALING;
    case 3:
        return MODEM_CALL_STATUS_RINGING;
    case 6:
        return MODEM_CALL_STATUS_ENDED;
    default:
        return MODEM_CALL_STATUS_FAILED;
    }
}
#endif

static void trim_sms_text(char *text)
{
    size_t length;

    if (text == NULL)
    {
        return;
    }

    while (*text == '\r' || *text == '\n' || *text == ' ' || *text == '\t')
    {
        memmove(text, text + 1, strlen(text));
    }

    length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == '\r' ||
            text[length - 1] == '\n' ||
            text[length - 1] == ' ' ||
            text[length - 1] == '\t'))
    {
        text[length - 1] = '\0';
        length--;
    }
}

static int parse_quoted_field(const char **cursor, char *out, size_t out_size)
{
    const char *start;
    const char *end;
    size_t length;

    if (cursor == NULL || *cursor == NULL || out == NULL || out_size == 0)
    {
        return -1;
    }

    start = strchr(*cursor, '"');
    if (start == NULL)
    {
        return -1;
    }
    start++;
    end = strchr(start, '"');
    if (end == NULL)
    {
        return -1;
    }

    length = (size_t)(end - start);
    if (length >= out_size)
    {
        length = out_size - 1;
    }

    memcpy(out, start, length);
    out[length] = '\0';
    *cursor = end + 1;
    return 0;
}

static int parse_cmgl_response(const char *response, modem_incoming_sms_t *sms)
{
    const char *line;
    const char *cursor;
    const char *text_start;
    const char *text_end;
    char ignored[MODEM_SMS_TEXT_MAX];
    int index;
    size_t text_length;

    if (response == NULL || sms == NULL)
    {
        return -1;
    }

    line = strstr(response, "+CMGL:");
    if (line == NULL)
    {
        return 0;
    }

    if (sscanf(line, "+CMGL: %d", &index) != 1)
    {
        return -1;
    }

    cursor = line;
    if (parse_quoted_field(&cursor, ignored, sizeof(ignored)) != 0)
    {
        return -1;
    }
    if (parse_quoted_field(&cursor, sms->sender, sizeof(sms->sender)) != 0)
    {
        return -1;
    }

    text_start = strstr(cursor, "\r\n");
    if (text_start == NULL)
    {
        return -1;
    }
    text_start += 2;

    text_end = strstr(text_start, "\r\n+CMGL:");
    if (text_end == NULL)
    {
        text_end = strstr(text_start, "\r\n\r\nOK");
    }
    if (text_end == NULL)
    {
        text_end = strstr(text_start, "\r\nOK");
    }
    if (text_end == NULL)
    {
        return -1;
    }

    text_length = (size_t)(text_end - text_start);
    if (text_length >= sizeof(sms->text))
    {
        text_length = sizeof(sms->text) - 1;
    }

    sms->index = index;
    memcpy(sms->text, text_start, text_length);
    sms->text[text_length] = '\0';
    trim_sms_text(sms->text);
    return 1;
}

int modem_sms_receive_init(void)
{
    int result = 0;

    pthread_mutex_lock(&modem_hal_mutex);
#ifdef SIMULATE_MODEM
    simulated_sms_receive_init_count++;
    printf("Modem HAL: would initialize SMS receiving with AT+CMGF=1, AT+CPMS=\"SM\",\"SM\",\"SM\", AT+CNMI=2,1,0,0,0\n");
#else
    /*
     * Use SIM-card storage for v1 because it is the standard portable SMS
     * store across SIMCom variants. Runtime deletes every processed message,
     * including rejected commands, so this store should not fill during normal
     * operation.
     */
    char response[MODEM_HAL_RESPONSE_SIZE];

    if (sim_modem_send_at("AT+CMGF=1", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0 ||
        strstr(response, "OK") == NULL)
    {
        result = -1;
        goto done;
    }
    if (sim_modem_send_at("AT+CPMS=\"SM\",\"SM\",\"SM\"", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0 ||
        strstr(response, "OK") == NULL)
    {
        result = -1;
        goto done;
    }
    if (sim_modem_send_at("AT+CNMI=2,1,0,0,0", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0 ||
        strstr(response, "OK") == NULL)
    {
        result = -1;
    }
done:
#endif
    pthread_mutex_unlock(&modem_hal_mutex);
    return result;
}

int modem_check_incoming_sms(modem_incoming_sms_t *sms)
{
    int result = 0;

    if (sms == NULL)
    {
        return -1;
    }

    memset(sms, 0, sizeof(*sms));
    pthread_mutex_lock(&modem_hal_mutex);
#ifdef SIMULATE_MODEM
    if (simulated_inbox_count == 0)
    {
        result = 0;
    }
    else
    {
        int i;

        *sms = simulated_inbox[0];
        for (i = 1; i < simulated_inbox_count; i++)
        {
            simulated_inbox[i - 1] = simulated_inbox[i];
        }
        simulated_inbox_count--;
        result = 1;
        printf("Modem HAL: simulated incoming SMS index=%d from %s: %s\n",
               sms->index,
               sms->sender,
               sms->text);
    }
#else
    {
        char response[MODEM_HAL_RESPONSE_SIZE];

        if (sim_modem_send_at("AT+CMGL=\"REC UNREAD\"",
                              response,
                              sizeof(response),
                              MODEM_HAL_SMS_READ_TIMEOUT_MS) != 0)
        {
            result = -1;
        }
        else
        {
            result = parse_cmgl_response(response, sms);
            if (result == 1)
            {
                printf("Modem HAL: received SMS index=%d from %s\n", sms->index, sms->sender);
            }
        }
    }
#endif
    pthread_mutex_unlock(&modem_hal_mutex);
    return result;
}

int modem_delete_sms(int index)
{
    int result = 0;

    if (index < 0)
    {
        return -1;
    }

    pthread_mutex_lock(&modem_hal_mutex);
#ifdef SIMULATE_MODEM
    simulated_deleted_sms_count++;
    printf("Modem HAL: would delete SMS index %d with AT+CMGD=%d\n", index, index);
#else
    {
        char command[MODEM_HAL_COMMAND_SIZE];
        char response[MODEM_HAL_RESPONSE_SIZE];

        snprintf(command, sizeof(command), "AT+CMGD=%d", index);
        if (sim_modem_send_at(command, response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0 ||
            strstr(response, "OK") == NULL)
        {
            result = -1;
        }
    }
#endif
    pthread_mutex_unlock(&modem_hal_mutex);
    return result;
}

int modem_send_sms(const char *number, const char *message)
{
    if (number == NULL || number[0] == '\0' || message == NULL || message[0] == '\0')
    {
        printf("Modem HAL: SMS number and message are required\n");
        return -1;
    }

#ifdef SIMULATE_MODEM
    int simulated_result = 0;

    pthread_mutex_lock(&modem_hal_mutex);
    if (simulated_sms_send_result_index < simulated_sms_send_result_count)
    {
        simulated_result = simulated_sms_send_results[simulated_sms_send_result_index++];
    }
    if (simulated_result != 0)
    {
        printf("Modem HAL: simulated SMS send failed to %s: %s\n", number, message);
        pthread_mutex_unlock(&modem_hal_mutex);
        return -1;
    }

    printf("Modem HAL: would send SMS to %s: %s\n", number, message);
    snprintf(simulated_last_sms_number, sizeof(simulated_last_sms_number), "%s", number);
    snprintf(simulated_last_sms_message, sizeof(simulated_last_sms_message), "%s", message);
    if (simulated_sms_count < MODEM_HAL_SIMULATED_HISTORY_MAX)
    {
        snprintf(simulated_sms_numbers[simulated_sms_count],
                 sizeof(simulated_sms_numbers[simulated_sms_count]),
                 "%s",
                 number);
    }
    simulated_sms_count++;
    pthread_mutex_unlock(&modem_hal_mutex);
    return 0;
#else
    int fd;
    char cmgs_command[MODEM_HAL_COMMAND_SIZE];
    char response[MODEM_HAL_RESPONSE_SIZE];
    unsigned char payload[MODEM_HAL_SMS_PAYLOAD_SIZE];
    int payload_length;
    int result = -1;

    pthread_mutex_lock(&modem_hal_mutex);
    fd = open_modem_serial();
    if (fd < 0)
    {
        pthread_mutex_unlock(&modem_hal_mutex);
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
    pthread_mutex_unlock(&modem_hal_mutex);
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
    pthread_mutex_lock(&modem_hal_mutex);
    printf("Modem HAL: would start voice call to %s with %s\n", number, command);
    snprintf(simulated_last_call_number, sizeof(simulated_last_call_number), "%s", number);
    if (simulated_call_count < MODEM_HAL_SIMULATED_HISTORY_MAX)
    {
        snprintf(simulated_call_numbers[simulated_call_count],
                 sizeof(simulated_call_numbers[simulated_call_count]),
                 "%s",
                 number);
    }
    simulated_call_count++;
    if (simulated_call_start_result_index < simulated_call_start_result_count &&
        simulated_call_start_results[simulated_call_start_result_index++] != 0)
    {
        printf("Modem HAL: simulated voice call command failed for %s\n", number);
        voice_call_active = 0;
        voice_call_status = MODEM_CALL_STATUS_FAILED;
        pthread_mutex_unlock(&modem_hal_mutex);
        return -1;
    }
#else
    pthread_mutex_lock(&modem_hal_mutex);
    if (sim_modem_send_at(command, response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
    {
        pthread_mutex_unlock(&modem_hal_mutex);
        return -1;
    }

    if (strstr(response, "ERROR") != NULL)
    {
        printf("Modem HAL: voice call command failed: %s\n", response);
        pthread_mutex_unlock(&modem_hal_mutex);
        return -1;
    }

    printf("Modem HAL: voice call started to %s\n", number);
#endif

    voice_call_active = 1;
    voice_call_elapsed_ms = 0;
    voice_call_status_elapsed_ms = 0;
    voice_call_status = MODEM_CALL_STATUS_DIALING;
    pthread_mutex_unlock(&modem_hal_mutex);
    return 0;
}

void modem_hangup_voice_call(void)
{
#ifndef SIMULATE_MODEM
    char response[MODEM_HAL_RESPONSE_SIZE];
#endif

    if (!voice_call_active)
    {
        voice_call_status = MODEM_CALL_STATUS_IDLE;
        return;
    }

    pthread_mutex_lock(&modem_hal_mutex);
#ifdef SIMULATE_MODEM
    printf("Modem HAL: would send ATH to end current call\n");
    simulated_hangup_count++;
#else
    if (sim_modem_send_at("ATH", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
    {
        printf("Modem HAL: warning: ATH failed while ending call\n");
    }
#endif

    voice_call_active = 0;
    voice_call_elapsed_ms = 0;
    voice_call_status_elapsed_ms = 0;
    voice_call_status = MODEM_CALL_STATUS_ENDED;
    pthread_mutex_unlock(&modem_hal_mutex);
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

    pthread_mutex_lock(&modem_hal_mutex);
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
        voice_call_status = MODEM_CALL_STATUS_ENDED;
        pthread_mutex_unlock(&modem_hal_mutex);
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
        pthread_mutex_unlock(&modem_hal_mutex);
        return;
    }
    voice_call_status_elapsed_ms = 0;

#ifdef SIMULATE_MODEM
    printf("Modem HAL: would poll voice call status with AT+CLCC\n");
    if (simulated_status_sequence_index < simulated_status_sequence_count)
    {
        voice_call_status = simulated_status_sequence[simulated_status_sequence_index];
        simulated_status_sequence_index++;
    }
    else
    {
        voice_call_status = MODEM_CALL_STATUS_RINGING;
    }

    if (voice_call_status == MODEM_CALL_STATUS_ENDED ||
        voice_call_status == MODEM_CALL_STATUS_FAILED)
    {
        voice_call_active = 0;
        voice_call_elapsed_ms = 0;
        voice_call_status_elapsed_ms = 0;
    }
#else
    if (sim_modem_send_at("AT+CLCC", response, sizeof(response), MODEM_HAL_SHORT_TIMEOUT_MS) != 0)
    {
        voice_call_status = MODEM_CALL_STATUS_FAILED;
        pthread_mutex_unlock(&modem_hal_mutex);
        return;
    }

    voice_call_status = parse_clcc_status(response);
    if (voice_call_status == MODEM_CALL_STATUS_ENDED ||
        voice_call_status == MODEM_CALL_STATUS_FAILED)
    {
        printf("Modem HAL: voice call no longer active\n");
        voice_call_active = 0;
        voice_call_elapsed_ms = 0;
        voice_call_status_elapsed_ms = 0;
    }
#endif
    pthread_mutex_unlock(&modem_hal_mutex);
}

int modem_voice_call_is_active(void)
{
    return voice_call_active;
}

modem_call_status_t modem_get_voice_call_status(void)
{
    return voice_call_status;
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

const char *modem_get_simulated_sms_number_at(int index)
{
    if (index < 0 || index >= simulated_sms_count || index >= MODEM_HAL_SIMULATED_HISTORY_MAX)
    {
        return "";
    }

    return simulated_sms_numbers[index];
}

const char *modem_get_simulated_call_number_at(int index)
{
    if (index < 0 || index >= simulated_call_count || index >= MODEM_HAL_SIMULATED_HISTORY_MAX)
    {
        return "";
    }

    return simulated_call_numbers[index];
}

void modem_set_simulated_call_status_sequence(const modem_call_status_t *statuses, int count)
{
    int i;

    simulated_status_sequence_count = 0;
    simulated_status_sequence_index = 0;

    if (statuses == NULL || count <= 0)
    {
        return;
    }

    if (count > MODEM_HAL_SIMULATED_STATUS_MAX)
    {
        count = MODEM_HAL_SIMULATED_STATUS_MAX;
    }

    for (i = 0; i < count; i++)
    {
        simulated_status_sequence[i] = statuses[i];
    }
    simulated_status_sequence_count = count;
}

void modem_set_simulated_call_start_results(const int *results, int count)
{
    int i;

    simulated_call_start_result_count = 0;
    simulated_call_start_result_index = 0;

    if (results == NULL || count <= 0)
    {
        return;
    }

    if (count > MODEM_HAL_SIMULATED_HISTORY_MAX)
    {
        count = MODEM_HAL_SIMULATED_HISTORY_MAX;
    }

    for (i = 0; i < count; i++)
    {
        simulated_call_start_results[i] = results[i];
    }
    simulated_call_start_result_count = count;
}

void modem_set_simulated_sms_send_results(const int *results, int count)
{
    int i;

    simulated_sms_send_result_count = 0;
    simulated_sms_send_result_index = 0;

    if (results == NULL || count <= 0)
    {
        return;
    }

    if (count > MODEM_HAL_SIMULATED_HISTORY_MAX)
    {
        count = MODEM_HAL_SIMULATED_HISTORY_MAX;
    }

    for (i = 0; i < count; i++)
    {
        simulated_sms_send_results[i] = results[i];
    }
    simulated_sms_send_result_count = count;
}

void modem_simulate_incoming_sms(const char *sender, const char *text)
{
    modem_incoming_sms_t *sms;

    if (sender == NULL || text == NULL)
    {
        return;
    }

    if (simulated_inbox_count >= MODEM_HAL_SIMULATED_INBOX_MAX)
    {
        printf("Modem HAL: simulated SMS inbox full, dropping message from %s\n", sender);
        return;
    }

    sms = &simulated_inbox[simulated_inbox_count++];
    sms->index = simulated_next_sms_index++;
    snprintf(sms->sender, sizeof(sms->sender), "%s", sender);
    snprintf(sms->text, sizeof(sms->text), "%s", text);
}

int modem_get_simulated_deleted_sms_count(void)
{
    return simulated_deleted_sms_count;
}

int modem_get_simulated_sms_receive_init_count(void)
{
    return simulated_sms_receive_init_count;
}

void modem_reset_simulated_state(void)
{
    int i;

    simulated_sms_count = 0;
    simulated_call_count = 0;
    simulated_hangup_count = 0;
    simulated_last_sms_number[0] = '\0';
    simulated_last_sms_message[0] = '\0';
    simulated_last_call_number[0] = '\0';
    for (i = 0; i < MODEM_HAL_SIMULATED_HISTORY_MAX; i++)
    {
        simulated_sms_numbers[i][0] = '\0';
        simulated_call_numbers[i][0] = '\0';
        simulated_sms_send_results[i] = 0;
        simulated_call_start_results[i] = 0;
    }
    simulated_status_sequence_count = 0;
    simulated_status_sequence_index = 0;
    simulated_call_start_result_count = 0;
    simulated_call_start_result_index = 0;
    simulated_sms_send_result_count = 0;
    simulated_sms_send_result_index = 0;
    simulated_inbox_count = 0;
    simulated_next_sms_index = 1;
    simulated_deleted_sms_count = 0;
    simulated_sms_receive_init_count = 0;
    for (i = 0; i < MODEM_HAL_SIMULATED_INBOX_MAX; i++)
    {
        simulated_inbox[i].index = 0;
        simulated_inbox[i].sender[0] = '\0';
        simulated_inbox[i].text[0] = '\0';
    }
    voice_call_active = 0;
    voice_call_elapsed_ms = 0;
    voice_call_status_elapsed_ms = 0;
    voice_call_status = MODEM_CALL_STATUS_IDLE;
}
#endif
