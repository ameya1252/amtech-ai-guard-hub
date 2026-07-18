#include "sensor_input.h"

#include <stdio.h>

#define TEST_SENSOR_PIN 17

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

int main(void)
{
    int failures = 0;

    if (sensor_input_init(TEST_SENSOR_PIN) != 0)
    {
        printf("FAIL: sensor_input_init failed\n");
        return 1;
    }

    sensor_input_simulated_state = 0;
    failures += check_read("Initial read", 0);

    sensor_input_simulated_state = 1;
    failures += check_read("Triggered read", 1);

    if (failures == 0)
    {
        printf("PASS: sensor input simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: sensor input simulation had %d failure(s)\n", failures);
    return 1;
}
