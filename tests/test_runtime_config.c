#include "config.h"
#include "alarm_logic.h"
#include "gpio_control.h"
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
