#include "alarm_logic.h"
#include "camera_detection.h"
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

    camera_detection_set_simulated_result_for_source("front", 1, 0.82f);
    check_int("front simulated camera run",
              camera_detection_run_once_for_source("front",
                                                   "intrusion-front",
                                                   "rtsp://example/front",
                                                   NULL,
                                                   &result),
              0);
    check_int("front simulated camera person detected", result.person_detected, 1);
    check_float_text("front simulated camera confidence", result.max_confidence, "0.820");
    check_int("front simulated source tag",
              strcmp(result.source, "front") == 0,
              1);
    check_int("front simulated event tag",
              strcmp(result.event_type, "intrusion-front") == 0,
              1);

    camera_detection_set_simulated_result_for_source("parking", 1, 0.73f);
    check_int("parking simulated camera run",
              camera_detection_run_once_for_source("parking",
                                                   "intrusion-parking",
                                                   "rtsp://example/parking",
                                                   NULL,
                                                   &result),
              0);
    check_int("parking simulated camera person detected", result.person_detected, 1);
    check_float_text("parking simulated camera confidence", result.max_confidence, "0.730");
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
    alarm_logic_set_armed(1);

    camera_detection_set_simulated_result_for_source("front", 1, 0.82f);
    camera_detection_set_simulated_result_for_source("parking", 1, 0.73f);
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
    check_int("first front camera frame does not trigger yet", alarm_logic_is_triggered(), 0);
    apply_camera_frame_to_alarm(&parking);
    check_int("first parking frame does not combine with front frame", alarm_logic_is_triggered(), 0);

    apply_camera_frame_to_alarm(&front);
    check_int("second front frame triggers front-source alarm", alarm_logic_is_triggered(), 1);
    check_int("front-source camera detection turns siren ON/LOW", gpio_get_simulated_value(42), 0);
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
    check_legacy_camera_wrapper();

    if (failures == 0)
    {
        printf("PASS: simulated camera detection behaved as expected\n");
        return 0;
    }

    printf("FAIL: simulated camera detection had %d failure(s)\n", failures);
    return 1;
}
