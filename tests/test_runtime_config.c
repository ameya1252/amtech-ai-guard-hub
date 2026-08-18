#include "config.h"
#include "alarm_logic.h"
#include "alert_dispatch.h"
#include "config_sync.h"
#include "device_command_sync.h"
#include "gpio_control.h"
#include "modem_hal.h"
#include "runtime_loop.h"
#include "sensor_input.h"

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

static void check_string(const char *label, const char *actual, const char *expected)
{
    int matches = strcmp(actual, expected) == 0;
    const char *result = matches ? "PASS" : "FAIL";

    printf("%s: got %s, expected %s: %s\n", label, actual, expected, result);
    if (!matches)
    {
        failures++;
    }
}

static void check_contains(const char *label, const char *actual, const char *expected_substring)
{
    int matches = strstr(actual, expected_substring) != NULL;
    const char *result = matches ? "PASS" : "FAIL";

    printf("%s: got %s, expected substring %s: %s\n",
           label,
           actual,
           expected_substring,
           result);
    if (!matches)
    {
        failures++;
    }
}

static void check_shutter_state(const char *label, shutter_state_t actual, shutter_state_t expected)
{
    const char *result = actual == expected ? "PASS" : "FAIL";

    printf("%s: got %s, expected %s: %s\n",
           label,
           shutter_state_to_string(actual),
           shutter_state_to_string(expected),
           result);
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

static const char *edge_for_pin(runtime_watched_pin_t pins[], int count, int pin)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (pins[i].pin == pin)
        {
            return pins[i].edge;
        }
    }

    return "";
}

static void check_pin_present(runtime_watched_pin_t pins[], int count, int pin, int expected)
{
    char label[64];

    snprintf(label, sizeof(label), "pin %d watched", pin);
    check_int(label, contains_pin(pins, count, pin), expected);
}

static void check_pin_edge(runtime_watched_pin_t pins[],
                           int count,
                           int pin,
                           const char *expected_edge)
{
    const char *actual_edge = edge_for_pin(pins, count, pin);
    int matches = strcmp(actual_edge, expected_edge) == 0;
    char label[80];

    snprintf(label, sizeof(label), "pin %d edge %s", pin, expected_edge);
    check_int(label, matches, 1);
}

static void check_camera_config(const runtime_camera_config_t *camera,
                                const char *expected_source,
                                const char *expected_event_type,
                                const char *expected_url)
{
    check_int("camera config enabled", camera->enabled, 1);
    check_string("camera config source", camera->source, expected_source);
    check_string("camera config event type", camera->event_type, expected_event_type);
    check_string("camera config RTSP URL", camera->rtsp_url, expected_url);
}

static void check_shutter2_ignored_when_single_shutter(void)
{
    amtech_config_t config;

    config.shutter_count = 1;
    config.panic_enabled = 1;
    config.smoke_enabled = 0;

    gpio_reset_simulated_values();
    alarm_logic_init(42);
    alarm_logic_set_armed(1);

    sensor_input_set_simulated_raw_value(33, 0);
    sensor_input_set_simulated_raw_value(40, 1);
    sensor_input_set_simulated_raw_value(41, 1);
    sensor_input_set_simulated_raw_value(72, 0);

    check_int("process configured shutters with SHUTTER_COUNT=1",
              runtime_process_configured_shutters(&config),
              0);
    check_int("Shutter-2 raw OPEN ignored when SHUTTER_COUNT=1",
              alarm_logic_is_triggered(),
              0);
    check_int("siren remains OFF/HIGH when Shutter-2 ignored",
              gpio_get_simulated_value(42),
              1);
    check_int("strobe remains OFF/HIGH when Shutter-2 ignored",
              gpio_get_simulated_value(48),
              1);
}

static void check_panic_active_high(void)
{
    gpio_reset_simulated_values();
    alarm_logic_init(42);
    alarm_logic_set_armed(0);

    check_int("panic raw LOW maps to not triggered",
              runtime_panic_triggered_from_raw(0),
              0);
    alarm_logic_handle_panic(runtime_panic_triggered_from_raw(0));
    check_int("panic raw LOW does not trigger alarm",
              alarm_logic_is_triggered(),
              0);

    check_int("panic raw HIGH maps to triggered",
              runtime_panic_triggered_from_raw(1),
              1);
    alarm_logic_handle_panic(runtime_panic_triggered_from_raw(1));
    check_int("panic raw HIGH triggers alarm",
              alarm_logic_is_triggered(),
              1);
}

