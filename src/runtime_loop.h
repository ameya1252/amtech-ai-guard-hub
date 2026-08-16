#ifndef AMTECH_RUNTIME_LOOP_H
#define AMTECH_RUNTIME_LOOP_H

#include "config.h"
#include "camera_detection.h"
#include "sensor_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_RUNTIME_MAX_WATCHED_PINS 6
#define AMTECH_RUNTIME_MAX_CAMERAS 2
#define AMTECH_STATIC_CALIBRATION_MS 60000U

typedef struct
{
    int pin;
    const char *name;
    const char *edge;
} runtime_watched_pin_t;

typedef struct
{
    int enabled;
    const char *source;
    const char *event_type;
    const char *rtsp_url;
} runtime_camera_config_t;

int runtime_build_watched_pins(const amtech_config_t *config,
                               runtime_watched_pin_t watched_pins[],
                               int max_watched_pins);
int runtime_build_camera_configs(const amtech_config_t *config,
                                 runtime_camera_config_t cameras[],
                                 int max_cameras);
int runtime_process_configured_shutters(const amtech_config_t *config);
void runtime_process_camera_detection_result(const camera_detection_result_t *result);
int runtime_poll_sms_remote_control(const amtech_config_t *config);
int runtime_panic_triggered_from_raw(int raw_value);
int runtime_active_high_confirmed_from_raw_sequence(int initial_raw_value, int confirmed_raw_value);
int runtime_active_low_confirmed_from_raw_sequence(int initial_raw_value, int confirmed_raw_value);
shutter_state_t runtime_confirmed_shutter_state_from_raw_sequence(int initial_nc_raw,
                                                                  int initial_no_raw,
                                                                  int confirmed_nc_raw,
                                                                  int confirmed_no_raw);

#ifdef AMTECH_RUNTIME_LOOP_TEST
int runtime_test_camera_queue_fifo_drop_oldest(void);
void runtime_test_set_armed(int armed);
void runtime_test_tick(unsigned int elapsed_ms);
int runtime_test_static_calibration_active(void);
void runtime_test_note_camera_health(const char *source, int success);
#endif

#ifdef __cplusplus
}
#endif

#endif
