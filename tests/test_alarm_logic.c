#include "alarm_logic.h"
#include "gpio_control.h"
#include "modem_hal.h"
#include "notify_client.h"

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

static void check_panic_retrigger_and_notification_cooldown(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_NETWORK
    notify_reset_simulated_send_count();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_shop_id("amtech-demo-shop");
    alarm_logic_set_armed(0);

    alarm_logic_handle_panic(1);
    check_int("panic first trigger sets active alarm", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("panic first trigger turns siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("panic first trigger turns strobe ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif
#ifdef SIMULATE_NETWORK
    check_int("panic first trigger sends notification", notify_get_simulated_send_count(), 1);
#endif
#ifdef SIMULATE_MODEM
    check_int("panic first trigger sends SMS", modem_get_simulated_sms_count(), 1);
    check_int("panic first trigger starts voice call", modem_get_simulated_call_count(), 1);
#endif

    alarm_logic_tick(AMTECH_SIREN_DURATION_MS + 1);
#ifdef SIMULATE_GPIO
    check_int("panic first trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("panic first trigger strobe stays ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_handle_panic(1);
    check_int("panic second trigger keeps alarm active", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("panic second trigger restarts siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("panic second trigger keeps strobe ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif
#ifdef SIMULATE_NETWORK
    check_int("panic second trigger inside cooldown suppresses notification",
              notify_get_simulated_send_count(),
              1);
#endif
#ifdef SIMULATE_MODEM
    check_int("panic second trigger inside cooldown suppresses SMS",
              modem_get_simulated_sms_count(),
              1);
    check_int("panic second trigger inside cooldown suppresses voice call",
              modem_get_simulated_call_count(),
              1);
#endif

    alarm_logic_tick(AMTECH_SIREN_DURATION_MS + 1);
#ifdef SIMULATE_GPIO
    check_int("panic second trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
#endif

    alarm_logic_tick(AMTECH_NOTIFICATION_COOLDOWN_MS);
    alarm_logic_handle_panic(1);
#ifdef SIMULATE_GPIO
    check_int("panic trigger after cooldown restarts siren ON/LOW",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
#endif
#ifdef SIMULATE_NETWORK
    check_int("panic trigger after cooldown sends notification",
              notify_get_simulated_send_count(),
              2);
#endif
#ifdef SIMULATE_MODEM
    check_int("panic trigger after cooldown sends SMS",
              modem_get_simulated_sms_count(),
              2);
#endif

    alarm_logic_reset();
    alarm_logic_handle_panic(1);
#ifdef SIMULATE_NETWORK
    check_int("reset clears notification cooldown",
              notify_get_simulated_send_count(),
              3);
#endif
#ifdef SIMULATE_MODEM
    check_int("reset clears SIM alert cooldown",
              modem_get_simulated_sms_count(),
              3);
#endif
}

static void check_outputs_reactivate_when_notification_is_suppressed(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_NETWORK
    notify_reset_simulated_send_count();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_shop_id("amtech-demo-shop");
    alarm_logic_set_armed(0);

    alarm_logic_handle_panic(1);
#ifdef SIMULATE_NETWORK
    check_int("regression first panic sends notification",
              notify_get_simulated_send_count(),
              1);
#endif
#ifdef SIMULATE_MODEM
    check_int("regression first panic sends SIM alert",
              modem_get_simulated_sms_count(),
              1);
#endif

    alarm_logic_tick(AMTECH_SIREN_DURATION_MS + 1);
#ifdef SIMULATE_GPIO
    check_int("regression siren is OFF before second panic",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              1);
#endif

    alarm_logic_handle_panic(1);
#ifdef SIMULATE_GPIO
    check_int("regression siren restarts even when notification suppressed",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
    check_int("regression strobe remains ON even when notification suppressed",
              gpio_get_simulated_value(TEST_STROBE_GPIO_PIN),
              0);
#endif
#ifdef SIMULATE_NETWORK
    check_int("regression second panic suppresses notification only",
              notify_get_simulated_send_count(),
              1);
#endif
#ifdef SIMULATE_MODEM
    check_int("regression second panic suppresses SIM alert too",
              modem_get_simulated_sms_count(),
              1);
#endif
}

static void check_shutter_retrigger(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_NETWORK
    notify_reset_simulated_send_count();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);

    alarm_logic_handle_shutter_dual(SHUTTER_OPEN);
    check_int("shutter first trigger sets active alarm", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("shutter first trigger turns siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("shutter first trigger turns strobe ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_tick(AMTECH_SIREN_DURATION_MS + 1);
#ifdef SIMULATE_GPIO
    check_int("shutter first trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("shutter first trigger strobe stays ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_handle_shutter_dual(SHUTTER_OPEN);
#ifdef SIMULATE_GPIO
    check_int("shutter second trigger restarts siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("shutter second trigger keeps strobe ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif
#ifdef SIMULATE_NETWORK
    check_int("shutter second trigger inside cooldown suppresses notification",
              notify_get_simulated_send_count(),
              1);
#endif
#ifdef SIMULATE_MODEM
    check_int("shutter second trigger inside cooldown suppresses SMS",
              modem_get_simulated_sms_count(),
              1);
#endif
}

static void send_three_person_frames(void)
{
    alarm_logic_handle_detection(0, "person", 0.80f);
    alarm_logic_end_frame();
    alarm_logic_handle_detection(0, "person", 0.81f);
    alarm_logic_end_frame();
    alarm_logic_handle_detection(0, "person", 0.82f);
    alarm_logic_end_frame();
}

static void check_person_retrigger(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_NETWORK
    notify_reset_simulated_send_count();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);

    send_three_person_frames();
    check_int("person first 3-frame trigger sets active alarm", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("person first trigger turns siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
#endif

    alarm_logic_tick(AMTECH_SIREN_DURATION_MS + 1);
#ifdef SIMULATE_GPIO
    check_int("person first trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
#endif

    send_three_person_frames();
#ifdef SIMULATE_GPIO
    check_int("person second 3-frame trigger restarts siren ON/LOW",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
    check_int("person second 3-frame trigger keeps strobe ON/LOW",
              gpio_get_simulated_value(TEST_STROBE_GPIO_PIN),
              0);
#endif
#ifdef SIMULATE_NETWORK
    check_int("person second trigger inside cooldown suppresses notification",
              notify_get_simulated_send_count(),
              1);
#endif
#ifdef SIMULATE_MODEM
    check_int("person second trigger inside cooldown suppresses SMS",
              modem_get_simulated_sms_count(),
              1);
#endif
}

static void check_smoke_triggers_regardless_of_armed_state(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);
    alarm_logic_handle_smoke(1);

    check_int("smoke detector while disarmed triggers immediately", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_smoke(1);

    check_int("smoke detector while armed triggers immediately", alarm_logic_is_triggered(), 1);
}

static void check_call_state_ticks_without_blocking_alarm_flow(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_NETWORK
    notify_reset_simulated_send_count();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);

    alarm_logic_handle_smoke(1);
    check_int("smoke starts non-blocking voice call", modem_voice_call_is_active(), 1);

    alarm_logic_handle_panic(1);
    check_int("panic still handled while voice call active", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("panic reactivates siren while voice call active",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
#endif
#ifdef SIMULATE_MODEM
    check_int("panic inside cooldown does not start overlapping call",
              modem_get_simulated_call_count(),
              1);
#endif

    alarm_logic_tick(44000);
    check_int("voice call remains active before 45 second timeout", modem_voice_call_is_active(), 1);
    alarm_logic_tick(1000);
    check_int("voice call clears at 45 second timeout", modem_voice_call_is_active(), 0);
#ifdef SIMULATE_MODEM
    check_int("voice call safety hangup sent once", modem_get_simulated_hangup_count(), 1);
#endif
}

int main(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_NETWORK
    notify_reset_simulated_send_count();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
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

    check_smoke_triggers_regardless_of_armed_state();

    check_panic_retrigger_and_notification_cooldown();
    check_outputs_reactivate_when_notification_is_suppressed();
    check_shutter_retrigger();
    check_person_retrigger();
    check_call_state_ticks_without_blocking_alarm_flow();

    if (failures == 0)
    {
        printf("PASS: alarm logic behaved as expected\n");
        return 0;
    }

    printf("FAIL: alarm logic had %d failure(s)\n", failures);
    return 1;
}
