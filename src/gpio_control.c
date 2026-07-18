#include "gpio_control.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifndef SIMULATE_GPIO
#include <unistd.h>
#endif

#define GPIO_PATH_MAX 128

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
        printf("GPIO: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (fprintf(fp, "%s", value) < 0)
    {
        printf("GPIO: failed to write %s to %s: %s\n", value, path, strerror(errno));
        fclose(fp);
        return -1;
    }

    if (fclose(fp) != 0)
    {
        printf("GPIO: failed to close %s: %s\n", path, strerror(errno));
        return -1;
    }

    return 0;
#endif
}

int gpio_export(int pin)
{
#ifdef SIMULATE_GPIO
    printf("GPIO %d exported\n", pin);
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

int gpio_set_output(int pin)
{
#ifdef SIMULATE_GPIO
    printf("GPIO %d direction output\n", pin);
    return 0;
#else
    char path[GPIO_PATH_MAX];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    return write_text_file(path, "out");
#endif
}

int gpio_write_value(int pin, int value)
{
    int normalized_value = value ? 1 : 0;

#ifdef SIMULATE_GPIO
    printf("GPIO %d set %d\n", pin, normalized_value);
    return 0;
#else
    char path[GPIO_PATH_MAX];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    return write_text_file(path, normalized_value ? "1" : "0");
#endif
}
