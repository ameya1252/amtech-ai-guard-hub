#include "alarm_logic.h"
#include "config.h"
#include "gpio_control.h"
#include "runtime_loop.h"
#include "schedule.h"
#include "sensor_input.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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
#define AMTECH_SHUTTER2_NC_GPIO_PIN 41
#define AMTECH_SHUTTER2_NO_GPIO_PIN 72
#define AMTECH_PANIC_GPIO_PIN 32
#define AMTECH_SHOP_ID "amtech-demo-shop"
#define AMTECH_RUNTIME_TEST_ITERATIONS 10
#define AMTECH_SENSOR_DEBOUNCE_MS 25
#define AMTECH_GPIO_POLL_TIMEOUT_MS 100

typedef enum
{
    WATCH_SHUTTER1_NC = 0,
    WATCH_SHUTTER1_NO,
    WATCH_SHUTTER2_NC,
    WATCH_SHUTTER2_NO,
    WATCH_PANIC
} watched_pin_role_t;

typedef struct
{
    int pin;
    watched_pin_role_t role;
    const char *name;
    const char *edge;
    int shutter_nc_pin;
    int shutter_no_pin;
    const char *shutter_name;
    const char *shutter_event_type;
    int fd;
    long long last_event_ms;
} gpio_watch_t;

static const gpio_watch_t SHUTTER1_NC_WATCH = {
    AMTECH_SHUTTER_NC_GPIO_PIN, WATCH_SHUTTER1_NC, "shutter-1 NC", "both",
    AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN, "shutter-1", "shutter-1", -1, 0};
static const gpio_watch_t SHUTTER1_NO_WATCH = {
    AMTECH_SHUTTER_NO_GPIO_PIN, WATCH_SHUTTER1_NO, "shutter-1 NO", "both",
    AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN, "shutter-1", "shutter-1", -1, 0};
static const gpio_watch_t SHUTTER2_NC_WATCH = {
    AMTECH_SHUTTER2_NC_GPIO_PIN, WATCH_SHUTTER2_NC, "shutter-2 NC", "both",
    AMTECH_SHUTTER2_NC_GPIO_PIN, AMTECH_SHUTTER2_NO_GPIO_PIN, "shutter-2", "shutter-2", -1, 0};
static const gpio_watch_t SHUTTER2_NO_WATCH = {
    AMTECH_SHUTTER2_NO_GPIO_PIN, WATCH_SHUTTER2_NO, "shutter-2 NO", "both",
    AMTECH_SHUTTER2_NC_GPIO_PIN, AMTECH_SHUTTER2_NO_GPIO_PIN, "shutter-2", "shutter-2", -1, 0};
static const gpio_watch_t PANIC_WATCH = {
    AMTECH_PANIC_GPIO_PIN, WATCH_PANIC, "panic", "rising",
    -1, -1, NULL, NULL, -1, 0};

static int add_watch(gpio_watch_t watches[], int max_watches, int *count, const gpio_watch_t *watch)
{
    if (*count >= max_watches)
    {
        printf("Runtime: too many GPIO watches configured\n");
        return -1;
    }

    watches[*count] = *watch;
    (*count)++;
    return 0;
}

static int build_gpio_watches(const amtech_config_t *config, gpio_watch_t watches[], int max_watches)
{
    int count = 0;

    if (config == NULL)
    {
        return -1;
    }

    if (add_watch(watches, max_watches, &count, &SHUTTER1_NC_WATCH) != 0 ||
        add_watch(watches, max_watches, &count, &SHUTTER1_NO_WATCH) != 0)
    {
        return -1;
    }

    if (config->shutter_count >= 2)
    {
        if (add_watch(watches, max_watches, &count, &SHUTTER2_NC_WATCH) != 0 ||
            add_watch(watches, max_watches, &count, &SHUTTER2_NO_WATCH) != 0)
        {
            return -1;
        }
    }

    if (config->panic_enabled)
    {
        if (add_watch(watches, max_watches, &count, &PANIC_WATCH) != 0)
        {
            return -1;
        }
    }

    return count;
}

