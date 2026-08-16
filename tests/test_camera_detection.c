#include "alarm_logic.h"
#include "camera_detection.h"
#include "config.h"
#include "gpio_control.h"
#include "runtime_loop.h"

#include <pthread.h>
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

typedef struct
{
    const char *source;
    const char *event_type;
    const char *rtsp_url;
    pthread_mutex_t *inference_mutex;
    camera_detection_result_t result;
    int rc;
} camera_thread_test_context_t;

static void *run_camera_test_thread(void *arg)
{
    camera_thread_test_context_t *context = (camera_thread_test_context_t *)arg;

    context->rc = camera_detection_run_once_for_source(context->source,
                                                       context->event_type,
                                                       context->rtsp_url,
                                                       context->inference_mutex,
                                                       &context->result);
    return NULL;
}

static void apply_camera_frame_to_alarm(const camera_detection_result_t *result)
{
    runtime_process_camera_detection_result(result);
}

static void check_camera_source_tags(void)
{
    camera_detection_result_t result;

    camera_detection_set_simulated_result_box_for_source("front", 1, 0.82f, 10, 20, 110, 220);
    check_int("front simulated camera run",
              camera_detection_run_once_for_source("front",
                                                   "intrusion-front",
                                                   "rtsp://example/front",
                                                   NULL,
                                                   &result),
              0);
    check_int("front simulated camera person detected", result.person_detected, 1);
    check_float_text("front simulated camera confidence", result.max_confidence, "0.820");
    check_int("front simulated camera bbox valid", result.person_box_valid, 1);
    check_int("front simulated camera bbox x1", result.person_x1, 10);
    check_int("front simulated camera bbox y1", result.person_y1, 20);
    check_int("front simulated camera bbox x2", result.person_x2, 110);
    check_int("front simulated camera bbox y2", result.person_y2, 220);
    check_int("front simulated source tag",
              strcmp(result.source, "front") == 0,
              1);
    check_int("front simulated event tag",
              strcmp(result.event_type, "intrusion-front") == 0,
              1);

    camera_detection_set_simulated_result_box_for_source("parking", 1, 0.73f, 210, 40, 340, 300);
    check_int("parking simulated camera run",
              camera_detection_run_once_for_source("parking",
                                                   "intrusion-parking",
                                                   "rtsp://example/parking",
                                                   NULL,
                                                   &result),
              0);
    check_int("parking simulated camera person detected", result.person_detected, 1);
    check_float_text("parking simulated camera confidence", result.max_confidence, "0.730");
    check_int("parking simulated camera bbox valid", result.person_box_valid, 1);
    check_int("parking simulated camera bbox x1", result.person_x1, 210);
    check_int("parking simulated camera bbox y1", result.person_y1, 40);
    check_int("parking simulated camera bbox x2", result.person_x2, 340);
    check_int("parking simulated camera bbox y2", result.person_y2, 300);
    check_int("parking simulated source tag",
              strcmp(result.source, "parking") == 0,
              1);
    check_int("parking simulated event tag",
              strcmp(result.event_type, "intrusion-parking") == 0,
              1);
}

static void check_inference_mutex_serializes_camera_threads(void)
{
    pthread_mutex_t inference_mutex;
    pthread_t front_thread;
    pthread_t parking_thread;
    camera_thread_test_context_t front;
    camera_thread_test_context_t parking;

    memset(&front, 0, sizeof(front));
    memset(&parking, 0, sizeof(parking));
    front.source = "front";
    front.event_type = "intrusion-front";
    front.rtsp_url = "rtsp://example/front";
    front.inference_mutex = &inference_mutex;
    parking.source = "parking";
    parking.event_type = "intrusion-parking";
    parking.rtsp_url = "rtsp://example/parking";
    parking.inference_mutex = &inference_mutex;
    camera_detection_set_simulated_result_for_source("front", 1, 0.82f);
    camera_detection_set_simulated_result_for_source("parking", 1, 0.73f);
    camera_detection_reset_simulated_metrics();
    camera_detection_set_simulated_delay_us(100000);
    pthread_mutex_init(&inference_mutex, NULL);

    pthread_create(&front_thread, NULL, run_camera_test_thread, &front);
    pthread_create(&parking_thread, NULL, run_camera_test_thread, &parking);
    pthread_join(front_thread, NULL);
    pthread_join(parking_thread, NULL);
    pthread_mutex_destroy(&inference_mutex);

    check_int("front threaded camera run succeeds", front.rc, 0);
    check_int("parking threaded camera run succeeds", parking.rc, 0);
    check_int("shared inference mutex keeps max concurrency at 1",
              camera_detection_get_simulated_max_concurrent_inference(),
              1);
    check_int("front threaded result keeps source",
              strcmp(front.result.source, "front") == 0,
              1);
    check_int("parking threaded result keeps source",
              strcmp(parking.result.source, "parking") == 0,
              1);

    camera_detection_set_simulated_delay_us(0);
}

