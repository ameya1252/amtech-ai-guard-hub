#include "alarm_logic.h"
#include "gpio_control.h"
#include "schedule.h"
#include "sensor_input.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef SIMULATE_GPIO
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

#define AMTECH_ALARM_GPIO_PIN 49
#define AMTECH_STROBE_GPIO_PIN 48
#define AMTECH_SHUTTER_NC_GPIO_PIN 33
#define AMTECH_SHUTTER_NO_GPIO_PIN 40
#define AMTECH_PANIC_GPIO_PIN 32
#define AMTECH_SHOP_ID "amtech-demo-shop"
#define AMTECH_RUNTIME_TEST_ITERATIONS 10
#define AMTECH_SENSOR_DEBOUNCE_MS 25

#ifndef SIMULATE_GPIO
typedef enum
{
    WATCH_SHUTTER_NC = 0,
    WATCH_SHUTTER_NO,
    WATCH_PANIC,
    WATCH_COUNT
} watched_pin_role_t;

typedef struct
{
    int pin;
    watched_pin_role_t role;
    const char *name;
    const char *edge;
    int fd;
    long long last_event_ms;
} gpio_watch_t;

static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        printf("Runtime: clock_gettime failed: %s\n", strerror(errno));
        return 0;
    }

    return ((long long)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

static int read_gpio_value_fd(int fd, int *raw_value)
{
    char value = 0;

    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        printf("Runtime: failed to seek GPIO value fd: %s\n", strerror(errno));
        return -1;
    }

    if (read(fd, &value, 1) != 1)
    {
        printf("Runtime: failed to read GPIO value fd: %s\n", strerror(errno));
        return -1;
    }

    *raw_value = value == '0' ? 0 : 1;
    return 0;
}

static int open_gpio_value_fd(int pin)
{
    char path[128];
    int fd;
    int raw_value;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("Runtime: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (read_gpio_value_fd(fd, &raw_value) != 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static int setup_gpio_watch(gpio_watch_t *watch)
{
    if (sensor_input_init(watch->pin) != 0)
    {
        return -1;
    }

    if (gpio_set_edge(watch->pin, watch->edge) != 0)
    {
        printf("Runtime: failed to configure %s edge for %s GPIO %d\n",
               watch->edge,
               watch->name,
               watch->pin);
        return -1;
    }

    watch->fd = open_gpio_value_fd(watch->pin);
    if (watch->fd < 0)
    {
        return -1;
    }

    watch->last_event_ms = 0;
    return 0;
}

static void close_gpio_watches(gpio_watch_t watches[], int count)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (watches[i].fd >= 0)
        {
            close(watches[i].fd);
            watches[i].fd = -1;
        }
    }
}

static void handle_sensor_event(gpio_watch_t *watch)
{
    long long now_ms = monotonic_ms();
    long long elapsed_ms = now_ms - watch->last_event_ms;
    int raw_value;
    int panic_triggered;
    shutter_state_t shutter_state;

    if (watch->last_event_ms != 0 && elapsed_ms >= 0 && elapsed_ms < AMTECH_SENSOR_DEBOUNCE_MS)
    {
        printf("Runtime: ignored %s GPIO %d bounce after %lld ms\n",
               watch->name,
               watch->pin,
               elapsed_ms);
        return;
    }
    watch->last_event_ms = now_ms;

    if (read_gpio_value_fd(watch->fd, &raw_value) != 0)
    {
        return;
    }

    printf("Runtime: %s GPIO %d falling-edge event raw=%d\n", watch->name, watch->pin, raw_value);

    if (watch->role == WATCH_PANIC)
    {
        panic_triggered = raw_value == 0 ? 1 : 0;
        alarm_logic_handle_panic(panic_triggered);
        return;
    }

    shutter_state = shutter_read_dual_state(AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN);
    alarm_logic_handle_shutter_dual(shutter_state);
}

