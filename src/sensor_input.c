#include "sensor_input.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef SIMULATE_GPIO
#include <unistd.h>
#endif

#define GPIO_PATH_MAX 128

#ifdef SIMULATE_GPIO
#define SIMULATED_GPIO_MAX 256
int sensor_input_simulated_state = 0;
static int simulated_raw_values[SIMULATED_GPIO_MAX];
static int simulated_raw_values_set[SIMULATED_GPIO_MAX];

void sensor_input_set_simulated_raw_value(int pin, int raw_value)
{
    if (pin < 0 || pin >= SIMULATED_GPIO_MAX)
    {
        printf("Sensor GPIO: simulated pin %d out of range\n", pin);
        return;
    }

    simulated_raw_values[pin] = raw_value ? 1 : 0;
    simulated_raw_values_set[pin] = 1;
}
#endif

static int write_text_file(const char *path, const char *value)
{
#ifdef SIMULATE_GPIO
    (void)path;
    (void)value;
    return 0;
#else
    FILE *fp = fopen(path, "w");
    if (fp == NULL)
    {
        printf("Sensor GPIO: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (fprintf(fp, "%s", value) < 0)
    {
        printf("Sensor GPIO: failed to write %s to %s: %s\n", value, path, strerror(errno));
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0)
    {
        printf("Sensor GPIO: failed to close %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
#endif
}

static int export_gpio(int pin)
{
#ifdef SIMULATE_GPIO
    printf("Sensor GPIO %d exported\n", pin);
    return 0;
#else
    char value[16];
    char gpio_dir[GPIO_PATH_MAX];

    snprintf(gpio_dir, sizeof(gpio_dir), "/sys/class/gpio/gpio%d", pin);
    if (access(gpio_dir, F_OK) == 0)
    {
        return 0;
    }

    snprintf(value, sizeof(value), "%d", pin);
    return write_text_file("/sys/class/gpio/export", value);
#endif
}

int sensor_input_init(int pin)
{
#ifdef SIMULATE_GPIO
    printf("Sensor GPIO %d direction input\n", pin);
    return export_gpio(pin);
#else
    char path[GPIO_PATH_MAX];

    if (export_gpio(pin) != 0)
    {
        return -1;
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    return write_text_file(path, "in");
#endif
}

int sensor_input_read(int pin)
{
#ifdef SIMULATE_GPIO
    int raw_value = sensor_input_simulated_state ? 1 : 0;
    if (pin >= 0 && pin < SIMULATED_GPIO_MAX && simulated_raw_values_set[pin])
    {
        raw_value = simulated_raw_values[pin];
    }
    int triggered = raw_value == 0 ? 1 : 0;
    printf("Sensor GPIO %d raw %d triggered %d\n", pin, raw_value, triggered);
    return triggered;
#else
    char path[GPIO_PATH_MAX];
    char value = 0;
    FILE *fp;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fp = fopen(path, "r");
    if (fp == NULL)
    {
        printf("Sensor GPIO: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (fread(&value, 1, 1, fp) != 1)
    {
        printf("Sensor GPIO: failed to read %s: %s\n", path, strerror(errno));
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0)
    {
        printf("Sensor GPIO: failed to close %s: %s\n", path, strerror(errno));
        return -1;
    }

    return value == '0' ? 1 : 0;
#endif
}

shutter_state_t shutter_read_dual_state(int nc_pin, int no_pin)
{
    int nc_low = sensor_input_read(nc_pin);
    int no_low = sensor_input_read(no_pin);

    if (nc_low < 0 || no_low < 0)
    {
        printf("Shutter: failed to read dual sensor pins nc=%d no=%d\n", nc_pin, no_pin);
        return SHUTTER_FAULT;
    }

    if (nc_low && !no_low)
    {
        return SHUTTER_CLOSED;
    }

    if (!nc_low && no_low)
    {
        return SHUTTER_OPEN;
    }

    if (!nc_low && !no_low)
    {
        return SHUTTER_TAMPER;
    }

    return SHUTTER_FAULT;
}

const char *shutter_state_to_string(shutter_state_t state)
{
    switch (state)
    {
    case SHUTTER_CLOSED:
        return "closed";
    case SHUTTER_OPEN:
        return "open";
    case SHUTTER_TAMPER:
        return "tamper";
    case SHUTTER_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}