static void check_sensor_confirmation_logic(void)
{
    gpio_reset_simulated_values();
    alarm_logic_init(42);
    alarm_logic_set_armed(0);

    check_int("panic LOW through 200ms is ignored",
              runtime_active_high_confirmed_from_raw_sequence(0, 0),
              0);
    check_int("panic HIGH blip shorter than 200ms is ignored",
              runtime_active_high_confirmed_from_raw_sequence(1, 0),
              0);
    alarm_logic_handle_panic(runtime_active_high_confirmed_from_raw_sequence(1, 0));
    check_int("panic HIGH blip does not trigger alarm",
              alarm_logic_is_triggered(),
              0);

    check_int("panic sustained HIGH through 200ms confirms",
              runtime_active_high_confirmed_from_raw_sequence(1, 1),
              1);
    alarm_logic_handle_panic(runtime_active_high_confirmed_from_raw_sequence(1, 1));
    check_int("panic sustained HIGH triggers alarm",
              alarm_logic_is_triggered(),
              1);

    alarm_logic_reset();

    check_shutter_state("shutter open blip shorter than 200ms confirms closed",
                        runtime_confirmed_shutter_state_from_raw_sequence(1, 0, 0, 1),
                        SHUTTER_CLOSED);
    alarm_logic_handle_shutter_dual(runtime_confirmed_shutter_state_from_raw_sequence(1, 0, 0, 1));
    check_int("shutter open blip does not trigger alarm",
              alarm_logic_is_triggered(),
              0);

    check_shutter_state("shutter sustained open through 200ms confirms open",
                        runtime_confirmed_shutter_state_from_raw_sequence(1, 0, 1, 0),
                        SHUTTER_OPEN);
    alarm_logic_set_armed(1);
    alarm_logic_handle_shutter_dual(runtime_confirmed_shutter_state_from_raw_sequence(1, 0, 1, 0));
    check_int("shutter sustained open triggers while armed",
              alarm_logic_is_triggered(),
              1);

    alarm_logic_reset();

    check_shutter_state("shutter sustained tamper through 200ms confirms tamper",
                        runtime_confirmed_shutter_state_from_raw_sequence(1, 1, 1, 1),
                        SHUTTER_TAMPER);
    alarm_logic_set_armed(0);
    alarm_logic_handle_shutter_dual(runtime_confirmed_shutter_state_from_raw_sequence(1, 1, 1, 1));
    check_int("shutter sustained tamper triggers while disarmed",
              alarm_logic_is_triggered(),
              1);

    alarm_logic_reset();

    check_int("smoke raw HIGH maps to not confirmed",
              runtime_active_low_confirmed_from_raw_sequence(1, 1),
              0);
    alarm_logic_handle_smoke(runtime_active_low_confirmed_from_raw_sequence(1, 1));
    check_int("smoke raw HIGH does not trigger alarm",
              alarm_logic_is_triggered(),
              0);

    check_int("smoke LOW blip shorter than 200ms is ignored",
              runtime_active_low_confirmed_from_raw_sequence(0, 1),
              0);
    alarm_logic_handle_smoke(runtime_active_low_confirmed_from_raw_sequence(0, 1));
    check_int("smoke LOW blip does not trigger alarm",
              alarm_logic_is_triggered(),
              0);

    check_int("smoke sustained LOW through 200ms confirms",
              runtime_active_low_confirmed_from_raw_sequence(0, 0),
              1);
    alarm_logic_handle_smoke(runtime_active_low_confirmed_from_raw_sequence(0, 0));
    check_int("smoke sustained LOW triggers alarm",
              alarm_logic_is_triggered(),
              1);
}