static void check_cross_camera_frames_do_not_mix(void)
{
    camera_detection_result_t front;
    camera_detection_result_t parking;

    gpio_reset_simulated_values();
    alarm_logic_init(42);
    runtime_test_set_armed(0);
    runtime_test_set_armed(1);
    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);

    camera_detection_set_simulated_result_box_for_source("front", 1, 0.82f, 10, 20, 110, 220);
    camera_detection_set_simulated_result_box_for_source("parking", 1, 0.73f, 210, 40, 340, 300);
    camera_detection_run_once_for_source("front",
                                         "intrusion-front",
                                         "rtsp://example/front",
                                         NULL,
                                         &front);
    camera_detection_run_once_for_source("parking",
                                         "intrusion-parking",
                                         "rtsp://example/parking",
                                         NULL,
                                         &parking);

    apply_camera_frame_to_alarm(&front);
    check_int("single front camera frame does not trigger front-source alarm", alarm_logic_is_triggered(), 0);
    apply_camera_frame_to_alarm(&front);
    check_int("second front camera frame triggers front-source alarm", alarm_logic_is_triggered(), 1);
    check_int("front-source camera detection turns siren ON/LOW", gpio_get_simulated_value(42), 0);

    alarm_logic_reset();
    runtime_test_set_armed(0);
    runtime_test_set_armed(1);
    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);
    apply_camera_frame_to_alarm(&parking);
    check_int("single parking camera frame does not trigger parking-source alarm", alarm_logic_is_triggered(), 0);
    apply_camera_frame_to_alarm(&parking);
    check_int("second parking camera frame triggers parking-source alarm", alarm_logic_is_triggered(), 1);
    check_int("parking-source camera detection turns siren ON/LOW", gpio_get_simulated_value(42), 0);
}

static void check_static_zone_filters_same_location_after_calibration(void)
{
    camera_detection_result_t parking_static;
    camera_detection_result_t parking_moved;
    int i;

    gpio_reset_simulated_values();
    alarm_logic_init(42);
    runtime_test_set_armed(0);
    runtime_test_set_armed(1);

    camera_detection_set_simulated_result_box_for_source("parking", 1, 0.84f, 200, 60, 330, 360);
    camera_detection_run_once_for_source("parking",
                                         "intrusion-parking",
                                         "rtsp://example/parking",
                                         NULL,
                                         &parking_static);

    for (i = 0; i < 3; i++)
    {
        apply_camera_frame_to_alarm(&parking_static);
    }
    check_int("static calibration suppresses repeated parking detections", alarm_logic_is_triggered(), 0);
    check_int("static calibration remains active before 60 seconds", runtime_test_static_calibration_active(), 1);

    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);
    check_int("static calibration ends after 60 seconds", runtime_test_static_calibration_active(), 0);

    apply_camera_frame_to_alarm(&parking_static);
    apply_camera_frame_to_alarm(&parking_static);
    check_int("post-calibration same parking box is filtered", alarm_logic_is_triggered(), 0);

    camera_detection_set_simulated_result_box_for_source("parking", 1, 0.84f, 20, 60, 150, 360);
    camera_detection_run_once_for_source("parking",
                                         "intrusion-parking",
                                         "rtsp://example/parking",
                                         NULL,
                                         &parking_moved);
    apply_camera_frame_to_alarm(&parking_moved);
    check_int("different parking box first frame does not trigger", alarm_logic_is_triggered(), 0);
    apply_camera_frame_to_alarm(&parking_moved);
    check_int("different parking box second frame triggers normally", alarm_logic_is_triggered(), 1);
}

