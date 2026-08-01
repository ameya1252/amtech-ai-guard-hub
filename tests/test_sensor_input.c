#include "sensor_input.h"

#include <stdio.h>

#define TEST_SENSOR_PIN 17
#define TEST_SHUTTER_NC_PIN 33
#define TEST_SHUTTER_NO_PIN 40

static int check_read(const char *label, int expected)
{
    int actual = sensor_input_read(TEST_SENSOR_PIN);
    const char *state = actual ? "triggered" : "not triggered";
    const char *result = actual == expected ? "PASS" : "FAIL";

    printf("%s: sensor is %s, expected %s: %s\n",
           label,
           state,
           expected ? "triggered" : "not triggered",
           result);

    return actual == expected ? 0 : 1;
}

static int check_shutter_state(const char *label, int nc_raw, int no_raw, shutter_state_t expected)
{
    shutter_state_t actual;
    const char *result;

    sensor_input_set_simulated_raw_value(TEST_SHUTTER_NC_PIN, nc_raw);
    sensor_input_set_simulated_raw_value(TEST_SHUTTER_NO_PIN, no_raw);

    actual = shutter_read_dual_state(TEST_SHUTTER_NC_PIN, TEST_SHUTTER_NO_PIN);
    result = actual == expected ? "PASS" : "FAIL";

    printf("%s: NC raw %d, NO raw %d -> %s, expected %s: %s\n",
           label,
           nc_raw,
           no_raw,
           shutter_state_to_string(actual),
           shutter_state_to_string(expected),
           result);

    return actual == expected ? 0 : 1;
}

int main(void)
{
    int failures = 0;

    if (sensor_input_init(TEST_SENSOR_PIN) != 0)
    {
        printf("FAIL: sensor_input_init failed\n");
        return 1;
    }

    sensor_input_simulated_state = 1;
    failures += check_read("Initial HIGH read", 0);

    sensor_input_simulated_state = 0;
    failures += check_read("Triggered LOW read", 1);

    failures += check_shutter_state("Shutter closed", 0, 1, SHUTTER_CLOSED);
    failures += check_shutter_state("Shutter open", 1, 0, SHUTTER_OPEN);
    failures += check_shutter_state("Shutter wire cut / open circuit", 1, 1, SHUTTER_TAMPER);
    failures += check_shutter_state("Shutter short circuit", 0, 0, SHUTTER_FAULT);

    if (failures == 0)
    {
        printf("PASS: sensor input simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: sensor input simulation had %d failure(s)\n", failures);
    return 1;
}
