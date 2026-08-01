#include "config.h"
#include "runtime_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check_int(const char *label, int actual, int expected)
{
    const char *result = actual == expected ? "PASS" : "FAIL";

    printf("%s: got %d, expected %d: %s\n", label, actual, expected, result);
    if (actual != expected)
    {
        failures++;
    }
}

static int contains_pin(runtime_watched_pin_t pins[], int count, int pin)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (pins[i].pin == pin)
        {
            return 1;
        }
    }

    return 0;
}

static void check_pin_present(runtime_watched_pin_t pins[], int count, int pin, int expected)
{
    char label[64];

    snprintf(label, sizeof(label), "pin %d watched", pin);
    check_int(label, contains_pin(pins, count, pin), expected);
}

int main(void)
{
    amtech_config_t config;
    runtime_watched_pin_t pins[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    int count;
    const char *missing_config_path = "/tmp/amtech_missing_config_for_test.txt";
    const char *two_shutter_config_path = "/tmp/amtech_two_shutter_config_for_test.txt";
    FILE *fp;

    remove(missing_config_path);
    check_int("missing config load", amtech_config_load(missing_config_path, &config), 0);
    check_int("missing config default shutter count", config.shutter_count, 1);
    check_int("missing config default panic enabled", config.panic_enabled, 1);

    fp = fopen(two_shutter_config_path, "w");
    if (fp == NULL)
    {
        printf("FAIL: could not write test config file\n");
        return 1;
    }
    fprintf(fp, "SHUTTER_COUNT=2\nPANIC_ENABLED=1\n");
    fclose(fp);

    check_int("two-shutter config load", amtech_config_load(two_shutter_config_path, &config), 0);
    check_int("two-shutter config shutter count", config.shutter_count, 2);
    check_int("two-shutter config panic enabled", config.panic_enabled, 1);

    amtech_config_set_defaults(&config);
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("default shutter count", config.shutter_count, 1);
    check_int("default watched pin count", count, 3);
    check_pin_present(pins, count, 33, 1);
    check_pin_present(pins, count, 40, 1);
    check_pin_present(pins, count, 41, 0);
    check_pin_present(pins, count, 72, 0);
    check_pin_present(pins, count, 32, 1);

    config.shutter_count = 1;
    config.panic_enabled = 1;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("SHUTTER_COUNT=1 watched pin count", count, 3);
    check_pin_present(pins, count, 33, 1);
    check_pin_present(pins, count, 40, 1);
    check_pin_present(pins, count, 41, 0);
    check_pin_present(pins, count, 72, 0);
    check_pin_present(pins, count, 32, 1);

    config.shutter_count = 2;
    config.panic_enabled = 1;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("SHUTTER_COUNT=2 watched pin count", count, 5);
    check_pin_present(pins, count, 33, 1);
    check_pin_present(pins, count, 40, 1);
    check_pin_present(pins, count, 41, 1);
    check_pin_present(pins, count, 72, 1);
    check_pin_present(pins, count, 32, 1);

    config.shutter_count = 2;
    config.panic_enabled = 0;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("PANIC_ENABLED=0 watched pin count", count, 4);
    check_pin_present(pins, count, 32, 0);

    if (failures == 0)
    {
        remove(two_shutter_config_path);
        printf("PASS: runtime config watch selection behaved as expected\n");
        return 0;
    }

    remove(two_shutter_config_path);
    printf("FAIL: runtime config watch selection had %d failure(s)\n", failures);
    return 1;
}
