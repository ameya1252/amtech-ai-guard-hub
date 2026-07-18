#include "sensor_input.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef SIMULATE_GPIO
#include <unistd.h>
#endif

#define GPIO_PATH_MAX 128

#ifdef SIMULATE_GPIO
int sensor_input_simulated_state = 0;
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
    int normalized_value = sensor_input_simulated_state ? 1 : 0;
    printf("Sensor GPIO %d read %d\n", pin, normalized_value);
    return normalized_value;
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

    return value == '0' ? 0 : 1;
#endif
}
