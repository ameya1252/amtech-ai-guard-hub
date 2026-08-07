#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define RAW_AT_DEVICE "/dev/ttyS5"
#define RAW_AT_BAUD B115200
#define RAW_AT_ATTEMPTS 5
#define RAW_AT_TIMEOUT_MS 3000
#define RAW_AT_RESPONSE_SIZE 512

static int configure_serial(int fd)
{
    struct termios options;

    if (tcgetattr(fd, &options) != 0)
    {
        printf("tcgetattr failed: %s\n", strerror(errno));
        return -1;
    }

    cfmakeraw(&options);
    cfsetispeed(&options, RAW_AT_BAUD);
    cfsetospeed(&options, RAW_AT_BAUD);

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

static void print_visible_bytes(const unsigned char *buffer, size_t length)
{
    size_t i;

    printf("received %lu byte(s):\n", (unsigned long)length);
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

static int run_attempt(int attempt)
{
    static const char command[] = "AT\r\n";
    unsigned char response[RAW_AT_RESPONSE_SIZE];
    size_t response_length = 0;
    int fd;
    int remaining_ms = RAW_AT_TIMEOUT_MS;

    printf("Attempt %d: opening %s\n", attempt, RAW_AT_DEVICE);

    fd = open(RAW_AT_DEVICE, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        printf("open failed: %s\n", strerror(errno));
        return -1;
    }

    if (configure_serial(fd) != 0)
    {
        close(fd);
        return -1;
    }

    printf("Attempt %d: sending AT\\r\\n\n", attempt);
    if (write(fd, command, sizeof(command) - 1) != (ssize_t)(sizeof(command) - 1))
    {
        printf("write failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    while (remaining_ms >= 0 && response_length < sizeof(response))
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
            printf("poll failed: %s\n", strerror(errno));
            close(fd);
            return -1;
        }

        if (ready > 0 && (pfd.revents & POLLIN))
        {
            ssize_t bytes_read = read(fd,
                                      response + response_length,
                                      sizeof(response) - response_length);
            if (bytes_read < 0)
            {
                if (errno == EAGAIN || errno == EINTR)
                {
                    continue;
                }
                printf("read failed: %s\n", strerror(errno));
                close(fd);
                return -1;
            }

            if (bytes_read > 0)
            {
                response_length += (size_t)bytes_read;
            }
        }

        if (remaining_ms == 0)
        {
            break;
        }
        remaining_ms -= wait_ms;
    }

    if (response_length == 0)
    {
        printf("Attempt %d: no response received within %d ms\n", attempt, RAW_AT_TIMEOUT_MS);
    }
    else
    {
        printf("Attempt %d: ", attempt);
        print_visible_bytes(response, response_length);
    }

    close(fd);
    return response_length > 0 ? 0 : 1;
}

int main(void)
{
    int attempt;
    int responses = 0;

    for (attempt = 1; attempt <= RAW_AT_ATTEMPTS; attempt++)
    {
        int result = run_attempt(attempt);

        if (result == 0)
        {
            responses++;
        }

        if (attempt < RAW_AT_ATTEMPTS)
        {
            sleep(1);
        }
    }

    printf("Raw AT test complete: %d/%d attempt(s) received data\n", responses, RAW_AT_ATTEMPTS);
    return responses > 0 ? 0 : 1;
}
