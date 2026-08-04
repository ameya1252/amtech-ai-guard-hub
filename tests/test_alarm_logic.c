#include "alarm_logic.h"
#include "gpio_control.h"

#include <stdio.h>

#define TEST_SIREN_GPIO_PIN 42
#define TEST_STROBE_GPIO_PIN 48

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

int main(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_shop_id("amtech-demo-shop");
    alarm_logic_set_armed(1);

#ifdef SIMULATE_GPIO
    check_int("siren initializes OFF/HIGH", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("strobe initializes OFF/HIGH", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 1);
#endif

    alarm_logic_handle_detection(0, "person", 0.75f);
    alarm_logic_end_frame();
    alarm_logic_handle_detection(0, "person", 0.80f);
    alarm_logic_end_frame();

    check_int("person detection does not trigger before 3 frames", alarm_logic_is_triggered(), 0);

    alarm_logic_handle_detection(0, "person", 0.90f);
    alarm_logic_end_frame();

    check_int("person detection triggers after 3 frames", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("siren turns ON/LOW when alarm triggers", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("strobe turns ON/LOW when alarm triggers", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_tick(AMTECH_SIREN_DURATION_MS - 1);

    check_int("alarm remains triggered before siren timeout", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("siren remains ON/LOW before timeout", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("strobe remains ON/LOW before siren timeout", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_tick(2);

    check_int("alarm remains triggered after siren timeout", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("siren turns OFF/HIGH after timeout", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("strobe remains ON/LOW after siren timeout", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_reset();

    check_int("alarm reset clears triggered state", alarm_logic_is_triggered(), 0);
#ifdef SIMULATE_GPIO
    check_int("siren turns OFF/HIGH on reset", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("strobe turns OFF/HIGH on reset", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 1);
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_detection(0, "person", 0.95f);
    alarm_logic_end_frame();

    check_int("person detection while disarmed does not trigger", alarm_logic_is_triggered(), 0);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_shutter_sensor(1);

    check_int("shutter sensor while disarmed does not trigger", alarm_logic_is_triggered(), 0);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_sensor(1);

    check_int("shutter sensor while armed triggers immediately", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_dual(SHUTTER_CLOSED);

    check_int("dual shutter closed does not trigger", alarm_logic_is_triggered(), 0);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_shutter_dual(SHUTTER_OPEN);

    check_int("dual shutter open while disarmed does not trigger", alarm_logic_is_triggered(), 0);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_dual(SHUTTER_OPEN);

    check_int("dual shutter open while armed triggers", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_dual_named(SHUTTER_OPEN, "shutter-2", "shutter-2");

    check_int("dual shutter-2 open while armed triggers", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_shutter_dual(SHUTTER_TAMPER);

    check_int("dual shutter tamper while disarmed triggers", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_dual(SHUTTER_TAMPER);

    check_int("dual shutter tamper while armed triggers", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_shutter_dual_named(SHUTTER_TAMPER, "shutter-2", "shutter-2");

    check_int("dual shutter-2 tamper while disarmed triggers", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_dual(SHUTTER_FAULT);

    check_int("dual shutter fault logs without triggering", alarm_logic_is_triggered(), 0);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_panic(1);

    check_int("panic button while disarmed triggers immediately", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_panic(1);

    check_int("panic button while armed triggers immediately", alarm_logic_is_triggered(), 1);

    if (failures == 0)
    {
        printf("PASS: alarm logic behaved as expected\n");
        return 0;
    }

    printf("FAIL: alarm logic had %d failure(s)\n", failures);
    return 1;
}