int runtime_build_watched_pins(const amtech_config_t *config,
                               runtime_watched_pin_t watched_pins[],
                               int max_watched_pins)
{
    gpio_watch_t watches[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    int count;
    int i;

    if (watched_pins == NULL || max_watched_pins <= 0)
    {
        return -1;
    }

    count = build_gpio_watches(config, watches, AMTECH_RUNTIME_MAX_WATCHED_PINS);
    if (count < 0 || count > max_watched_pins)
    {
        return -1;
    }

    for (i = 0; i < count; i++)
    {
        watched_pins[i].pin = watches[i].pin;
        watched_pins[i].name = watches[i].name;
        watched_pins[i].edge = watches[i].edge;
    }

    return count;
}

int runtime_panic_triggered_from_raw(int raw_value)
{
    return raw_value ? 1 : 0;
}

int runtime_process_configured_shutters(const amtech_config_t *config)
{
    shutter_state_t shutter_state;

    if (config == NULL)
    {
        return -1;
    }

    shutter_state = shutter_read_dual_state(AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN);
    alarm_logic_handle_shutter_dual_named(shutter_state, "shutter-1", "shutter-1");

    if (config->shutter_count >= 2)
    {
        shutter_state = shutter_read_dual_state(AMTECH_SHUTTER2_NC_GPIO_PIN, AMTECH_SHUTTER2_NO_GPIO_PIN);
        alarm_logic_handle_shutter_dual_named(shutter_state, "shutter-2", "shutter-2");
    }

    return 0;
}

static int env_force_armed_enabled(void)
{
    const char *value = getenv("AMTECH_FORCE_ARMED");

    return value != NULL &&
           (strcmp(value, "1") == 0 ||
            strcmp(value, "true") == 0 ||
            strcmp(value, "TRUE") == 0 ||
            strcmp(value, "yes") == 0 ||
            strcmp(value, "YES") == 0);
}

static int parse_force_armed_arg(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--force-armed") == 0)
        {
            return 1;
        }

        printf("Runtime: unknown argument %s\n", argv[i]);
        printf("Usage: %s [--force-armed]\n", argv[0]);
        return -1;
    }

    return env_force_armed_enabled();
}

static void print_force_armed_warning(void)
{
    printf("WARNING: FORCE-ARMED TEST MODE - NOT FOR PRODUCTION USE\n");
    printf("WARNING: Schedule checks are disabled and the system is forced ARMED\n");
}

static const char *runtime_config_path(void)
{
    const char *path = getenv("AMTECH_CONFIG_PATH");

    if (path != NULL && path[0] != '\0')
    {
        return path;
    }

    return AMTECH_DEFAULT_CONFIG_PATH;
}

#ifndef SIMULATE_GPIO
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

static void update_schedule_from_realtime(void)
{
    time_t now_seconds;
    struct tm now_local;

    now_seconds = time(NULL);
    if (now_seconds == (time_t)-1)
    {
        printf("Runtime: time failed: %s\n", strerror(errno));
        return;
    }

    if (localtime_r(&now_seconds, &now_local) == NULL)
    {
        printf("Runtime: localtime_r failed\n");
        return;
    }

    alarm_logic_set_armed(schedule_should_be_armed(now_local.tm_hour, now_local.tm_min));
}

static void tick_schedule_elapsed_seconds(long long *last_tick_ms)
{
    long long now_ms = monotonic_ms();

    if (*last_tick_ms == 0)
    {
        *last_tick_ms = now_ms;
        return;
    }

    while (now_ms - *last_tick_ms >= 1000)
    {
        schedule_tick();
        *last_tick_ms += 1000;
    }
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

    printf("Runtime: %s GPIO %d edge event raw=%d\n", watch->name, watch->pin, raw_value);

    if (watch->role == WATCH_PANIC)
    {
        panic_triggered = runtime_panic_triggered_from_raw(raw_value);
        alarm_logic_handle_panic(panic_triggered);
        return;
    }

    shutter_state = shutter_read_dual_state(watch->shutter_nc_pin, watch->shutter_no_pin);
    alarm_logic_handle_shutter_dual_named(shutter_state,
                                          watch->shutter_name,
                                          watch->shutter_event_type);
}

