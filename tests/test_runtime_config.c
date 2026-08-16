#include "config.h"
#include "alarm_logic.h"
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
    check_string("contact 1 ARM reply text", modem_get_simulated_last_sms_message(), "System ARMED");
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

    alarm_logic_set_armed(1);

    modem_simulate_incoming_sms("+919999999999", "DISARM");
    check_int("unknown sender SMS ignored", runtime_poll_sms_remote_control(&config), 0);
    check_int("unknown sender does not disarm", alarm_logic_is_armed(), 1);
    check_int("unknown sender gets no reply", modem_get_simulated_sms_count(), sms_count_after_trigger + 2);
    check_int("unknown sender SMS still deleted", modem_get_simulated_deleted_sms_count(), 8);

    modem_simulate_incoming_sms("+911111111111", "STATUS");
    check_int("malformed valid-sender SMS ignored", runtime_poll_sms_remote_control(&config), 0);
    check_int("malformed SMS leaves armed unchanged", alarm_logic_is_armed(), 1);
    check_int("malformed SMS gets no reply", modem_get_simulated_sms_count(), sms_count_after_trigger + 2);
    check_int("malformed SMS still deleted", modem_get_simulated_deleted_sms_count(), 9);

    modem_make_voice_call("+911111111111");
    modem_simulate_incoming_sms("+911111111111", "DISARM");
    check_int("SMS poll still runs while voice call active", runtime_poll_sms_remote_control(&config), 1);
    check_int("SMS deleted while call active", modem_get_simulated_deleted_sms_count(), 10);
    check_int("DISARM clears armed while call active", alarm_logic_is_armed(), 0);
    check_int("DISARM reply sent while call active", modem_get_simulated_sms_count(), sms_count_after_trigger + 3);
    modem_hangup_voice_call();
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

    fp = fopen(two_shutter_config_path, "w");
    if (fp == NULL)
    {
        printf("FAIL: could not write test config file\n");
        return 1;
    }
    fprintf(fp, "SHUTTER_COUNT=2\nPANIC_ENABLED=1\nSMOKE_ENABLED=1\nMODEM_DEVICE=/dev/ttyS1\nALERT_CONTACT_1=+919999999991\nALERT_CONTACT_2=+919999999992\nALERT_CONTACT_3=+919999999993\nCAMERA_ENABLED=1\nCAMERA_RTSP_URL=rtsp://user:pass@192.168.0.2:554/stream1\nCAMERA2_ENABLED=1\nCAMERA2_RTSP_URL=rtsp://user:pass@192.168.0.4:554/stream1\n");
    fclose(fp);

    check_int("two-shutter config load", amtech_config_load(two_shutter_config_path, &config), 0);
    check_int("two-shutter config shutter count", config.shutter_count, 2);
    check_int("two-shutter config panic enabled", config.panic_enabled, 1);
    check_int("two-shutter config smoke enabled", config.smoke_enabled, 1);
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