static void check_static_zones_are_independent_per_camera(void)
{
    camera_detection_result_t front_static;
    camera_detection_result_t parking_static;
    int i;

    gpio_reset_simulated_values();
    alarm_logic_init(42);
    runtime_test_set_armed(0);
    runtime_test_set_armed(1);

    camera_detection_set_simulated_result_box_for_source("parking", 1, 0.81f, 200, 60, 330, 360);
    camera_detection_run_once_for_source("parking",
                                         "intrusion-parking",
                                         "rtsp://example/parking",
                                         NULL,
                                         &parking_static);
    for (i = 0; i < 3; i++)
    {
        apply_camera_frame_to_alarm(&parking_static);
    }

    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);

    camera_detection_set_simulated_result_box_for_source("front", 1, 0.81f, 200, 60, 330, 360);
    camera_detection_run_once_for_source("front",
                                         "intrusion-front",
                                         "rtsp://example/front",
                                         NULL,
                                         &front_static);

    apply_camera_frame_to_alarm(&front_static);
    check_int("parking static zone does not filter same box on front first frame", alarm_logic_is_triggered(), 0);
    apply_camera_frame_to_alarm(&front_static);
    check_int("parking static zone does not filter same box on front second frame", alarm_logic_is_triggered(), 1);

    alarm_logic_reset();
    runtime_test_set_armed(0);
    runtime_test_set_armed(1);
    camera_detection_set_simulated_result_box_for_source("front", 1, 0.82f, 10, 20, 110, 220);
    camera_detection_run_once_for_source("front",
                                         "intrusion-front",
                                         "rtsp://example/front",
                                         NULL,
                                         &front_static);
    for (i = 0; i < 3; i++)
    {
        apply_camera_frame_to_alarm(&front_static);
    }
    runtime_test_tick(AMTECH_STATIC_CALIBRATION_MS);

    apply_camera_frame_to_alarm(&front_static);
    apply_camera_frame_to_alarm(&front_static);
    check_int("front static zone filters front box independently", alarm_logic_is_triggered(), 0);
}

static void check_camera_detection_pauses_while_disarmed(void)
{
    amtech_config_t config;

    amtech_config_set_defaults(&config);
    config.camera_enabled = 1;
    snprintf(config.camera_rtsp_url, sizeof(config.camera_rtsp_url), "%s", "rtsp://example/front");

    gpio_reset_simulated_values();
    alarm_logic_init(42);
    runtime_test_set_armed(0);
    camera_detection_reset_simulated_metrics();
    camera_detection_set_simulated_result_box_for_source("front", 1, 0.82f, 10, 20, 110, 220);

    runtime_test_run_simulated_camera_once(&config);
    check_int("disarmed runtime reports camera detection paused", runtime_test_camera_detection_should_run(), 0);
    check_int("disarmed runtime skips simulated camera capture", camera_detection_get_simulated_run_count(), 0);

    runtime_test_set_armed(1);
    runtime_test_run_simulated_camera_once(&config);
    check_int("armed runtime reports camera detection active", runtime_test_camera_detection_should_run(), 1);
    check_int("armed runtime runs simulated camera capture", camera_detection_get_simulated_run_count(), 1);

    runtime_test_set_armed(0);
    runtime_test_run_simulated_camera_once(&config);
    check_int("disarmed runtime pauses camera capture again", camera_detection_get_simulated_run_count(), 1);
}

static void check_legacy_camera_wrapper(void)
{
    camera_detection_result_t result;

    camera_detection_set_simulated_result(1, 0.82f);
    check_int("legacy simulated camera run",
              camera_detection_run_once("rtsp://example/stream1", &result),
              0);
    check_int("legacy simulated camera person detected", result.person_detected, 1);
    check_float_text("legacy simulated camera confidence", result.max_confidence, "0.820");
    check_int("legacy simulated source defaults to front",
              strcmp(result.source, "front") == 0,
              1);
    check_int("legacy simulated event defaults to intrusion",
              strcmp(result.event_type, "intrusion") == 0,
              1);
}

int main(void)
{
    check_camera_source_tags();
    check_inference_mutex_serializes_camera_threads();
    check_cross_camera_frames_do_not_mix();
    check_static_zone_filters_same_location_after_calibration();
    check_static_zones_are_independent_per_camera();
    check_camera_detection_pauses_while_disarmed();
    check_legacy_camera_wrapper();

    if (failures == 0)
    {
        printf("PASS: simulated camera detection behaved as expected\n");
        return 0;
    }

    printf("FAIL: simulated camera detection had %d failure(s)\n", failures);
    return 1;
}