static int run_interrupt_loop(int force_armed, const amtech_config_t *config)
{
    /*
     * Shutter uses both edges because wire-cut tamper from the closed state is
     * NC LOW -> HIGH with NO already HIGH, so falling-only would not wake us.
     */
    gpio_watch_t watches[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    struct pollfd poll_fds[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    long long last_schedule_tick_ms = 0;
    long long last_alarm_tick_ms = 0;
    int watch_count;
    int i;

    watch_count = build_gpio_watches(config, watches, AMTECH_RUNTIME_MAX_WATCHED_PINS);
    if (watch_count < 0)
    {
        return -1;
    }

    for (i = 0; i < watch_count; i++)
    {
        if (setup_gpio_watch(&watches[i]) != 0)
        {
            close_gpio_watches(watches, watch_count);
            return -1;
        }

        poll_fds[i].fd = watches[i].fd;
        poll_fds[i].events = POLLPRI | POLLERR;
        poll_fds[i].revents = 0;
    }

    for (;;)
    {
        int ready;
        long long now_ms;

        now_ms = monotonic_ms();
        if (last_alarm_tick_ms != 0 && now_ms >= last_alarm_tick_ms)
        {
            alarm_logic_tick((unsigned int)(now_ms - last_alarm_tick_ms));
        }
        last_alarm_tick_ms = now_ms;

        if (!force_armed)
        {
            tick_schedule_elapsed_seconds(&last_schedule_tick_ms);
            update_schedule_from_realtime();
        }

        ready = poll(poll_fds, watch_count, AMTECH_GPIO_POLL_TIMEOUT_MS);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("Runtime: poll failed: %s\n", strerror(errno));
            close_gpio_watches(watches, watch_count);
            return -1;
        }

        if (ready == 0)
        {
            continue;
        }

        for (i = 0; i < watch_count; i++)
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

#ifdef SIMULATE_GPIO
static void runtime_iteration(int iteration, int force_armed, const amtech_config_t *config)
{
    int panic_triggered;
    int panic_raw_value;
    int should_be_armed;

    printf("Runtime: iteration %d\n", iteration);

    if (!force_armed)
    {
        schedule_tick();

        should_be_armed = schedule_should_be_armed(23, 30);
        alarm_logic_set_armed(should_be_armed);
    }

    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NC_GPIO_PIN, iteration == 4 ? 1 : 0);
    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NO_GPIO_PIN, iteration == 4 ? 0 : 1);
    if (config->shutter_count >= 2)
    {
        sensor_input_set_simulated_raw_value(AMTECH_SHUTTER2_NC_GPIO_PIN, iteration == 5 ? 1 : 0);
        sensor_input_set_simulated_raw_value(AMTECH_SHUTTER2_NO_GPIO_PIN, iteration == 5 ? 0 : 1);
    }

    runtime_process_configured_shutters(config);

    if (config->panic_enabled)
    {
        panic_raw_value = iteration == 6 ? 1 : 0;
        sensor_input_set_simulated_raw_value(AMTECH_PANIC_GPIO_PIN, panic_raw_value);
        printf("Runtime: panic GPIO %d raw %d\n", AMTECH_PANIC_GPIO_PIN, panic_raw_value);
        panic_triggered = runtime_panic_triggered_from_raw(panic_raw_value);
        alarm_logic_handle_panic(panic_triggered);
    }

    /*
     * TODO: Capture camera frame, run RKNN YOLO inference, pass each detection to
     * alarm_logic_handle_detection(), then call alarm_logic_end_frame().
     */
    alarm_logic_end_frame();
    alarm_logic_tick(1000);
}
#endif

#ifndef AMTECH_RUNTIME_LOOP_NO_MAIN
int main(int argc, char **argv)
{
    int force_armed;
    amtech_config_t config;
#ifdef SIMULATE_GPIO
    int i;
#endif

    force_armed = parse_force_armed_arg(argc, argv);
    if (force_armed < 0)
    {
        return 1;
    }

    if (amtech_config_load(runtime_config_path(), &config) != 0)
    {
        return 1;
    }

    alarm_logic_init(AMTECH_ALARM_GPIO_PIN);
    alarm_logic_set_shop_id(AMTECH_SHOP_ID);
    if (force_armed)
    {
        /*
         * Testing-only override for bench/bring-up validation. Do not use this
         * in production because it deliberately bypasses the real arm schedule.
         */
        print_force_armed_warning();
        alarm_logic_set_armed(1);
    }
    else
    {
        alarm_logic_set_armed(0);
    }
#ifdef SIMULATE_GPIO
    sensor_input_init(AMTECH_SHUTTER_NC_GPIO_PIN);
    sensor_input_init(AMTECH_SHUTTER_NO_GPIO_PIN);
    if (config.shutter_count >= 2)
    {
        sensor_input_init(AMTECH_SHUTTER2_NC_GPIO_PIN);
        sensor_input_init(AMTECH_SHUTTER2_NO_GPIO_PIN);
    }
    if (config.panic_enabled)
    {
        sensor_input_init(AMTECH_PANIC_GPIO_PIN);
    }
#endif
    schedule_set_armed_window(23, 0, 6, 0);

#ifdef SIMULATE_GPIO
    for (i = 0; i < AMTECH_RUNTIME_TEST_ITERATIONS; i++)
    {
        runtime_iteration(i, force_armed, &config);
    }

    return 0;
#else
    return run_interrupt_loop(force_armed, &config);
#endif
}
#endif