static void check_sms_remote_control(void)
{
    amtech_config_t config;
    int sms_count_after_trigger;

    amtech_config_set_defaults(&config);
    snprintf(config.alert_contacts[0], sizeof(config.alert_contacts[0]), "%s", "+911111111111");
    snprintf(config.alert_contacts[1], sizeof(config.alert_contacts[1]), "%s", "+912222222222");
    snprintf(config.alert_contacts[2], sizeof(config.alert_contacts[2]), "%s", "+913333333333");

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    alarm_logic_set_armed(0);

    check_int("SMS receive init succeeds", modem_sms_receive_init(), 0);
    check_int("SMS receive init count", modem_get_simulated_sms_receive_init_count(), 1);

    modem_simulate_incoming_sms("+911111111111", "ARM");
    check_int("contact 1 ARM SMS accepted", runtime_poll_sms_remote_control(&config), 1);
    check_int("contact 1 ARM sets armed", alarm_logic_is_armed(), 1);
    check_int("contact 1 ARM sends confirmation SMS", modem_get_simulated_sms_count(), 1);
    check_string("contact 1 ARM reply text", modem_get_simulated_last_sms_message(), "System ARMING...");
    check_int("contact 1 ARM SMS deleted", modem_get_simulated_deleted_sms_count(), 1);

    modem_simulate_incoming_sms("911111111111", "arm");
    check_int("redundant ARM SMS accepted from 91-prefixed sender", runtime_poll_sms_remote_control(&config), 1);
    check_int("redundant ARM leaves armed", alarm_logic_is_armed(), 1);
    check_int("redundant ARM sends confirmation SMS", modem_get_simulated_sms_count(), 2);
    check_string("redundant ARM reply text", modem_get_simulated_last_sms_message(), "System already ARMED");
    check_int("redundant ARM SMS deleted", modem_get_simulated_deleted_sms_count(), 2);

    modem_simulate_incoming_sms("2222222222", "  disarm \r\n");
    check_int("contact 2 DISARM SMS accepted from 10-digit sender", runtime_poll_sms_remote_control(&config), 1);
    check_int("contact 2 DISARM clears armed", alarm_logic_is_armed(), 0);
    check_int("contact 2 DISARM sends confirmation SMS", modem_get_simulated_sms_count(), 3);
    check_string("contact 2 DISARM reply text", modem_get_simulated_last_sms_message(), "System DISARMED");
    check_int("contact 2 DISARM SMS deleted", modem_get_simulated_deleted_sms_count(), 3);

    modem_simulate_incoming_sms("+912222222222", "DISARM");
    check_int("redundant DISARM SMS accepted", runtime_poll_sms_remote_control(&config), 1);
    check_int("redundant DISARM leaves disarmed", alarm_logic_is_armed(), 0);
    check_int("redundant DISARM sends confirmation SMS", modem_get_simulated_sms_count(), 4);
    check_string("redundant DISARM reply text", modem_get_simulated_last_sms_message(), "System already DISARMED");
    check_int("redundant DISARM SMS deleted", modem_get_simulated_deleted_sms_count(), 4);

    modem_simulate_incoming_sms("+913333333333", "aRm");
    check_int("contact 3 mixed-case ARM SMS accepted", runtime_poll_sms_remote_control(&config), 1);
    check_int("contact 3 ARM sets armed", alarm_logic_is_armed(), 1);
    check_int("contact 3 ARM sends confirmation SMS", modem_get_simulated_sms_count(), 5);
    check_int("contact 3 ARM SMS deleted", modem_get_simulated_deleted_sms_count(), 5);

    alarm_logic_handle_panic(1);
    check_int("STOP setup alarm is triggered", alarm_logic_is_triggered(), 1);
    check_int("STOP setup siren is ON/LOW", gpio_get_simulated_value(42), 0);
    check_int("STOP setup strobe is ON/LOW", gpio_get_simulated_value(48), 0);
    sms_count_after_trigger = modem_get_simulated_sms_count();

    modem_simulate_incoming_sms("+911111111111", "stop");
    check_int("STOP SMS accepted while alarm and alert call are active", runtime_poll_sms_remote_control(&config), 1);
    check_int("STOP clears triggered state", alarm_logic_is_triggered(), 0);
    check_int("STOP disarms system", alarm_logic_is_armed(), 0);
    check_int("STOP turns siren OFF/HIGH", gpio_get_simulated_value(42), 1);
    check_int("STOP turns strobe OFF/HIGH", gpio_get_simulated_value(48), 1);
    check_int("STOP sends one confirmation SMS", modem_get_simulated_sms_count(), sms_count_after_trigger + 1);
    check_string("STOP reply text", modem_get_simulated_last_sms_message(), "Alarm stopped, system DISARMED");
    check_int("STOP SMS deleted", modem_get_simulated_deleted_sms_count(), 6);

    modem_simulate_incoming_sms("1111111111", "STOP");
    check_int("STOP SMS accepted with no active alarm from 10-digit sender", runtime_poll_sms_remote_control(&config), 1);
    check_int("STOP no-active leaves triggered clear", alarm_logic_is_triggered(), 0);
    check_int("STOP no-active sends feedback SMS", modem_get_simulated_sms_count(), sms_count_after_trigger + 2);
    check_string("STOP no-active reply text", modem_get_simulated_last_sms_message(), "No active alarm");
    check_int("STOP no-active SMS deleted", modem_get_simulated_deleted_sms_count(), 7);

    runtime_test_note_camera_health("front", 1);
    runtime_test_note_camera_health("parking", 0);
    config.shutter_count = 2;
    config.smoke_enabled = 1;
    config.camera_enabled = 1;
    snprintf(config.camera_rtsp_url, sizeof(config.camera_rtsp_url), "%s", "rtsp://example/front");
    config.camera2_enabled = 1;
    snprintf(config.camera2_rtsp_url, sizeof(config.camera2_rtsp_url), "%s", "rtsp://example/parking");
    modem_simulate_incoming_sms("+911111111111", "status");
    check_int("STATUS SMS accepted from authorized sender", runtime_poll_sms_remote_control(&config), 1);
    check_int("STATUS sends reply", modem_get_simulated_sms_count(), sms_count_after_trigger + 3);
    check_contains("STATUS starts with armed state", modem_get_simulated_last_sms_message(), "DISARMED; Panic cfg");
    check_contains("STATUS includes shutter-1 configured", modem_get_simulated_last_sms_message(), "Sh1 cfg");
    check_contains("STATUS includes shutter-2 configured", modem_get_simulated_last_sms_message(), "Sh2 cfg");
    check_contains("STATUS includes smoke configured", modem_get_simulated_last_sms_message(), "Smoke cfg");
    check_contains("STATUS includes front camera recent OK", modem_get_simulated_last_sms_message(), "Front cam recent OK");
    check_contains("STATUS includes parking camera failing", modem_get_simulated_last_sms_message(), "Parking cam failing");
    check_contains("STATUS includes modem last-known state", modem_get_simulated_last_sms_message(), "Modem POWER_OFF last-known");
    check_int("STATUS SMS deleted", modem_get_simulated_deleted_sms_count(), 8);

    config.shutter_count = 1;
    config.smoke_enabled = 0;
    config.camera_enabled = 1;
    snprintf(config.camera_rtsp_url, sizeof(config.camera_rtsp_url), "%s", "rtsp://example/front");
    config.camera2_enabled = 0;
    config.camera2_rtsp_url[0] = '\0';
    modem_simulate_incoming_sms("+912222222222", "STATUS");
    check_int("STATUS SMS accepted with disabled optional devices", runtime_poll_sms_remote_control(&config), 1);
    check_int("STATUS disabled-device reply count", modem_get_simulated_sms_count(), sms_count_after_trigger + 4);
    check_contains("STATUS reports shutter-2 off", modem_get_simulated_last_sms_message(), "Sh2 off");
    check_contains("STATUS reports smoke off", modem_get_simulated_last_sms_message(), "Smoke off");
    check_contains("STATUS reports camera2 off", modem_get_simulated_last_sms_message(), "Parking cam off");
    check_int("STATUS disabled-device SMS deleted", modem_get_simulated_deleted_sms_count(), 9);

    modem_simulate_incoming_sms("+913333333333", "HeLp");
    check_int("HELP SMS accepted from authorized sender", runtime_poll_sms_remote_control(&config), 1);
    check_int("HELP sends reply", modem_get_simulated_sms_count(), sms_count_after_trigger + 5);
    check_contains("HELP includes ARM", modem_get_simulated_last_sms_message(), "ARM - Arm system");
    check_contains("HELP includes DISARM", modem_get_simulated_last_sms_message(), "DISARM - Disarm system");
    check_contains("HELP includes STOP", modem_get_simulated_last_sms_message(), "STOP - Stop active alarm & disarm");
    check_contains("HELP includes STATUS", modem_get_simulated_last_sms_message(), "STATUS - System status report");
    check_contains("HELP includes HELP", modem_get_simulated_last_sms_message(), "HELP - This message");
    check_int("HELP SMS deleted", modem_get_simulated_deleted_sms_count(), 10);

    alarm_logic_set_armed(1);

    modem_simulate_incoming_sms("+919999999999", "HELP");
    check_int("unknown sender SMS ignored", runtime_poll_sms_remote_control(&config), 0);
    check_int("unknown sender does not change armed state", alarm_logic_is_armed(), 1);
    check_int("unknown sender gets no reply", modem_get_simulated_sms_count(), sms_count_after_trigger + 5);
    check_int("unknown sender SMS still deleted", modem_get_simulated_deleted_sms_count(), 11);

    modem_simulate_incoming_sms("+911111111111", "BANANA");
    check_int("malformed valid-sender SMS ignored", runtime_poll_sms_remote_control(&config), 0);
    check_int("malformed SMS leaves armed unchanged", alarm_logic_is_armed(), 1);
    check_int("malformed SMS gets no reply", modem_get_simulated_sms_count(), sms_count_after_trigger + 5);
    check_int("malformed SMS still deleted", modem_get_simulated_deleted_sms_count(), 12);

    modem_make_voice_call("+911111111111");
    modem_simulate_incoming_sms("+911111111111", "DISARM");
    check_int("SMS poll still runs while voice call active", runtime_poll_sms_remote_control(&config), 1);
    check_int("SMS deleted while call active", modem_get_simulated_deleted_sms_count(), 13);
    check_int("DISARM clears armed while call active", alarm_logic_is_armed(), 0);
    check_int("DISARM reply sent while call active", modem_get_simulated_sms_count(), sms_count_after_trigger + 6);
    modem_hangup_voice_call();
}

