#include "alarm_logic.h"
#include "camera_detection.h"
#include "gpio_control.h"

#include <stdio.h>
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

static void check_float_text(const char *label, float actual, const char *expected)
{
    char value[32];
    int matches;

    snprintf(value, sizeof(value), "%.3f", actual);
    matches = strcmp(value, expected) == 0;

    printf("%s: got %s, expected %s: %s\n",
           label,
           value,
           expected,
           matches ? "PASS" : "FAIL");
    if (!matches)
    {
        failures++;
    }
}

static void apply_camera_frame_to_alarm(const camera_detection_result_t *result)
{
    if (result->person_detected)
    {
        alarm_logic_handle_detection(0, "person", result->max_confidence);
    }
    alarm_logic_end_frame();
}

int main(void)
{
    camera_detection_result_t result;

    camera_detection_set_simulated_result(1, 0.82f);
    check_int("simulated camera run",
              camera_detection_run_once("rtsp://example/stream1", &result),
              0);
    check_int("simulated camera person detected", result.person_detected, 1);
    check_float_text("simulated camera confidence", result.max_confidence, "0.820");

    gpio_reset_simulated_values();
    alarm_logic_init(42);
    alarm_logic_set_armed(1);

    apply_camera_frame_to_alarm(&result);
    check_int("first camera person frame does not trigger yet", alarm_logic_is_triggered(), 0);

    apply_camera_frame_to_alarm(&result);
    check_int("second camera person frame triggers alarm", alarm_logic_is_triggered(), 1);
    check_int("camera detection turns siren ON/LOW", gpio_get_simulated_value(42), 0);

    if (failures == 0)
    {
        printf("PASS: simulated camera detection behaved as expected\n");
        return 0;
    }

    printf("FAIL: simulated camera detection had %d failure(s)\n", failures);
    return 1;
}
