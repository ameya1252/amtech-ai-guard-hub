#include "alarm_logic.h"
#include "alert_dispatch.h"
#include "gpio_control.h"
#include "modem_hal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static void expire_camera_arm_grace(void)
{
    alarm_logic_tick(AMTECH_CAMERA_ARM_GRACE_MS);
}

static void wait_alert_dispatch(void)
{
#ifdef SIMULATE_MODEM
    check_int("alert dispatch worker becomes idle",
              alert_dispatch_test_wait_idle(2000),
              0);
#endif
}

static void force_siren_timeout(void)
{
#ifdef SIMULATE_GPIO
    check_int("test siren timeout helper succeeds",
              alarm_logic_test_force_siren_timeout(),
              0);
#endif
}

static void check_panic_retrigger_and_alert_dispatch_cooldown(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
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
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("panic first trigger sends SMS to all contacts", modem_get_simulated_sms_count(), 3);
    check_int("panic first trigger starts voice call", modem_get_simulated_call_count(), 1);
#endif

    force_siren_timeout();
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
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("panic second trigger inside cooldown suppresses SMS",
              modem_get_simulated_sms_count(),
               3);
    check_int("panic second trigger inside cooldown suppresses voice call",
              modem_get_simulated_call_count(),
              1);
#endif

    force_siren_timeout();
#ifdef SIMULATE_GPIO
    check_int("panic second trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
#endif

    alarm_logic_tick(AMTECH_ALERT_DISPATCH_COOLDOWN_MS);
    alarm_logic_handle_panic(1);
#ifdef SIMULATE_GPIO
    check_int("panic trigger after cooldown restarts siren ON/LOW",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
#endif
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("panic trigger after cooldown sends SMS",
              modem_get_simulated_sms_count(),
               6);
#endif

    alarm_logic_reset();
    alarm_logic_handle_panic(1);
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("reset clears SIM alert cooldown",
              modem_get_simulated_sms_count(),
               9);
#endif
}

static void check_outputs_reactivate_when_alert_dispatch_is_suppressed(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_shop_id("amtech-demo-shop");
    alarm_logic_set_armed(0);

    alarm_logic_handle_panic(1);
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("regression first panic sends SIM alert",
              modem_get_simulated_sms_count(),
               3);
#endif

    force_siren_timeout();
#ifdef SIMULATE_GPIO
    check_int("regression siren is OFF before second panic",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              1);
#endif

    alarm_logic_handle_panic(1);
#ifdef SIMULATE_GPIO
    check_int("regression siren restarts even when alert dispatch suppressed",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
    check_int("regression strobe remains ON even when alert dispatch suppressed",
              gpio_get_simulated_value(TEST_STROBE_GPIO_PIN),
              0);
#endif
#ifdef SIMULATE_MODEM
    check_int("regression second panic suppresses SIM alert too",
              modem_get_simulated_sms_count(),
               3);
#endif
}

static void check_siren_timer_independent_of_blocked_alert_dispatch(void)
{
#if defined(SIMULATE_GPIO) && defined(SIMULATE_MODEM)
    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alert_dispatch_test_set_send_delay_ms(7000);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_shop_id("amtech-demo-shop");
    alarm_logic_set_armed(0);

    alarm_logic_handle_panic(1);
    check_int("blocked dispatch test siren starts ON/LOW",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);

    usleep((AMTECH_SIREN_DURATION_MS + 500U) * 1000U);
    check_int("blocked dispatch test siren auto-stops by wall-clock thread",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              1);
    check_int("blocked dispatch test strobe remains ON/LOW",
              gpio_get_simulated_value(TEST_STROBE_GPIO_PIN),
              0);

    check_int("blocked dispatch eventually completes",
              alert_dispatch_test_wait_idle(4000),
              0);
    alert_dispatch_test_set_send_delay_ms(0);
    alarm_logic_reset();
#endif
}

static void check_siren_retrigger_extends_wall_clock_deadline(void)
{
#if defined(SIMULATE_GPIO) && defined(SIMULATE_MODEM)
    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alert_dispatch_test_set_send_delay_ms(0);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);

    alarm_logic_handle_panic(1);
    wait_alert_dispatch();
    usleep(3000U * 1000U);
    alarm_logic_handle_panic(1);
    usleep(2500U * 1000U);
    check_int("retrigger keeps siren ON past original 5s deadline",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);

    usleep(3000U * 1000U);
    check_int("retriggered siren turns OFF after extended deadline",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              1);
    alarm_logic_reset();
#endif
}

static void check_reset_cancels_pending_async_voice_escalation(void)
{
#if defined(SIMULATE_GPIO) && defined(SIMULATE_MODEM)
    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alert_dispatch_test_set_send_delay_ms(1000);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(0);

    alarm_logic_handle_panic(1);
    alarm_logic_reset();
    check_int("reset cancels pending dispatch worker",
              alert_dispatch_test_wait_idle(3000),
              0);
    check_int("reset canceled pending dispatch voice call",
              modem_get_simulated_call_count(),
              0);
    alert_dispatch_test_set_send_delay_ms(0);
#endif
}

static void check_shutter_retrigger(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
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
    wait_alert_dispatch();

    force_siren_timeout();
#ifdef SIMULATE_GPIO
    check_int("shutter first trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("shutter first trigger strobe stays ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif

    alarm_logic_handle_shutter_dual(SHUTTER_OPEN);
#ifdef SIMULATE_GPIO
    check_int("shutter second trigger restarts siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
    check_int("shutter second trigger keeps strobe ON/LOW", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 0);
#endif
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("shutter second trigger inside cooldown suppresses SMS",
              modem_get_simulated_sms_count(),
               3);
#endif
}

static void send_person_frame(void)
{
    alarm_logic_handle_detection(0, "person", 0.80f);
    alarm_logic_end_frame();
}

static void send_two_person_frames(void)
{
    send_person_frame();
    send_person_frame();
}

static void check_person_retrigger(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    expire_camera_arm_grace();

    send_person_frame();
    check_int("person first frame does not trigger with 2-frame confirmation", alarm_logic_is_triggered(), 0);
    send_person_frame();
    check_int("person second frame triggers active alarm", alarm_logic_is_triggered(), 1);
#ifdef SIMULATE_GPIO
    check_int("person first trigger turns siren ON/LOW", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 0);
#endif
    wait_alert_dispatch();

    force_siren_timeout();
#ifdef SIMULATE_GPIO
    check_int("person first trigger siren auto-stops", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
#endif

    send_two_person_frames();
#ifdef SIMULATE_GPIO
    check_int("person second 2-frame trigger restarts siren ON/LOW",
              gpio_get_simulated_value(TEST_SIREN_GPIO_PIN),
              0);
    check_int("person second 2-frame trigger keeps strobe ON/LOW",
              gpio_get_simulated_value(TEST_STROBE_GPIO_PIN),
              0);
#endif
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("person second trigger inside cooldown suppresses SMS",
              modem_get_simulated_sms_count(),
               3);
#endif
}

static void check_camera_sources_confirm_independently(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    expire_camera_arm_grace();

    alarm_logic_handle_detection_source(0, "person", 0.82f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    check_int("single front frame does not trigger front intrusion",
              alarm_logic_is_triggered(),
              0);
    alarm_logic_handle_detection_source(0, "person", 0.82f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    check_int("second front frame triggers front intrusion",
              alarm_logic_is_triggered(),
              1);
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("front camera source sends SMS fan-out",
              modem_get_simulated_sms_count(),
              3);
    check_int("front camera source message selected",
              strcmp(modem_get_simulated_last_sms_message(),
                     "AMTECH ALERT: Person detected on the front camera while armed.") == 0,
              1);
#endif

    alarm_logic_reset();
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_set_armed(1);
    expire_camera_arm_grace();
    alarm_logic_handle_detection_source(0, "person", 0.83f, "intrusion-parking");
    alarm_logic_end_frame_source("intrusion-parking");
    check_int("single parking frame does not trigger parking intrusion",
              alarm_logic_is_triggered(),
              0);
    alarm_logic_handle_detection_source(0, "person", 0.83f, "intrusion-parking");
    alarm_logic_end_frame_source("intrusion-parking");
    check_int("second parking frame triggers parking intrusion",
              alarm_logic_is_triggered(),
              1);
    wait_alert_dispatch();
#ifdef SIMULATE_MODEM
    check_int("parking camera source message selected",
              strcmp(modem_get_simulated_last_sms_message(),
                     "AMTECH ALERT: Person detected on the parking camera while armed.") == 0,
              1);
#endif
}

static void check_camera_arm_grace_period(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);

    alarm_logic_handle_detection_source(0, "person", 0.80f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    alarm_logic_handle_detection_source(0, "person", 0.82f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    check_int("camera detections during arm grace do not trigger",
              alarm_logic_is_triggered(),
              0);

    alarm_logic_tick(AMTECH_CAMERA_ARM_GRACE_MS - 1);
    alarm_logic_handle_detection_source(0, "person", 0.83f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    check_int("camera detection before grace expiry still does not trigger",
              alarm_logic_is_triggered(),
              0);

    alarm_logic_tick(1);
    alarm_logic_handle_detection_source(0, "person", 0.84f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    check_int("first camera frame after grace does not trigger",
              alarm_logic_is_triggered(),
              0);
    alarm_logic_handle_detection_source(0, "person", 0.85f, "intrusion-front");
    alarm_logic_end_frame_source("intrusion-front");
    check_int("second camera frame after grace triggers",
              alarm_logic_is_triggered(),
              1);
    wait_alert_dispatch();

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_sensor(1);
    check_int("shutter still triggers during camera grace",
              alarm_logic_is_triggered(),
              1);
    wait_alert_dispatch();

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_panic(1);
    check_int("panic still triggers during camera grace",
              alarm_logic_is_triggered(),
              1);
    wait_alert_dispatch();

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_smoke(1);
    check_int("smoke still triggers during camera grace",
              alarm_logic_is_triggered(),
              1);
    wait_alert_dispatch();
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
    wait_alert_dispatch();

    check_int("smoke detector while disarmed triggers immediately", alarm_logic_is_triggered(), 1);

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_smoke(1);
    wait_alert_dispatch();

    check_int("smoke detector while armed triggers immediately", alarm_logic_is_triggered(), 1);
}

static void check_call_state_ticks_without_blocking_alarm_flow(void)
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
    wait_alert_dispatch();
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
    check_int("voice escalation remains active after first no-answer timeout", modem_voice_call_is_active(), 1);
    alarm_logic_tick(1000);
    check_int("voice escalation continues with next attempt", modem_voice_call_is_active(), 1);
#ifdef SIMULATE_MODEM
    check_int("voice escalation hung up first unanswered attempt", modem_get_simulated_hangup_count(), 1);
#endif
}

int main(void)
{
#ifdef SIMULATE_GPIO
    gpio_reset_simulated_values();
#endif
#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    alarm_logic_init(TEST_SIREN_GPIO_PIN);
    alarm_logic_set_shop_id("amtech-demo-shop");
    alarm_logic_set_armed(1);
    expire_camera_arm_grace();

#ifdef SIMULATE_GPIO
    check_int("siren initializes OFF/HIGH", gpio_get_simulated_value(TEST_SIREN_GPIO_PIN), 1);
    check_int("strobe initializes OFF/HIGH", gpio_get_simulated_value(TEST_STROBE_GPIO_PIN), 1);
#endif

    alarm_logic_handle_detection(1, "car", 0.95f);
    alarm_logic_end_frame();
    check_int("non-person detection does not trigger", alarm_logic_is_triggered(), 0);

    alarm_logic_handle_detection(0, "person", 0.80f);
    alarm_logic_end_frame();

    check_int("person detection does not trigger after 1 frame", alarm_logic_is_triggered(), 0);
    alarm_logic_handle_detection(0, "person", 0.80f);
    alarm_logic_end_frame();

    check_int("person detection triggers after 2 frames", alarm_logic_is_triggered(), 1);
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

    force_siren_timeout();

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

    check_panic_retrigger_and_alert_dispatch_cooldown();
    check_outputs_reactivate_when_alert_dispatch_is_suppressed();
    check_siren_timer_independent_of_blocked_alert_dispatch();
    check_siren_retrigger_extends_wall_clock_deadline();
    check_reset_cancels_pending_async_voice_escalation();
    check_shutter_retrigger();
    check_person_retrigger();
    check_camera_sources_confirm_independently();
    check_camera_arm_grace_period();
    check_call_state_ticks_without_blocking_alarm_flow();

    if (failures == 0)
    {
        printf("PASS: alarm logic behaved as expected\n");
        return 0;
    }

    printf("FAIL: alarm logic had %d failure(s)\n", failures);
    return 1;
}