static void check_sms_reply_failures_are_not_reported_as_sent(void)
{
    amtech_config_t config;
    int fail_once[1] = {-1};

    amtech_config_set_defaults(&config);
    snprintf(config.alert_contacts[0], sizeof(config.alert_contacts[0]), "%s", "+911111111111");

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_set_armed(0);

    modem_set_simulated_sms_send_results(fail_once, 1);
    modem_simulate_incoming_sms("+911111111111", "ARM");
    check_int("ARM command still applies when reply SMS fails",
              runtime_poll_sms_remote_control(&config),
              1);
    check_int("ARM command armed despite reply failure", alarm_logic_is_armed(), 1);
    check_int("ARM failed reply is not counted as sent", modem_get_simulated_sms_count(), 0);
    check_int("ARM failed-reply SMS still deleted", modem_get_simulated_deleted_sms_count(), 1);

    modem_set_simulated_sms_send_results(fail_once, 1);
    modem_simulate_incoming_sms("+911111111111", "DISARM");
    check_int("DISARM command still applies when reply SMS fails",
              runtime_poll_sms_remote_control(&config),
              1);
    check_int("DISARM command disarmed despite reply failure", alarm_logic_is_armed(), 0);
    check_int("DISARM failed reply is not counted as sent", modem_get_simulated_sms_count(), 0);
    check_int("DISARM failed-reply SMS still deleted", modem_get_simulated_deleted_sms_count(), 2);

    alarm_logic_handle_panic(1);
    check_int("STOP failure setup alarm triggered", alarm_logic_is_triggered(), 1);
    check_int("STOP failure setup alert dispatch completed",
              alert_dispatch_test_wait_idle(2000),
              0);
    modem_reset_simulated_state();
    modem_set_simulated_sms_send_results(fail_once, 1);
    modem_simulate_incoming_sms("+911111111111", "STOP");
    check_int("STOP command still applies when reply SMS fails",
              runtime_poll_sms_remote_control(&config),
              1);
    check_int("STOP command reset alarm despite reply failure", alarm_logic_is_triggered(), 0);
    check_int("STOP failed-reply SMS still deleted", modem_get_simulated_deleted_sms_count(), 1);

    modem_set_simulated_sms_send_results(fail_once, 1);
    modem_simulate_incoming_sms("+911111111111", "STATUS");
    check_int("STATUS command still processed when reply SMS fails",
              runtime_poll_sms_remote_control(&config),
              1);
    check_int("STATUS failed-reply SMS still deleted", modem_get_simulated_deleted_sms_count(), 2);
}