static int run_interrupt_loop(void)
{
    /*
     * Shutter uses both edges because wire-cut tamper from the closed state is
     * NC LOW -> HIGH with NO already HIGH, so falling-only would not wake us.
     */
    gpio_watch_t watches[WATCH_COUNT] = {
        {AMTECH_SHUTTER_NC_GPIO_PIN, WATCH_SHUTTER_NC, "shutter NC", "both", -1, 0},
        {AMTECH_SHUTTER_NO_GPIO_PIN, WATCH_SHUTTER_NO, "shutter NO", "both", -1, 0},
        {AMTECH_PANIC_GPIO_PIN, WATCH_PANIC, "panic", "falling", -1, 0},
    };
    struct pollfd poll_fds[WATCH_COUNT];
    int i;

    for (i = 0; i < WATCH_COUNT; i++)
    {
        if (setup_gpio_watch(&watches[i]) != 0)
        {
            close_gpio_watches(watches, WATCH_COUNT);
            return -1;
        }

        poll_fds[i].fd = watches[i].fd;
        poll_fds[i].events = POLLPRI | POLLERR;
        poll_fds[i].revents = 0;
    }

    for (;;)
    {
        int ready;

        schedule_tick();
        alarm_logic_set_armed(schedule_should_be_armed(23, 30));

        ready = poll(poll_fds, WATCH_COUNT, 1000);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("Runtime: poll failed: %s\n", strerror(errno));
            close_gpio_watches(watches, WATCH_COUNT);
            return -1;
        }

        if (ready == 0)
        {
            continue;
        }

        for (i = 0; i < WATCH_COUNT; i++)
        {
            if (poll_fds[i].revents & (POLLPRI | POLLERR))
            {
                handle_sensor_event(&watches[i]);
            }
            poll_fds[i].revents = 0;
        }

        alarm_logic_end_frame();
    }
}
#endif

static void runtime_iteration(int iteration)
{
    shutter_state_t shutter_state;
    int panic_triggered;
    int should_be_armed;

    printf("Runtime: iteration %d\n", iteration);

    schedule_tick();

    should_be_armed = schedule_should_be_armed(23, 30);
    alarm_logic_set_armed(should_be_armed);

#ifdef SIMULATE_GPIO
    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NC_GPIO_PIN, iteration == 4 ? 1 : 0);
    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NO_GPIO_PIN, iteration == 4 ? 0 : 1);
#endif

    shutter_state = shutter_read_dual_state(AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN);
    alarm_logic_handle_shutter_dual(shutter_state);

#ifdef SIMULATE_GPIO
    sensor_input_simulated_state = iteration == 6 ? 0 : 1;
#endif

    panic_triggered = sensor_input_read(AMTECH_PANIC_GPIO_PIN);
    alarm_logic_handle_panic(panic_triggered);

    /*
     * TODO: Capture camera frame, run RKNN YOLO inference, pass each detection to
     * alarm_logic_handle_detection(), then call alarm_logic_end_frame().
     */
    alarm_logic_end_frame();
}

int main(void)
{
#ifdef SIMULATE_GPIO
    int i;
#endif

    alarm_logic_init(AMTECH_ALARM_GPIO_PIN);
    alarm_logic_set_shop_id(AMTECH_SHOP_ID);
    alarm_logic_set_armed(0);
#ifdef SIMULATE_GPIO
    sensor_input_init(AMTECH_SHUTTER_NC_GPIO_PIN);
    sensor_input_init(AMTECH_SHUTTER_NO_GPIO_PIN);
    sensor_input_init(AMTECH_PANIC_GPIO_PIN);
#endif
    schedule_set_armed_window(23, 0, 6, 0);

#ifdef SIMULATE_GPIO
    for (i = 0; i < AMTECH_RUNTIME_TEST_ITERATIONS; i++)
    {
        runtime_iteration(i);
    }

    return 0;
#else
    return run_interrupt_loop();
#endif
}