static void check_sms_manual_override_blocks_schedule_until_boundary(void)
{
    amtech_config_t config;

    amtech_config_set_defaults(&config);
    snprintf(config.alert_contacts[0], sizeof(config.alert_contacts[0]), "%s", "+911111111111");

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_set_armed(0);

    modem_simulate_incoming_sms("+911111111111", "ARM");
    check_int("manual override ARM SMS accepted", runtime_poll_sms_remote_control(&config), 1);
    check_int("manual override ARM sets armed", alarm_logic_is_armed(), 1);
    check_string("manual override immediate ARM reply", modem_get_simulated_last_sms_message(), "System ARMING...");

    runtime_test_apply_schedule_armed(0);
    check_int("schedule cannot immediately disarm SMS manual ARM", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(0);
    check_int("repeated same schedule state still cannot disarm SMS manual ARM", alarm_logic_is_armed(), 1);

    alarm_logic_handle_shutter_dual_named(SHUTTER_OPEN, "shutter-1", "shutter-1");
    check_int("shutter open triggers after SMS ARM despite schedule being disarmed", alarm_logic_is_triggered(), 1);
    alarm_logic_reset();

    runtime_test_apply_schedule_armed(1);
    check_int("schedule boundary clears manual ARM override and keeps scheduled armed", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(0);
    check_int("normal schedule disarms after manual ARM override cleared", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(1);
    check_int("normal schedule-only arm still works without manual override", alarm_logic_is_armed(), 1);

    modem_simulate_incoming_sms("+911111111111", "DISARM");
    check_int("manual override DISARM SMS accepted", runtime_poll_sms_remote_control(&config), 1);
    check_int("manual override DISARM clears armed", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(1);
    check_int("schedule cannot immediately re-arm SMS manual DISARM", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(1);
    check_int("repeated same schedule state still cannot re-arm SMS manual DISARM", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(0);
    check_int("schedule boundary clears manual DISARM override and keeps scheduled disarmed", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(1);
    check_int("normal schedule arms after manual DISARM override cleared", alarm_logic_is_armed(), 1);
}

static void check_app_command_manual_override_blocks_schedule_until_boundary(void)
{
    amtech_config_t config;

    amtech_config_set_defaults(&config);

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_set_armed(0);

    runtime_test_apply_app_command(AMTECH_DEVICE_COMMAND_ARM, &config);
    check_int("app ARM command sets armed", alarm_logic_is_armed(), 1);

    runtime_test_apply_schedule_armed(0);
    check_int("schedule cannot immediately disarm app manual ARM", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(0);
    check_int("repeated same schedule state still cannot disarm app manual ARM", alarm_logic_is_armed(), 1);

    alarm_logic_handle_shutter_dual_named(SHUTTER_OPEN, "shutter-1", "shutter-1");
    check_int("shutter open triggers after app ARM despite schedule being disarmed", alarm_logic_is_triggered(), 1);
    alarm_logic_reset();

    runtime_test_apply_schedule_armed(1);
    check_int("schedule boundary clears app ARM override and keeps scheduled armed", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(0);
    check_int("normal schedule disarms after app ARM override cleared", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(1);
    check_int("normal schedule-only arm works before app DISARM", alarm_logic_is_armed(), 1);

    runtime_test_apply_app_command(AMTECH_DEVICE_COMMAND_DISARM, &config);
    check_int("app DISARM command clears armed", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(1);
    check_int("schedule cannot immediately re-arm app manual DISARM", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(1);
    check_int("repeated same schedule state still cannot re-arm app manual DISARM", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(0);
    check_int("schedule boundary clears app DISARM override and keeps scheduled disarmed", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(1);
    check_int("normal schedule arms after app DISARM override cleared", alarm_logic_is_armed(), 1);
}

static void check_persisted_armed_state_survives_restart_until_schedule_boundary(void)
{
    amtech_config_t config;
    const char *state_path = "/tmp/amtech_runtime_state_test.txt";
    char contents[256];
    FILE *fp;
    size_t bytes_read;

    amtech_config_set_defaults(&config);
    remove(state_path);
    setenv("AMTECH_STATE_PATH", state_path, 1);

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_disable_state_persistence();
    runtime_test_set_armed(0);
    runtime_test_enable_state_persistence();

    runtime_test_apply_app_command(AMTECH_DEVICE_COMMAND_ARM, &config);
    check_int("persisted state app ARM sets armed", alarm_logic_is_armed(), 1);

    fp = fopen(state_path, "r");
    if (fp == NULL)
    {
        printf("FAIL: could not read persisted runtime state file\n");
        failures++;
        runtime_test_disable_state_persistence();
        unsetenv("AMTECH_STATE_PATH");
        return;
    }
    bytes_read = fread(contents, 1, sizeof(contents) - 1, fp);
    contents[bytes_read] = '\0';
    fclose(fp);
    check_contains("persisted state records armed", contents, "ARMED=1");
    check_contains("persisted state records manual control", contents, "CONTROL=MANUAL");

    runtime_test_disable_state_persistence();
    runtime_test_set_armed(0);
    check_int("simulated restart starts from disarmed memory state", alarm_logic_is_armed(), 0);
    runtime_test_enable_state_persistence();
    check_int("restore persisted runtime state", runtime_test_restore_persisted_state(&config), 0);
    check_int("restore persisted ARM after restart", alarm_logic_is_armed(), 1);

    runtime_test_apply_schedule_armed(0);
    check_int("restored ARM not disarmed by first schedule-disarmed tick", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(0);
    check_int("restored ARM not disarmed by repeated schedule-disarmed tick", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(1);
    check_int("restored ARM override clears at schedule arm boundary", alarm_logic_is_armed(), 1);
    runtime_test_apply_schedule_armed(0);
    check_int("schedule can disarm after restored override boundary", alarm_logic_is_armed(), 0);

    runtime_test_disable_state_persistence();
    remove(state_path);
    unsetenv("AMTECH_STATE_PATH");
}

static void check_persisted_disarmed_state_survives_restart_until_schedule_boundary(void)
{
    amtech_config_t config;
    const char *state_path = "/tmp/amtech_runtime_state_test.txt";

    amtech_config_set_defaults(&config);
    remove(state_path);
    setenv("AMTECH_STATE_PATH", state_path, 1);

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_disable_state_persistence();
    runtime_test_set_armed(1);
    runtime_test_enable_state_persistence();

    runtime_test_apply_app_command(AMTECH_DEVICE_COMMAND_DISARM, &config);
    check_int("persisted state app DISARM clears armed", alarm_logic_is_armed(), 0);

    runtime_test_disable_state_persistence();
    runtime_test_set_armed(1);
    check_int("simulated restart starts from armed memory state", alarm_logic_is_armed(), 1);
    runtime_test_enable_state_persistence();
    check_int("restore persisted DISARM runtime state", runtime_test_restore_persisted_state(&config), 0);
    check_int("restore persisted DISARM after restart", alarm_logic_is_armed(), 0);

    runtime_test_apply_schedule_armed(1);
    check_int("restored DISARM not re-armed by first schedule-armed tick", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(1);
    check_int("restored DISARM not re-armed by repeated schedule-armed tick", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(0);
    check_int("restored DISARM override clears at schedule disarm boundary", alarm_logic_is_armed(), 0);
    runtime_test_apply_schedule_armed(1);
    check_int("schedule can arm after restored override boundary", alarm_logic_is_armed(), 1);

    runtime_test_disable_state_persistence();
    remove(state_path);
    unsetenv("AMTECH_STATE_PATH");
}

static void check_device_command_sync_parser_and_ack(void)
{
    amtech_config_t config;
    amtech_device_command_t command;

    amtech_config_set_defaults(&config);

    amtech_device_command_simulated_reset();
    amtech_device_command_set_simulated_response(
        "{\"ok\":true,\"shop_id\":\"amtech-demo-shop\","
        "\"pending_command\":\"arm\","
        "\"pending_command_id\":\"cmd-arm-1\"}");
    check_int("device command fetch ARM", amtech_device_command_fetch(&config, "amtech-demo-shop", &command), 0);
    check_int("device command type ARM", command.type, AMTECH_DEVICE_COMMAND_ARM);
    check_string("device command ARM id", command.id, "cmd-arm-1");
    check_int("device command ack ARM", amtech_device_command_ack(&config, "amtech-demo-shop", &command), 0);
    check_int("device command ack count after ARM", amtech_device_command_simulated_ack_count(), 1);

    amtech_device_command_set_simulated_response(
        "{\"ok\":true,\"shop_id\":\"amtech-demo-shop\","
        "\"pending_command\":\"disarm\","
        "\"pending_command_id\":\"cmd-disarm-1\"}");
    check_int("device command fetch DISARM", amtech_device_command_fetch(&config, "amtech-demo-shop", &command), 0);
    check_int("device command type DISARM", command.type, AMTECH_DEVICE_COMMAND_DISARM);
    check_string("device command DISARM id", command.id, "cmd-disarm-1");

    amtech_device_command_set_simulated_response("{\"ok\":true,\"pending_command\":null,\"pending_command_id\":null}");
    check_int("device command fetch none", amtech_device_command_fetch(&config, "amtech-demo-shop", &command), 0);
    check_int("device command type none", command.type, AMTECH_DEVICE_COMMAND_NONE);
}

static void check_camera_monitoring_active_sms(void)
{
    amtech_config_t config;

    amtech_config_set_defaults(&config);
    snprintf(config.alert_contacts[0], sizeof(config.alert_contacts[0]), "%s", "+911111111111");
    snprintf(config.alert_contacts[1], sizeof(config.alert_contacts[1]), "%s", "+912222222222");
    snprintf(config.alert_contacts[2], sizeof(config.alert_contacts[2]), "%s", "+913333333333");

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_set_armed(0);

    modem_simulate_incoming_sms("+911111111111", "ARM");
    check_int("camera active SMS setup ARM accepted", runtime_poll_sms_remote_control(&config), 1);
    check_int("camera active SMS immediate ARM reply only", modem_get_simulated_sms_count(), 1);

    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);
    check_int("SMS ARM calibration completion sends all-contact SMS", modem_get_simulated_sms_count(), 4);
    check_string("SMS ARM completion contact 1", modem_get_simulated_sms_number_at(1), "+911111111111");
    check_string("SMS ARM completion contact 2", modem_get_simulated_sms_number_at(2), "+912222222222");
    check_string("SMS ARM completion contact 3", modem_get_simulated_sms_number_at(3), "+913333333333");
    check_string("SMS ARM completion message",
                 modem_get_simulated_last_sms_message(),
                 "System ARMED");

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_set_armed(0);

    modem_simulate_incoming_sms("+911111111111", "ARM");
    check_int("abort setup ARM accepted", runtime_poll_sms_remote_control(&config), 1);
    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS / 2);
    modem_simulate_incoming_sms("+911111111111", "DISARM");
    check_int("abort DISARM accepted during calibration", runtime_poll_sms_remote_control(&config), 1);
    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);
    check_int("disarm during calibration suppresses active-camera SMS", modem_get_simulated_sms_count(), 2);
    check_string("abort final SMS remains DISARM reply",
                 modem_get_simulated_last_sms_message(),
                 "System DISARMED");

    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    alarm_logic_init(42);
    runtime_test_set_armed(0);

    runtime_test_apply_schedule_armed_with_config(1, &config);
    check_int("schedule ARM sets armed for camera active SMS", alarm_logic_is_armed(), 1);
    check_int("schedule ARM sends no immediate SMS", modem_get_simulated_sms_count(), 0);
    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);
    check_int("schedule ARM calibration completion sends all-contact SMS", modem_get_simulated_sms_count(), 3);
    check_string("schedule completion contact 1", modem_get_simulated_sms_number_at(0), "+911111111111");
    check_string("schedule completion contact 2", modem_get_simulated_sms_number_at(1), "+912222222222");
    check_string("schedule completion contact 3", modem_get_simulated_sms_number_at(2), "+913333333333");
    check_string("schedule completion message",
                 modem_get_simulated_last_sms_message(),
                 "System ARMED");
}

static void check_config_sync_preserves_unrelated_keys(void)
{
    const char *config_path = "/tmp/amtech_config_sync_test.txt";
    FILE *fp;
    amtech_config_t config;
    char contents[2048];
    size_t bytes_read;

    fp = fopen(config_path, "w");
    if (fp == NULL)
    {
        printf("FAIL: could not write config sync test file\n");
        failures++;
        return;
    }
    fprintf(fp,
            "SHUTTER_COUNT=2\n"
            "PANIC_ENABLED=1\n"
            "SMOKE_ENABLED=1\n"
            "SCHEDULE_ARM=23:00\n"
            "SCHEDULE_DISARM=06:00\n"
            "MODEM_DEVICE=/dev/ttyS1\n"
            "ALERT_CONTACT_1=+911111111111\n"
            "ALERT_CONTACT_2=+912222222222\n"
            "ALERT_CONTACT_3=+913333333333\n"
            "CAMERA_ENABLED=1\n"
            "CAMERA_RTSP_URL=rtsp://front\n"
            "CAMERA2_ENABLED=1\n"
            "CAMERA2_RTSP_URL=rtsp://parking\n"
            "BACKEND_BASE_URL=https://backend.example\n"
            "DEVICE_CONFIG_TOKEN=local-token\n");
    fclose(fp);

    check_int("config sync initial load", amtech_config_load(config_path, &config), 0);
    amtech_config_sync_set_simulated_response(
        "{\"ok\":true,\"shop_id\":\"amtech-demo-shop\","
        "\"schedule\":{\"arm_hour\":21,\"arm_minute\":30,\"disarm_hour\":7,\"disarm_minute\":15},"
        "\"emergency_contacts\":["
        "{\"name\":\"A\",\"phone\":\"+914444444444\",\"slot\":1},"
        "{\"name\":\"B\",\"phone\":\"+915555555555\",\"slot\":2},"
        "{\"name\":\"C\",\"phone\":\"+916666666666\",\"slot\":3}]}");

    check_int("config sync poll applies changed backend config",
              amtech_config_sync_poll(config_path, "amtech-demo-shop", &config),
              1);
    check_int("config sync updated arm hour", config.schedule_arm_hour, 21);
    check_int("config sync updated arm minute", config.schedule_arm_minute, 30);
    check_int("config sync updated disarm hour", config.schedule_disarm_hour, 7);
    check_int("config sync updated disarm minute", config.schedule_disarm_minute, 15);
    check_string("config sync updated contact 1", config.alert_contacts[0], "+914444444444");
    check_string("config sync preserved modem device", config.modem_device, "/dev/ttyS1");
    check_string("config sync preserved camera URL", config.camera_rtsp_url, "rtsp://front");
    check_string("config sync preserved camera2 URL", config.camera2_rtsp_url, "rtsp://parking");
    check_string("config sync preserved backend URL", config.backend_base_url, "https://backend.example");
    check_string("config sync preserved device token", config.device_config_token, "local-token");

    fp = fopen(config_path, "r");
    if (fp == NULL)
    {
        printf("FAIL: could not read config sync result file\n");
        failures++;
        remove(config_path);
        return;
    }
    bytes_read = fread(contents, 1, sizeof(contents) - 1, fp);
    contents[bytes_read] = '\0';
    fclose(fp);

    check_contains("config sync file has updated schedule arm", contents, "SCHEDULE_ARM=21:30");
    check_contains("config sync file has updated schedule disarm", contents, "SCHEDULE_DISARM=07:15");
    check_contains("config sync file has updated contact 3", contents, "ALERT_CONTACT_3=+916666666666");
    check_contains("config sync file preserves shutter count", contents, "SHUTTER_COUNT=2");
    check_contains("config sync file preserves camera1 URL", contents, "CAMERA_RTSP_URL=rtsp://front");
    check_contains("config sync file preserves camera2 URL", contents, "CAMERA2_RTSP_URL=rtsp://parking");

    check_int("config sync same payload no-op",
              amtech_config_sync_poll(config_path, "amtech-demo-shop", &config),
              0);

    remove(config_path);
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
    check_int("missing config default smoke disabled", config.smoke_enabled, 0);
    check_int("missing config default schedule arm hour", config.schedule_arm_hour, 23);
    check_int("missing config default schedule arm minute", config.schedule_arm_minute, 0);
    check_int("missing config default schedule disarm hour", config.schedule_disarm_hour, 6);
    check_int("missing config default schedule disarm minute", config.schedule_disarm_minute, 0);
    check_string("missing config default modem device", config.modem_device, AMTECH_DEFAULT_MODEM_DEVICE);
    check_string("missing config default alert contact 1",
                 config.alert_contacts[0],
                 AMTECH_DEFAULT_ALERT_CONTACT_1);
    check_string("missing config default alert contact 2",
                 config.alert_contacts[1],
                 AMTECH_DEFAULT_ALERT_CONTACT_2);
    check_string("missing config default alert contact 3",
                 config.alert_contacts[2],
                 AMTECH_DEFAULT_ALERT_CONTACT_3);
    check_int("missing config default camera disabled", config.camera_enabled, 0);
    check_string("missing config default camera URL", config.camera_rtsp_url, "");
    check_int("missing config default camera2 disabled", config.camera2_enabled, 0);
    check_string("missing config default camera2 URL", config.camera2_rtsp_url, "");
    check_string("missing config default backend base URL", config.backend_base_url, AMTECH_DEFAULT_BACKEND_BASE_URL);
    check_string("missing config default shop id", config.shop_id, AMTECH_DEFAULT_SHOP_ID);

    fp = fopen(two_shutter_config_path, "w");
    if (fp == NULL)
    {
        printf("FAIL: could not write test config file\n");
        return 1;
    }
    fprintf(fp, "SHUTTER_COUNT=2\nPANIC_ENABLED=1\nSMOKE_ENABLED=1\nSCHEDULE_ARM=22:15\nSCHEDULE_DISARM=05:45\nMODEM_DEVICE=/dev/ttyS1\nALERT_CONTACT_1=+919999999991\nALERT_CONTACT_2=+919999999992\nALERT_CONTACT_3=+919999999993\nCAMERA_ENABLED=1\nCAMERA_RTSP_URL=rtsp://user:pass@192.168.0.2:554/stream1\nCAMERA2_ENABLED=1\nCAMERA2_RTSP_URL=rtsp://user:pass@192.168.0.4:554/stream1\nBACKEND_BASE_URL=https://example.test\nDEVICE_CONFIG_TOKEN=test-token\nSHOP_ID=real-shop-123\n");
    fclose(fp);

    check_int("two-shutter config load", amtech_config_load(two_shutter_config_path, &config), 0);
    check_int("two-shutter config shutter count", config.shutter_count, 2);
    check_int("two-shutter config panic enabled", config.panic_enabled, 1);
    check_int("two-shutter config smoke enabled", config.smoke_enabled, 1);
    check_int("configured schedule arm hour", config.schedule_arm_hour, 22);
    check_int("configured schedule arm minute", config.schedule_arm_minute, 15);
    check_int("configured schedule disarm hour", config.schedule_disarm_hour, 5);
    check_int("configured schedule disarm minute", config.schedule_disarm_minute, 45);
    check_string("configured modem device", config.modem_device, "/dev/ttyS1");
    check_string("configured alert contact 1", config.alert_contacts[0], "+919999999991");
    check_string("configured alert contact 2", config.alert_contacts[1], "+919999999992");
    check_string("configured alert contact 3", config.alert_contacts[2], "+919999999993");
    check_string("configured camera RTSP URL",
                 config.camera_rtsp_url,
                 "rtsp://user:pass@192.168.0.2:554/stream1");
    check_int("configured camera enabled", config.camera_enabled, 1);
    check_int("configured camera2 enabled", config.camera2_enabled, 1);
    check_string("configured camera2 RTSP URL",
                 config.camera2_rtsp_url,
                 "rtsp://user:pass@192.168.0.4:554/stream1");
    check_string("configured backend base URL", config.backend_base_url, "https://example.test");
    check_string("configured device config token", config.device_config_token, "test-token");
    check_string("configured shop id", config.shop_id, "real-shop-123");

    {
        runtime_camera_config_t cameras[AMTECH_RUNTIME_MAX_CAMERAS];

        count = runtime_build_camera_configs(&config, cameras, AMTECH_RUNTIME_MAX_CAMERAS);
        check_int("both cameras configured count", count, 2);
        check_camera_config(&cameras[0],
                            "front",
                            "intrusion-front",
                            "rtsp://user:pass@192.168.0.2:554/stream1");
        check_camera_config(&cameras[1],
                            "parking",
                            "intrusion-parking",
                            "rtsp://user:pass@192.168.0.4:554/stream1");

        config.camera_enabled = 0;
        config.camera2_enabled = 1;
        count = runtime_build_camera_configs(&config, cameras, AMTECH_RUNTIME_MAX_CAMERAS);
        check_int("camera1 disabled camera2 enabled count", count, 1);
        check_camera_config(&cameras[0],
                            "parking",
                            "intrusion-parking",
                            "rtsp://user:pass@192.168.0.4:554/stream1");

        config.camera_enabled = 1;
        config.camera2_enabled = 0;
        count = runtime_build_camera_configs(&config, cameras, AMTECH_RUNTIME_MAX_CAMERAS);
        check_int("camera1 enabled camera2 disabled count", count, 1);
        check_camera_config(&cameras[0],
                            "front",
                            "intrusion-front",
                            "rtsp://user:pass@192.168.0.2:554/stream1");

        config.camera_enabled = 1;
        config.camera_rtsp_url[0] = '\0';
        config.camera2_enabled = 0;
        count = runtime_build_camera_configs(&config, cameras, AMTECH_RUNTIME_MAX_CAMERAS);
        check_int("enabled camera with empty URL does not start", count, 0);
    }

    amtech_config_set_defaults(&config);
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("default shutter count", config.shutter_count, 1);
    check_int("default smoke disabled", config.smoke_enabled, 0);
    check_int("default watched pin count", count, 3);
    check_pin_present(pins, count, 33, 1);
    check_pin_present(pins, count, 40, 1);
    check_pin_present(pins, count, 41, 0);
    check_pin_present(pins, count, 72, 0);
    check_pin_present(pins, count, 32, 1);
    check_pin_present(pins, count, 54, 0);
    check_pin_edge(pins, count, 32, "rising");

    config.shutter_count = 1;
    config.panic_enabled = 1;
    config.smoke_enabled = 0;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("SHUTTER_COUNT=1 watched pin count", count, 3);
    check_pin_present(pins, count, 33, 1);
    check_pin_present(pins, count, 40, 1);
    check_pin_present(pins, count, 41, 0);
    check_pin_present(pins, count, 72, 0);
    check_pin_present(pins, count, 32, 1);
    check_pin_present(pins, count, 54, 0);
    check_pin_edge(pins, count, 32, "rising");

    config.shutter_count = 2;
    config.panic_enabled = 1;
    config.smoke_enabled = 0;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("SHUTTER_COUNT=2 watched pin count", count, 5);
    check_pin_present(pins, count, 33, 1);
    check_pin_present(pins, count, 40, 1);
    check_pin_present(pins, count, 41, 1);
    check_pin_present(pins, count, 72, 1);
    check_pin_present(pins, count, 32, 1);
    check_pin_present(pins, count, 54, 0);
    check_pin_edge(pins, count, 33, "both");
    check_pin_edge(pins, count, 40, "both");
    check_pin_edge(pins, count, 41, "both");
    check_pin_edge(pins, count, 72, "both");
    check_pin_edge(pins, count, 32, "rising");

    config.shutter_count = 2;
    config.panic_enabled = 0;
    config.smoke_enabled = 0;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("PANIC_ENABLED=0 watched pin count", count, 4);
    check_pin_present(pins, count, 32, 0);

    config.shutter_count = 2;
    config.panic_enabled = 1;
    config.smoke_enabled = 1;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("SMOKE_ENABLED=1 watched pin count", count, 6);
    check_pin_present(pins, count, 54, 1);
    check_pin_edge(pins, count, 54, "both");

    config.shutter_count = 2;
    config.panic_enabled = 1;
    config.smoke_enabled = 0;
    count = runtime_build_watched_pins(&config, pins, AMTECH_RUNTIME_MAX_WATCHED_PINS);

    check_int("SMOKE_ENABLED=0 watched pin count", count, 5);
    check_pin_present(pins, count, 54, 0);

    check_shutter2_ignored_when_single_shutter();
    check_panic_active_high();
    check_sensor_confirmation_logic();
    check_sms_remote_control();
    check_sms_reply_failures_are_not_reported_as_sent();
    check_sms_manual_override_blocks_schedule_until_boundary();
    check_app_command_manual_override_blocks_schedule_until_boundary();
    check_persisted_armed_state_survives_restart_until_schedule_boundary();
    check_persisted_disarmed_state_survives_restart_until_schedule_boundary();
    check_camera_monitoring_active_sms();
    check_device_command_sync_parser_and_ack();
    check_config_sync_preserves_unrelated_keys();

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
