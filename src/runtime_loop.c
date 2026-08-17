#include "alarm_logic.h"
#include "camera_detection.h"
#include "config.h"
#include "config_sync.h"
#include "device_command_sync.h"
#include "gpio_control.h"
#include "modem_hal.h"
#include "modem_state.h"
#include "runtime_loop.h"
#include "schedule.h"
#include "sensor_input.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SIMULATE_GPIO
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#endif

#define AMTECH_ALARM_GPIO_PIN 49
#define AMTECH_STROBE_GPIO_PIN 48
#define AMTECH_SHUTTER_NC_GPIO_PIN 33
#define AMTECH_SHUTTER_NO_GPIO_PIN 40
#define AMTECH_SHUTTER2_NC_GPIO_PIN 41
#define AMTECH_SHUTTER2_NO_GPIO_PIN 72
#define AMTECH_PANIC_GPIO_PIN 32
#define AMTECH_SMOKE_GPIO_PIN 54
#define AMTECH_RUNTIME_TEST_ITERATIONS 10
#define AMTECH_DEBOUNCE_CONFIRM_MS 200
#define AMTECH_GPIO_POLL_TIMEOUT_MS 100
#define AMTECH_SMS_COMMAND_POLL_MS 5000
#define AMTECH_CAMERA_RETRY_DELAY_SECONDS 1
#define AMTECH_CAMERA_QUEUE_CAPACITY 8
#define AMTECH_STATIC_ZONE_IOU_THRESHOLD 0.80f
#define AMTECH_STATIC_ZONE_MIN_OCCURRENCES 3
#define AMTECH_STATIC_ZONE_MAX_PER_CAMERA 8
#define AMTECH_SMS_REPLY_MAX 256
#define AMTECH_CAMERA_MONITORING_ACTIVE_SMS "System ARMED"

typedef enum
{
    RUNTIME_ARM_CONTROL_SCHEDULE = 0,
    RUNTIME_ARM_CONTROL_MANUAL
} runtime_arm_control_t;

typedef enum
{
    WATCH_SHUTTER1_NC = 0,
    WATCH_SHUTTER1_NO,
    WATCH_SHUTTER2_NC,
    WATCH_SHUTTER2_NO,
    WATCH_PANIC,
    WATCH_SMOKE
} watched_pin_role_t;

typedef struct
{
    int pin;
    watched_pin_role_t role;
    const char *name;
    const char *edge;
    int shutter_nc_pin;
    int shutter_no_pin;
    const char *shutter_name;
    const char *shutter_event_type;
    int fd;
    long long last_event_ms;
} gpio_watch_t;

typedef struct
{
    int valid;
    int x1;
    int y1;
    int x2;
    int y2;
    int seen_count;
} runtime_static_zone_t;

typedef struct
{
    int used;
    char source[AMTECH_CAMERA_SOURCE_MAX];
    runtime_static_zone_t zones[AMTECH_STATIC_ZONE_MAX_PER_CAMERA];
} runtime_camera_static_state_t;

typedef struct
{
    int used;
    char source[AMTECH_CAMERA_SOURCE_MAX];
    int success_count;
    int failure_count;
    int last_success;
} runtime_camera_health_t;

#ifndef SIMULATE_GPIO
typedef struct
{
    pthread_mutex_t mutex;
    int stop_requested;
    int has_command;
    amtech_device_command_t command;
    amtech_config_t config;
    char shop_id[AMTECH_DEVICE_COMMAND_SHOP_ID_MAX];
} runtime_device_command_context_t;

#ifndef SIMULATE_CAMERA
typedef struct
{
    pthread_mutex_t mutex;
    camera_detection_result_t results[AMTECH_CAMERA_QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
    unsigned int dropped_results;
} camera_result_queue_t;

typedef struct
{
    char source[AMTECH_CAMERA_SOURCE_MAX];
    char event_type[AMTECH_CAMERA_EVENT_TYPE_MAX];
    char rtsp_url[AMTECH_CAMERA_RTSP_URL_MAX];
    camera_result_queue_t *queue;
    pthread_mutex_t *inference_mutex;
    volatile int stop_requested;
} camera_thread_context_t;
#endif
#endif

static const gpio_watch_t SHUTTER1_NC_WATCH = {
    AMTECH_SHUTTER_NC_GPIO_PIN, WATCH_SHUTTER1_NC, "shutter-1 NC", "both",
    AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN, "shutter-1", "shutter-1", -1, 0};
static const gpio_watch_t SHUTTER1_NO_WATCH = {
    AMTECH_SHUTTER_NO_GPIO_PIN, WATCH_SHUTTER1_NO, "shutter-1 NO", "both",
    AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN, "shutter-1", "shutter-1", -1, 0};
static const gpio_watch_t SHUTTER2_NC_WATCH = {
    AMTECH_SHUTTER2_NC_GPIO_PIN, WATCH_SHUTTER2_NC, "shutter-2 NC", "both",
    AMTECH_SHUTTER2_NC_GPIO_PIN, AMTECH_SHUTTER2_NO_GPIO_PIN, "shutter-2", "shutter-2", -1, 0};
static const gpio_watch_t SHUTTER2_NO_WATCH = {
    AMTECH_SHUTTER2_NO_GPIO_PIN, WATCH_SHUTTER2_NO, "shutter-2 NO", "both",
    AMTECH_SHUTTER2_NC_GPIO_PIN, AMTECH_SHUTTER2_NO_GPIO_PIN, "shutter-2", "shutter-2", -1, 0};
static const gpio_watch_t PANIC_WATCH = {
    AMTECH_PANIC_GPIO_PIN, WATCH_PANIC, "panic", "rising",
    -1, -1, NULL, NULL, -1, 0};
static const gpio_watch_t SMOKE_WATCH = {
    AMTECH_SMOKE_GPIO_PIN, WATCH_SMOKE, "smoke", "both",
    -1, -1, NULL, NULL, -1, 0};

static runtime_camera_static_state_t static_camera_states[AMTECH_RUNTIME_MAX_CAMERAS];
static runtime_camera_health_t camera_health_states[AMTECH_RUNTIME_MAX_CAMERAS];
static pthread_mutex_t camera_health_mutex = PTHREAD_MUTEX_INITIALIZER;
static int static_calibration_active = 0;
static unsigned int static_calibration_elapsed_ms = AMTECH_STATIC_CALIBRATION_MS;
static int runtime_observed_armed = 0;
static runtime_arm_control_t runtime_arm_control = RUNTIME_ARM_CONTROL_SCHEDULE;
static int runtime_last_schedule_known = 0;
static int runtime_last_schedule_armed = 0;
static int runtime_calibration_completion_sms_pending = 0;
static char runtime_calibration_completion_contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX];

static int add_watch(gpio_watch_t watches[], int max_watches, int *count, const gpio_watch_t *watch)
{
    if (*count >= max_watches)
    {
        printf("Runtime: too many GPIO watches configured\n");
        return -1;
    }

    watches[*count] = *watch;
    (*count)++;
    return 0;
}

static int build_gpio_watches(const amtech_config_t *config, gpio_watch_t watches[], int max_watches)
{
    int count = 0;

    if (config == NULL)
    {
        return -1;
    }

    if (add_watch(watches, max_watches, &count, &SHUTTER1_NC_WATCH) != 0 ||
        add_watch(watches, max_watches, &count, &SHUTTER1_NO_WATCH) != 0)
    {
        return -1;
    }

    if (config->shutter_count >= 2)
    {
        if (add_watch(watches, max_watches, &count, &SHUTTER2_NC_WATCH) != 0 ||
            add_watch(watches, max_watches, &count, &SHUTTER2_NO_WATCH) != 0)
        {
            return -1;
        }
    }

    if (config->panic_enabled)
    {
        if (add_watch(watches, max_watches, &count, &PANIC_WATCH) != 0)
        {
            return -1;
        }
    }

    if (config->smoke_enabled)
    {
        if (add_watch(watches, max_watches, &count, &SMOKE_WATCH) != 0)
        {
            return -1;
        }
    }

    return count;
}

int runtime_build_watched_pins(const amtech_config_t *config,
                               runtime_watched_pin_t watched_pins[],
                               int max_watched_pins)
{
    gpio_watch_t watches[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    int count;
    int i;

    if (watched_pins == NULL || max_watched_pins <= 0)
    {
        return -1;
    }

    count = build_gpio_watches(config, watches, AMTECH_RUNTIME_MAX_WATCHED_PINS);
    if (count < 0 || count > max_watched_pins)
    {
        return -1;
    }

    for (i = 0; i < count; i++)
    {
        watched_pins[i].pin = watches[i].pin;
        watched_pins[i].name = watches[i].name;
        watched_pins[i].edge = watches[i].edge;
    }

    return count;
}

static int add_camera_config(runtime_camera_config_t cameras[],
                             int max_cameras,
                             int *count,
                             int enabled,
                             const char *source,
                             const char *event_type,
                             const char *rtsp_url)
{
    if (!enabled)
    {
        return 0;
    }

    if (rtsp_url == NULL || rtsp_url[0] == '\0')
    {
        printf("Runtime: camera %s enabled but RTSP URL is empty; not starting\n", source);
        return 0;
    }

    if (*count >= max_cameras)
    {
        printf("Runtime: too many cameras configured\n");
        return -1;
    }

    cameras[*count].enabled = 1;
    cameras[*count].source = source;
    cameras[*count].event_type = event_type;
    cameras[*count].rtsp_url = rtsp_url;
    (*count)++;
    return 0;
}

int runtime_build_camera_configs(const amtech_config_t *config,
                                 runtime_camera_config_t cameras[],
                                 int max_cameras)
{
    int count = 0;

    if (config == NULL || cameras == NULL || max_cameras <= 0)
    {
        return -1;
    }

    if (add_camera_config(cameras,
                          max_cameras,
                          &count,
                          config->camera_enabled,
                          "front",
                          "intrusion-front",
                          config->camera_rtsp_url) != 0)
    {
        return -1;
    }

    if (add_camera_config(cameras,
                          max_cameras,
                          &count,
                          config->camera2_enabled,
                          "parking",
                          "intrusion-parking",
                          config->camera2_rtsp_url) != 0)
    {
        return -1;
    }

    return count;
}

int runtime_panic_triggered_from_raw(int raw_value)
{
    return raw_value ? 1 : 0;
}

int runtime_active_high_confirmed_from_raw_sequence(int initial_raw_value, int confirmed_raw_value)
{
    if (initial_raw_value == 0)
    {
        return 0;
    }

    return confirmed_raw_value != 0 ? 1 : 0;
}

int runtime_active_low_confirmed_from_raw_sequence(int initial_raw_value, int confirmed_raw_value)
{
    if (initial_raw_value != 0)
    {
        return 0;
    }

    return confirmed_raw_value == 0 ? 1 : 0;
}

shutter_state_t runtime_confirmed_shutter_state_from_raw_sequence(int initial_nc_raw,
                                                                  int initial_no_raw,
                                                                  int confirmed_nc_raw,
                                                                  int confirmed_no_raw)
{
    int nc_triggered;
    int no_triggered;

    (void)initial_nc_raw;
    (void)initial_no_raw;

    nc_triggered = confirmed_nc_raw == 0 ? 1 : 0;
    no_triggered = confirmed_no_raw == 0 ? 1 : 0;

    if (nc_triggered && !no_triggered)
    {
        return SHUTTER_CLOSED;
    }
    if (!nc_triggered && no_triggered)
    {
        return SHUTTER_OPEN;
    }
    if (!nc_triggered && !no_triggered)
    {
        return SHUTTER_TAMPER;
    }
    return SHUTTER_FAULT;
}

int runtime_process_configured_shutters(const amtech_config_t *config)
{
    shutter_state_t shutter_state;

    if (config == NULL)
    {
        return -1;
    }

    shutter_state = shutter_read_dual_state(AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN);
    alarm_logic_handle_shutter_dual_named(shutter_state, "shutter-1", "shutter-1");

    if (config->shutter_count >= 2)
    {
        shutter_state = shutter_read_dual_state(AMTECH_SHUTTER2_NC_GPIO_PIN, AMTECH_SHUTTER2_NO_GPIO_PIN);
        alarm_logic_handle_shutter_dual_named(shutter_state, "shutter-2", "shutter-2");
    }

    return 0;
}

static int env_force_armed_enabled(void)
{
    const char *value = getenv("AMTECH_FORCE_ARMED");

    return value != NULL &&
           (strcmp(value, "1") == 0 ||
            strcmp(value, "true") == 0 ||
            strcmp(value, "TRUE") == 0 ||
            strcmp(value, "yes") == 0 ||
            strcmp(value, "YES") == 0);
}

static int parse_force_armed_arg(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--force-armed") == 0)
        {
            return 1;
        }

        printf("Runtime: unknown argument %s\n", argv[i]);
        printf("Usage: %s [--force-armed]\n", argv[0]);
        return -1;
    }

    return env_force_armed_enabled();
}

static void print_force_armed_warning(void)
{
    printf("WARNING: FORCE-ARMED TEST MODE - NOT FOR PRODUCTION USE\n");
    printf("WARNING: Schedule checks are disabled and the system is forced ARMED\n");
}

static const char *runtime_config_path(void)
{
    const char *path = getenv("AMTECH_CONFIG_PATH");

    if (path != NULL && path[0] != '\0')
    {
        return path;
    }

    return AMTECH_DEFAULT_CONFIG_PATH;
}

static void runtime_static_calibration_clear(void)
{
    memset(static_camera_states, 0, sizeof(static_camera_states));
}

static void runtime_clear_calibration_completion_sms(void)
{
    runtime_calibration_completion_sms_pending = 0;
    memset(runtime_calibration_completion_contacts, 0, sizeof(runtime_calibration_completion_contacts));
}

static void runtime_prepare_calibration_completion_sms(const amtech_config_t *config)
{
    int i;

    runtime_clear_calibration_completion_sms();

    if (config == NULL)
    {
        return;
    }

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        snprintf(runtime_calibration_completion_contacts[i],
                 sizeof(runtime_calibration_completion_contacts[i]),
                 "%s",
                 config->alert_contacts[i]);
        if (runtime_calibration_completion_contacts[i][0] != '\0')
        {
            runtime_calibration_completion_sms_pending = 1;
        }
    }
}

static void runtime_send_calibration_completion_sms(void)
{
    int i;

    if (!runtime_calibration_completion_sms_pending)
    {
        return;
    }

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        if (runtime_calibration_completion_contacts[i][0] == '\0')
        {
            continue;
        }

        modem_send_sms(runtime_calibration_completion_contacts[i],
                       AMTECH_CAMERA_MONITORING_ACTIVE_SMS);
    }

    printf("Runtime: sent camera monitoring active SMS to configured alert contacts\n");
    runtime_clear_calibration_completion_sms();
}

static void runtime_static_calibration_start(const amtech_config_t *config)
{
    runtime_static_calibration_clear();
    runtime_prepare_calibration_completion_sms(config);
    static_calibration_active = 1;
    static_calibration_elapsed_ms = 0;
    printf("Runtime: camera static-scene calibration started for %u ms\n",
           AMTECH_STATIC_CALIBRATION_MS);
}

static void runtime_static_calibration_stop(void)
{
    if (static_calibration_active)
    {
        printf("Runtime: camera static-scene calibration stopped before completion\n");
    }
    static_calibration_active = 0;
    static_calibration_elapsed_ms = AMTECH_STATIC_CALIBRATION_MS;
    runtime_clear_calibration_completion_sms();
    runtime_static_calibration_clear();
}

static void runtime_static_calibration_tick(unsigned int elapsed_ms)
{
    if (!static_calibration_active)
    {
        return;
    }

    if (elapsed_ms > AMTECH_STATIC_CALIBRATION_MS - static_calibration_elapsed_ms)
    {
        static_calibration_elapsed_ms = AMTECH_STATIC_CALIBRATION_MS;
    }
    else
    {
        static_calibration_elapsed_ms += elapsed_ms;
    }

    if (static_calibration_elapsed_ms >= AMTECH_STATIC_CALIBRATION_MS)
    {
        static_calibration_active = 0;
        printf("Runtime: camera static-scene calibration completed\n");
        runtime_send_calibration_completion_sms();
    }
}

static void runtime_note_armed_state(const amtech_config_t *config)
{
    int is_armed = alarm_logic_is_armed();

    if (!runtime_observed_armed && is_armed)
    {
        runtime_static_calibration_start(config);
    }
    else if (runtime_observed_armed && !is_armed)
    {
        runtime_static_calibration_stop();
    }

    runtime_observed_armed = is_armed;
}

static void runtime_set_armed(int next_armed, const amtech_config_t *config)
{
    alarm_logic_set_armed(next_armed);
    runtime_note_armed_state(config);
}

static void runtime_apply_schedule_armed(int scheduled_armed, const amtech_config_t *config)
{
    if (!runtime_last_schedule_known)
    {
        runtime_last_schedule_known = 1;
        runtime_last_schedule_armed = scheduled_armed ? 1 : 0;
    }
    else if (runtime_last_schedule_armed != (scheduled_armed ? 1 : 0))
    {
        runtime_last_schedule_armed = scheduled_armed ? 1 : 0;
        if (runtime_arm_control == RUNTIME_ARM_CONTROL_MANUAL)
        {
            printf("Runtime: schedule boundary reached; clearing SMS manual arm override\n");
            runtime_arm_control = RUNTIME_ARM_CONTROL_SCHEDULE;
        }
    }

    if (runtime_arm_control == RUNTIME_ARM_CONTROL_MANUAL)
    {
        return;
    }

    runtime_set_armed(scheduled_armed, config);
}

static void runtime_set_manual_armed(int next_armed, const amtech_config_t *config)
{
    runtime_arm_control = RUNTIME_ARM_CONTROL_MANUAL;
    runtime_set_armed(next_armed, config);
}

static void runtime_apply_config_schedule(const amtech_config_t *config)
{
    if (config == NULL)
    {
        return;
    }

    schedule_set_armed_window(config->schedule_arm_hour,
                              config->schedule_arm_minute,
                              config->schedule_disarm_hour,
                              config->schedule_disarm_minute);
    printf("Runtime: schedule window set from config %02d:%02d-%02d:%02d\n",
           config->schedule_arm_hour,
           config->schedule_arm_minute,
           config->schedule_disarm_hour,
           config->schedule_disarm_minute);
}

static runtime_camera_static_state_t *runtime_find_static_camera(const char *source, int create)
{
    int i;
    int empty_index = -1;

    if (source == NULL || source[0] == '\0')
    {
        source = "front";
    }

    for (i = 0; i < AMTECH_RUNTIME_MAX_CAMERAS; i++)
    {
        if (static_camera_states[i].used &&
            strcmp(static_camera_states[i].source, source) == 0)
        {
            return &static_camera_states[i];
        }

        if (!static_camera_states[i].used && empty_index < 0)
        {
            empty_index = i;
        }
    }

    if (!create || empty_index < 0)
    {
        return NULL;
    }

    static_camera_states[empty_index].used = 1;
    snprintf(static_camera_states[empty_index].source,
             sizeof(static_camera_states[empty_index].source),
             "%s",
             source);
    memset(static_camera_states[empty_index].zones,
           0,
           sizeof(static_camera_states[empty_index].zones));
    return &static_camera_states[empty_index];
}

static float runtime_box_iou(int ax1, int ay1, int ax2, int ay2, int bx1, int by1, int bx2, int by2)
{
    int ix1 = ax1 > bx1 ? ax1 : bx1;
    int iy1 = ay1 > by1 ? ay1 : by1;
    int ix2 = ax2 < bx2 ? ax2 : bx2;
    int iy2 = ay2 < by2 ? ay2 : by2;
    int intersection_width = ix2 - ix1;
    int intersection_height = iy2 - iy1;
    int intersection_area;
    int area_a;
    int area_b;
    int union_area;

    if (intersection_width <= 0 || intersection_height <= 0 ||
        ax2 <= ax1 || ay2 <= ay1 || bx2 <= bx1 || by2 <= by1)
    {
        return 0.0f;
    }

    intersection_area = intersection_width * intersection_height;
    area_a = (ax2 - ax1) * (ay2 - ay1);
    area_b = (bx2 - bx1) * (by2 - by1);
    union_area = area_a + area_b - intersection_area;
    if (union_area <= 0)
    {
        return 0.0f;
    }

    return (float)intersection_area / (float)union_area;
}

static void runtime_static_calibration_learn(const camera_detection_result_t *result)
{
    runtime_camera_static_state_t *camera_state;
    int i;
    int empty_index = -1;

    if (result == NULL || !result->person_detected || !result->person_box_valid)
    {
        return;
    }

    camera_state = runtime_find_static_camera(result->source, 1);
    if (camera_state == NULL)
    {
        return;
    }

    for (i = 0; i < AMTECH_STATIC_ZONE_MAX_PER_CAMERA; i++)
    {
        runtime_static_zone_t *zone = &camera_state->zones[i];

        if (!zone->valid)
        {
            if (empty_index < 0)
            {
                empty_index = i;
            }
            continue;
        }

        if (runtime_box_iou(zone->x1,
                            zone->y1,
                            zone->x2,
                            zone->y2,
                            result->person_x1,
                            result->person_y1,
                            result->person_x2,
                            result->person_y2) >= AMTECH_STATIC_ZONE_IOU_THRESHOLD)
        {
            zone->seen_count++;
            if (zone->seen_count == AMTECH_STATIC_ZONE_MIN_OCCURRENCES)
            {
                printf("Runtime: confirmed static zone camera=%s box=(%d,%d,%d,%d) seen=%d\n",
                       result->source,
                       zone->x1,
                       zone->y1,
                       zone->x2,
                       zone->y2,
                       zone->seen_count);
            }
            return;
        }
    }

    if (empty_index < 0)
    {
        printf("Runtime: static-zone table full for camera=%s; cannot learn box=(%d,%d,%d,%d)\n",
               result->source,
               result->person_x1,
               result->person_y1,
               result->person_x2,
               result->person_y2);
        return;
    }

    camera_state->zones[empty_index].valid = 1;
    camera_state->zones[empty_index].x1 = result->person_x1;
    camera_state->zones[empty_index].y1 = result->person_y1;
    camera_state->zones[empty_index].x2 = result->person_x2;
    camera_state->zones[empty_index].y2 = result->person_y2;
    camera_state->zones[empty_index].seen_count = 1;
    printf("Runtime: learned static-zone candidate camera=%s box=(%d,%d,%d,%d)\n",
           result->source,
           result->person_x1,
           result->person_y1,
           result->person_x2,
           result->person_y2);
}

static int runtime_camera_detection_matches_static_zone(const camera_detection_result_t *result)
{
    runtime_camera_static_state_t *camera_state;
    int i;

    if (result == NULL || !result->person_detected || !result->person_box_valid)
    {
        return 0;
    }

    camera_state = runtime_find_static_camera(result->source, 0);
    if (camera_state == NULL)
    {
        return 0;
    }

    for (i = 0; i < AMTECH_STATIC_ZONE_MAX_PER_CAMERA; i++)
    {
        runtime_static_zone_t *zone = &camera_state->zones[i];

        if (!zone->valid || zone->seen_count < AMTECH_STATIC_ZONE_MIN_OCCURRENCES)
        {
            continue;
        }

        if (runtime_box_iou(zone->x1,
                            zone->y1,
                            zone->x2,
                            zone->y2,
                            result->person_x1,
                            result->person_y1,
                            result->person_x2,
                            result->person_y2) >= AMTECH_STATIC_ZONE_IOU_THRESHOLD)
        {
            printf("Runtime: ignored static-zone detection at camera=%s box=(%d,%d,%d,%d)\n",
                   result->source,
                   result->person_x1,
                   result->person_y1,
                   result->person_x2,
                   result->person_y2);
            return 1;
        }
    }

    return 0;
}

static runtime_camera_health_t *runtime_find_camera_health(const char *source, int create)
{
    int i;
    int empty_index = -1;

    if (source == NULL || source[0] == '\0')
    {
        source = "front";
    }

    for (i = 0; i < AMTECH_RUNTIME_MAX_CAMERAS; i++)
    {
        if (camera_health_states[i].used &&
            strcmp(camera_health_states[i].source, source) == 0)
        {
            return &camera_health_states[i];
        }

        if (!camera_health_states[i].used && empty_index < 0)
        {
            empty_index = i;
        }
    }

    if (!create || empty_index < 0)
    {
        return NULL;
    }

    camera_health_states[empty_index].used = 1;
    snprintf(camera_health_states[empty_index].source,
             sizeof(camera_health_states[empty_index].source),
             "%s",
             source);
    camera_health_states[empty_index].success_count = 0;
    camera_health_states[empty_index].failure_count = 0;
    camera_health_states[empty_index].last_success = 0;
    return &camera_health_states[empty_index];
}

static void runtime_camera_health_note(const char *source, int success)
{
    runtime_camera_health_t *health;

    pthread_mutex_lock(&camera_health_mutex);
    health = runtime_find_camera_health(source, 1);
    if (health != NULL)
    {
        if (success)
        {
            health->success_count++;
            health->last_success = 1;
        }
        else
        {
            health->failure_count++;
            health->last_success = 0;
        }
    }
    pthread_mutex_unlock(&camera_health_mutex);
}

static void runtime_camera_health_text(const char *source, char *buffer, size_t buffer_size)
{
    runtime_camera_health_t snapshot;
    runtime_camera_health_t *health;

    if (buffer == NULL || buffer_size == 0)
    {
        return;
    }
    snprintf(buffer, buffer_size, "%s", "no frame yet");

    pthread_mutex_lock(&camera_health_mutex);
    health = runtime_find_camera_health(source, 0);
    if (health != NULL)
    {
        snapshot = *health;
        if (snapshot.success_count > 0 || snapshot.failure_count > 0)
        {
            snprintf(buffer,
                     buffer_size,
                     "%s",
                     snapshot.last_success ? "recent OK" : "failing");
        }
    }
    pthread_mutex_unlock(&camera_health_mutex);
}

static int runtime_camera_detection_should_run(void)
{
    return alarm_logic_is_armed();
}

static void runtime_build_camera_status(const amtech_config_t *config,
                                        int camera_index,
                                        char *buffer,
                                        size_t buffer_size)
{
    const char *source = camera_index == 2 ? "parking" : "front";
    int enabled = camera_index == 2 ? config->camera2_enabled : config->camera_enabled;
    const char *rtsp_url = camera_index == 2 ? config->camera2_rtsp_url : config->camera_rtsp_url;

    if (!enabled || rtsp_url == NULL || rtsp_url[0] == '\0')
    {
        snprintf(buffer, buffer_size, "off");
        return;
    }

    runtime_camera_health_text(source, buffer, buffer_size);
}

static void runtime_build_status_message(const amtech_config_t *config, char *buffer, size_t buffer_size)
{
    char front_camera[32];
    char parking_camera[32];
    const char *modem_name;
    int modem_state;

    if (buffer == NULL || buffer_size == 0)
    {
        return;
    }
    buffer[0] = '\0';

    if (config == NULL)
    {
        snprintf(buffer, buffer_size, "Status unavailable");
        return;
    }

    runtime_build_camera_status(config, 1, front_camera, sizeof(front_camera));
    runtime_build_camera_status(config, 2, parking_camera, sizeof(parking_camera));
    modem_state = modem_get_registration_status();
    modem_name = modem_state_name((modem_state_t)modem_state);

    snprintf(buffer,
             buffer_size,
             "%s; Panic %s; Sh1 cfg; Sh2 %s; Smoke %s; Front cam %s; Parking cam %s; Modem %s last-known",
             alarm_logic_is_armed() ? "ARMED" : "DISARMED",
             config->panic_enabled ? "cfg" : "off",
             config->shutter_count >= 2 ? "cfg" : "off",
             config->smoke_enabled ? "cfg" : "off",
             front_camera,
             parking_camera,
             modem_name);
}

static void runtime_build_help_message(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0)
    {
        return;
    }

    snprintf(buffer,
             buffer_size,
             "ARM - Arm system; DISARM - Disarm system; STOP - Stop active alarm & disarm; STATUS - System status report; HELP - This message");
}

void runtime_process_camera_detection_result(const camera_detection_result_t *result)
{
    if (result == NULL)
    {
        return;
    }

    runtime_note_armed_state(NULL);

    printf("Runtime: camera source=%s event=%s frame person=%d confidence=%.3f box_valid=%d box=(%d,%d,%d,%d)\n",
           result->source,
           result->event_type,
           result->person_detected,
           result->max_confidence,
           result->person_box_valid,
           result->person_x1,
           result->person_y1,
           result->person_x2,
           result->person_y2);

    if (alarm_logic_is_armed() && static_calibration_active)
    {
        runtime_static_calibration_learn(result);
        printf("Runtime: camera detection suppressed during static-scene calibration (%u/%u ms)\n",
               static_calibration_elapsed_ms,
               AMTECH_STATIC_CALIBRATION_MS);
        alarm_logic_end_frame_source(result->event_type);
        return;
    }

    if (runtime_camera_detection_matches_static_zone(result))
    {
        alarm_logic_end_frame_source(result->event_type);
        return;
    }

    if (result->person_detected)
    {
        alarm_logic_handle_detection_source(0,
                                            "person",
                                            result->max_confidence,
                                            result->event_type);
    }

    alarm_logic_end_frame_source(result->event_type);
}

#ifdef AMTECH_RUNTIME_LOOP_TEST
void runtime_test_set_armed(int armed)
{
    runtime_arm_control = RUNTIME_ARM_CONTROL_SCHEDULE;
    runtime_last_schedule_known = 0;
    runtime_last_schedule_armed = 0;
    runtime_set_armed(armed, NULL);
}

void runtime_test_apply_schedule_armed(int armed)
{
    runtime_apply_schedule_armed(armed, NULL);
}

void runtime_test_apply_schedule_armed_with_config(int armed, const amtech_config_t *config)
{
    runtime_apply_schedule_armed(armed, config);
}

void runtime_test_apply_app_command(amtech_device_command_type_t command, const amtech_config_t *config)
{
    if (command == AMTECH_DEVICE_COMMAND_ARM)
    {
        runtime_set_manual_armed(1, config);
    }
    else if (command == AMTECH_DEVICE_COMMAND_DISARM)
    {
        runtime_set_manual_armed(0, config);
    }
}

void runtime_test_tick(unsigned int elapsed_ms)
{
    alarm_logic_tick(elapsed_ms);
    runtime_static_calibration_tick(elapsed_ms);
}

int runtime_test_static_calibration_active(void)
{
    return static_calibration_active;
}

void runtime_test_note_camera_health(const char *source, int success)
{
    runtime_camera_health_note(source, success);
}

int runtime_test_camera_detection_should_run(void)
{
    return runtime_camera_detection_should_run();
}
#endif

#ifdef SIMULATE_GPIO
static void trim_runtime_text(char *text)
{
    size_t length;

    if (text == NULL)
    {
        return;
    }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    {
        memmove(text, text + 1, strlen(text));
    }

    length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == ' ' ||
            text[length - 1] == '\t' ||
            text[length - 1] == '\r' ||
            text[length - 1] == '\n'))
    {
        text[length - 1] = '\0';
        length--;
    }
}

static void uppercase_runtime_text(char *text)
{
    while (text != NULL && *text != '\0')
    {
        *text = (char)toupper((unsigned char)*text);
        text++;
    }
}

static void normalize_phone_number(const char *input, char *output, size_t output_size)
{
    char digits[MODEM_SMS_SENDER_MAX];
    size_t digit_count = 0;
    size_t start = 0;
    size_t copy_length;
    size_t i;

    if (output == NULL || output_size == 0)
    {
        return;
    }
    output[0] = '\0';

    if (input == NULL)
    {
        return;
    }

    for (i = 0; input[i] != '\0' && digit_count < sizeof(digits) - 1; i++)
    {
        if (isdigit((unsigned char)input[i]))
        {
            digits[digit_count++] = input[i];
        }
    }
    digits[digit_count] = '\0';

    if (digit_count >= 2 && digits[0] == '0' && digits[1] == '0')
    {
        start = 2;
    }

    if (digit_count - start > 10)
    {
        start = digit_count - 10;
    }

    copy_length = digit_count - start;
    if (copy_length >= output_size)
    {
        copy_length = output_size - 1;
    }

    memcpy(output, digits + start, copy_length);
    output[copy_length] = '\0';
}

static int sms_sender_is_authorized(const amtech_config_t *config, const char *sender)
{
    int i;
    char normalized_sender[MODEM_SMS_SENDER_MAX];
    char normalized_contact[MODEM_SMS_SENDER_MAX];

    if (config == NULL || sender == NULL)
    {
        return 0;
    }

    normalize_phone_number(sender, normalized_sender, sizeof(normalized_sender));
    if (normalized_sender[0] == '\0')
    {
        return 0;
    }

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        normalize_phone_number(config->alert_contacts[i], normalized_contact, sizeof(normalized_contact));
        if (normalized_contact[0] != '\0' &&
            strcmp(normalized_sender, normalized_contact) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int process_sms_remote_command(const amtech_config_t *config, const modem_incoming_sms_t *sms)
{
    char command[MODEM_SMS_TEXT_MAX];

    if (config == NULL || sms == NULL)
    {
        return -1;
    }

    if (!sms_sender_is_authorized(config, sms->sender))
    {
        printf("Runtime: ignored SMS command from unauthorized sender %s\n", sms->sender);
        return 0;
    }

    snprintf(command, sizeof(command), "%s", sms->text);
    trim_runtime_text(command);
    uppercase_runtime_text(command);

    if (strcmp(command, "ARM") == 0)
    {
        if (alarm_logic_is_armed())
        {
            runtime_set_manual_armed(1, config);
            modem_send_sms(sms->sender, "System already ARMED");
            printf("Runtime: accepted redundant SMS ARM command from %s\n", sms->sender);
            return 1;
        }

        runtime_set_manual_armed(1, config);
        modem_send_sms(sms->sender, "System ARMING...");
        printf("Runtime: accepted SMS ARM command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "DISARM") == 0)
    {
        if (!alarm_logic_is_armed())
        {
            runtime_set_manual_armed(0, config);
            modem_send_sms(sms->sender, "System already DISARMED");
            printf("Runtime: accepted redundant SMS DISARM command from %s\n", sms->sender);
            return 1;
        }

        runtime_set_manual_armed(0, config);
        modem_send_sms(sms->sender, "System DISARMED");
        printf("Runtime: accepted SMS DISARM command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "STOP") == 0)
    {
        if (!alarm_logic_is_triggered())
        {
            modem_send_sms(sms->sender, "No active alarm");
            printf("Runtime: accepted SMS STOP command from %s with no active alarm\n", sms->sender);
            return 1;
        }

        alarm_logic_reset();
        runtime_set_manual_armed(0, config);
        modem_send_sms(sms->sender, "Alarm stopped, system DISARMED");
        printf("Runtime: accepted SMS STOP command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "STATUS") == 0)
    {
        char reply[AMTECH_SMS_REPLY_MAX];

        runtime_build_status_message(config, reply, sizeof(reply));
        modem_send_sms(sms->sender, reply);
        printf("Runtime: accepted SMS STATUS command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "HELP") == 0)
    {
        char reply[AMTECH_SMS_REPLY_MAX];

        runtime_build_help_message(reply, sizeof(reply));
        modem_send_sms(sms->sender, reply);
        printf("Runtime: accepted SMS HELP command from %s\n", sms->sender);
        return 1;
    }

    printf("Runtime: ignored unrecognized SMS command from authorized sender %s\n", sms->sender);
    return 0;
}

int runtime_poll_sms_remote_control(const amtech_config_t *config)
{
    modem_incoming_sms_t sms;
    int check_result;
    int process_result;

    if (config == NULL)
    {
        return -1;
    }

    check_result = modem_check_incoming_sms(&sms);
    if (check_result <= 0)
    {
        return check_result;
    }

    process_result = process_sms_remote_command(config, &sms);
    if (modem_delete_sms(sms.index) != 0)
    {
        printf("Runtime: warning: failed to delete processed SMS index %d\n", sms.index);
    }

    return process_result;
}
#endif

#ifndef SIMULATE_GPIO
static long long monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        printf("Runtime: clock_gettime failed: %s\n", strerror(errno));
        return 0;
    }

    return ((long long)now.tv_sec * 1000) + (now.tv_nsec / 1000000);
}

static void *device_command_thread_main(void *arg)
{
    runtime_device_command_context_t *context = (runtime_device_command_context_t *)arg;
    long long last_poll_ms = 0;

    while (1)
    {
        long long now_ms = monotonic_ms();
        int should_stop;

        pthread_mutex_lock(&context->mutex);
        should_stop = context->stop_requested;
        pthread_mutex_unlock(&context->mutex);
        if (should_stop)
        {
            break;
        }

        if (last_poll_ms == 0 || now_ms - last_poll_ms >= AMTECH_DEVICE_COMMAND_POLL_MS)
        {
            amtech_device_command_t command;

            if (amtech_device_command_fetch(&context->config, context->shop_id, &command) == 0 &&
                command.type != AMTECH_DEVICE_COMMAND_NONE)
            {
                pthread_mutex_lock(&context->mutex);
                if (!context->has_command ||
                    strcmp(context->command.id, command.id) != 0)
                {
                    context->command = command;
                    context->has_command = 1;
                    printf("Runtime: received app pending command %s id=%s\n",
                           command.type == AMTECH_DEVICE_COMMAND_ARM ? "arm" : "disarm",
                           command.id);
                }
                pthread_mutex_unlock(&context->mutex);
            }
            last_poll_ms = now_ms;
        }

        usleep(200000);
    }

    return NULL;
}

static int start_device_command_thread(runtime_device_command_context_t *context,
                                       pthread_t *thread,
                                       const amtech_config_t *config,
                                       const char *shop_id)
{
    if (context == NULL || thread == NULL || config == NULL || shop_id == NULL)
    {
        return -1;
    }

    memset(context, 0, sizeof(*context));
    pthread_mutex_init(&context->mutex, NULL);
    context->config = *config;
    snprintf(context->shop_id, sizeof(context->shop_id), "%s", shop_id);

    if (pthread_create(thread, NULL, device_command_thread_main, context) != 0)
    {
        pthread_mutex_destroy(&context->mutex);
        printf("Runtime: failed to start app command polling thread\n");
        return -1;
    }

    printf("Runtime: app command polling thread started interval=%u ms\n", AMTECH_DEVICE_COMMAND_POLL_MS);
    return 0;
}

static void stop_device_command_thread(runtime_device_command_context_t *context,
                                       pthread_t thread,
                                       int started)
{
    if (context == NULL || !started)
    {
        return;
    }

    pthread_mutex_lock(&context->mutex);
    context->stop_requested = 1;
    pthread_mutex_unlock(&context->mutex);
    pthread_join(thread, NULL);
    pthread_mutex_destroy(&context->mutex);
}

static int consume_device_command(runtime_device_command_context_t *context,
                                  amtech_device_command_t *command)
{
    int has_command;

    if (context == NULL || command == NULL)
    {
        return 0;
    }

    pthread_mutex_lock(&context->mutex);
    has_command = context->has_command;
    if (has_command)
    {
        *command = context->command;
        context->has_command = 0;
        context->command.type = AMTECH_DEVICE_COMMAND_NONE;
        context->command.id[0] = '\0';
    }
    pthread_mutex_unlock(&context->mutex);

    return has_command;
}

static void apply_device_command(runtime_device_command_context_t *context,
                                 const amtech_config_t *config)
{
    amtech_device_command_t command;

    if (!consume_device_command(context, &command))
    {
        return;
    }

    if (command.type == AMTECH_DEVICE_COMMAND_ARM)
    {
        runtime_set_manual_armed(1, config);
        printf("Runtime: accepted app ARM command id=%s\n", command.id);
    }
    else if (command.type == AMTECH_DEVICE_COMMAND_DISARM)
    {
        runtime_set_manual_armed(0, config);
        printf("Runtime: accepted app DISARM command id=%s\n", command.id);
    }
    else
    {
        return;
    }

    if (amtech_device_command_ack(config, config->shop_id, &command) != 0)
    {
        printf("Runtime: warning: failed to ack app command id=%s\n", command.id);
    }
}

static void update_schedule_from_realtime(const amtech_config_t *config)
{
    time_t now_seconds;
    struct tm now_local;

    now_seconds = time(NULL);
    if (now_seconds == (time_t)-1)
    {
        printf("Runtime: time failed: %s\n", strerror(errno));
        return;
    }

    if (localtime_r(&now_seconds, &now_local) == NULL)
    {
        printf("Runtime: localtime_r failed\n");
        return;
    }

    runtime_apply_schedule_armed(schedule_should_be_armed(now_local.tm_hour, now_local.tm_min), config);
}

static void trim_runtime_text(char *text)
{
    size_t length;

    if (text == NULL)
    {
        return;
    }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
    {
        memmove(text, text + 1, strlen(text));
    }

    length = strlen(text);
    while (length > 0 &&
           (text[length - 1] == ' ' ||
            text[length - 1] == '\t' ||
            text[length - 1] == '\r' ||
            text[length - 1] == '\n'))
    {
        text[length - 1] = '\0';
        length--;
    }
}

static void uppercase_runtime_text(char *text)
{
    while (text != NULL && *text != '\0')
    {
        *text = (char)toupper((unsigned char)*text);
        text++;
    }
}

static void normalize_phone_number(const char *input, char *output, size_t output_size)
{
    char digits[MODEM_SMS_SENDER_MAX];
    size_t digit_count = 0;
    size_t start = 0;
    size_t copy_length;
    size_t i;

    if (output == NULL || output_size == 0)
    {
        return;
    }
    output[0] = '\0';

    if (input == NULL)
    {
        return;
    }

    for (i = 0; input[i] != '\0' && digit_count < sizeof(digits) - 1; i++)
    {
        if (isdigit((unsigned char)input[i]))
        {
            digits[digit_count++] = input[i];
        }
    }
    digits[digit_count] = '\0';

    if (digit_count >= 2 && digits[0] == '0' && digits[1] == '0')
    {
        start = 2;
    }

    if (digit_count - start > 10)
    {
        start = digit_count - 10;
    }

    copy_length = digit_count - start;
    if (copy_length >= output_size)
    {
        copy_length = output_size - 1;
    }

    memcpy(output, digits + start, copy_length);
    output[copy_length] = '\0';
}

static int sms_sender_is_authorized(const amtech_config_t *config, const char *sender)
{
    int i;
    char normalized_sender[MODEM_SMS_SENDER_MAX];
    char normalized_contact[MODEM_SMS_SENDER_MAX];

    if (config == NULL || sender == NULL)
    {
        return 0;
    }

    normalize_phone_number(sender, normalized_sender, sizeof(normalized_sender));
    if (normalized_sender[0] == '\0')
    {
        return 0;
    }

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        normalize_phone_number(config->alert_contacts[i], normalized_contact, sizeof(normalized_contact));
        if (normalized_contact[0] != '\0' &&
            strcmp(normalized_sender, normalized_contact) == 0)
        {
            return 1;
        }
    }

    return 0;
}

static int process_sms_remote_command(const amtech_config_t *config, const modem_incoming_sms_t *sms)
{
    char command[MODEM_SMS_TEXT_MAX];

    if (config == NULL || sms == NULL)
    {
        return -1;
    }

    if (!sms_sender_is_authorized(config, sms->sender))
    {
        printf("Runtime: ignored SMS command from unauthorized sender %s\n", sms->sender);
        return 0;
    }

    snprintf(command, sizeof(command), "%s", sms->text);
    trim_runtime_text(command);
    uppercase_runtime_text(command);

    if (strcmp(command, "ARM") == 0)
    {
        if (alarm_logic_is_armed())
        {
            runtime_set_manual_armed(1, config);
            modem_send_sms(sms->sender, "System already ARMED");
            printf("Runtime: accepted redundant SMS ARM command from %s\n", sms->sender);
            return 1;
        }

        runtime_set_manual_armed(1, config);
        modem_send_sms(sms->sender, "System ARMING...");
        printf("Runtime: accepted SMS ARM command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "DISARM") == 0)
    {
        if (!alarm_logic_is_armed())
        {
            runtime_set_manual_armed(0, config);
            modem_send_sms(sms->sender, "System already DISARMED");
            printf("Runtime: accepted redundant SMS DISARM command from %s\n", sms->sender);
            return 1;
        }

        runtime_set_manual_armed(0, config);
        modem_send_sms(sms->sender, "System DISARMED");
        printf("Runtime: accepted SMS DISARM command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "STOP") == 0)
    {
        if (!alarm_logic_is_triggered())
        {
            modem_send_sms(sms->sender, "No active alarm");
            printf("Runtime: accepted SMS STOP command from %s with no active alarm\n", sms->sender);
            return 1;
        }

        alarm_logic_reset();
        runtime_set_manual_armed(0, config);
        modem_send_sms(sms->sender, "Alarm stopped, system DISARMED");
        printf("Runtime: accepted SMS STOP command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "STATUS") == 0)
    {
        char reply[AMTECH_SMS_REPLY_MAX];

        runtime_build_status_message(config, reply, sizeof(reply));
        modem_send_sms(sms->sender, reply);
        printf("Runtime: accepted SMS STATUS command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "HELP") == 0)
    {
        char reply[AMTECH_SMS_REPLY_MAX];

        runtime_build_help_message(reply, sizeof(reply));
        modem_send_sms(sms->sender, reply);
        printf("Runtime: accepted SMS HELP command from %s\n", sms->sender);
        return 1;
    }

    printf("Runtime: ignored unrecognized SMS command from authorized sender %s\n", sms->sender);
    return 0;
}

int runtime_poll_sms_remote_control(const amtech_config_t *config)
{
    modem_incoming_sms_t sms;
    int check_result;
    int process_result = 0;

    if (config == NULL)
    {
        return -1;
    }

    check_result = modem_check_incoming_sms(&sms);
    if (check_result <= 0)
    {
        return check_result;
    }

    process_result = process_sms_remote_command(config, &sms);
    if (modem_delete_sms(sms.index) != 0)
    {
        printf("Runtime: warning: failed to delete processed SMS index %d\n", sms.index);
    }

    return process_result;
}

#ifndef SIMULATE_CAMERA
static void camera_queue_init(camera_result_queue_t *queue)
{
    pthread_mutex_init(&queue->mutex, NULL);
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;
    queue->dropped_results = 0;
    memset(queue->results, 0, sizeof(queue->results));
}

static void camera_queue_destroy(camera_result_queue_t *queue)
{
    pthread_mutex_destroy(&queue->mutex);
}

static void camera_queue_publish(camera_result_queue_t *queue, const camera_detection_result_t *result)
{
    pthread_mutex_lock(&queue->mutex);
    if (queue->count == AMTECH_CAMERA_QUEUE_CAPACITY)
    {
        printf("Runtime: camera result queue full, dropping oldest result from %s\n",
               queue->results[queue->head].source);
        queue->head = (queue->head + 1) % AMTECH_CAMERA_QUEUE_CAPACITY;
        queue->count--;
        queue->dropped_results++;
    }

    queue->results[queue->tail] = *result;
    queue->tail = (queue->tail + 1) % AMTECH_CAMERA_QUEUE_CAPACITY;
    queue->count++;
    pthread_mutex_unlock(&queue->mutex);
}

static int camera_queue_consume(camera_result_queue_t *queue, camera_detection_result_t *result)
{
    int has_result = 0;

    pthread_mutex_lock(&queue->mutex);
    if (queue->count > 0)
    {
        *result = queue->results[queue->head];
        queue->head = (queue->head + 1) % AMTECH_CAMERA_QUEUE_CAPACITY;
        queue->count--;
        has_result = 1;
    }
    pthread_mutex_unlock(&queue->mutex);

    return has_result;
}

#ifdef AMTECH_RUNTIME_LOOP_TEST
int runtime_test_camera_queue_fifo_drop_oldest(void)
{
    camera_result_queue_t queue;
    camera_detection_result_t result;
    int i;
    int ok = 1;

    camera_queue_init(&queue);

    for (i = 0; i < AMTECH_CAMERA_QUEUE_CAPACITY + 1; i++)
    {
        camera_detection_result_t item;

        memset(&item, 0, sizeof(item));
        snprintf(item.source, sizeof(item.source), "cam%d", i);
        snprintf(item.event_type, sizeof(item.event_type), "event%d", i);
        item.person_detected = 1;
        item.max_confidence = (float)i;
        camera_queue_publish(&queue, &item);
    }

    if (queue.dropped_results != 1)
    {
        ok = 0;
    }

    for (i = 1; i < AMTECH_CAMERA_QUEUE_CAPACITY + 1; i++)
    {
        if (!camera_queue_consume(&queue, &result))
        {
            ok = 0;
            break;
        }

        {
            char expected_source[AMTECH_CAMERA_SOURCE_MAX];
            snprintf(expected_source, sizeof(expected_source), "cam%d", i);
            if (strcmp(result.source, expected_source) != 0)
            {
                ok = 0;
                break;
            }
        }
    }

    if (camera_queue_consume(&queue, &result))
    {
        ok = 0;
    }

    camera_queue_destroy(&queue);
    return ok ? 0 : -1;
}
#endif

static void *camera_thread_main(void *arg)
{
    camera_thread_context_t *context = (camera_thread_context_t *)arg;

    printf("Runtime: camera detection thread started source=%s\n", context->source);
    while (!context->stop_requested)
    {
        camera_detection_result_t result;

        if (!runtime_camera_detection_should_run())
        {
            sleep(AMTECH_CAMERA_RETRY_DELAY_SECONDS);
            continue;
        }

        if (camera_detection_run_once_for_source(context->source,
                                                 context->event_type,
                                                 context->rtsp_url,
                                                 context->inference_mutex,
                                                 &result) == 0)
        {
            runtime_camera_health_note(context->source, 1);
            camera_queue_publish(context->queue, &result);
        }
        else
        {
            runtime_camera_health_note(context->source, 0);
            printf("Runtime: camera detection cycle failed source=%s\n", context->source);
            sleep(AMTECH_CAMERA_RETRY_DELAY_SECONDS);
        }
    }

    printf("Runtime: camera detection thread stopped source=%s\n", context->source);
    return NULL;
}

static int start_camera_thread(const runtime_camera_config_t *camera,
                               camera_result_queue_t *queue,
                               pthread_mutex_t *inference_mutex,
                               camera_thread_context_t *context,
                               pthread_t *thread)
{
    snprintf(context->source, sizeof(context->source), "%s", camera->source);
    snprintf(context->event_type, sizeof(context->event_type), "%s", camera->event_type);
    snprintf(context->rtsp_url, sizeof(context->rtsp_url), "%s", camera->rtsp_url);
    context->queue = queue;
    context->inference_mutex = inference_mutex;
    context->stop_requested = 0;

    if (pthread_create(thread, NULL, camera_thread_main, context) != 0)
    {
        printf("Runtime: failed to start camera detection thread source=%s\n", camera->source);
        return -1;
    }

    return 0;
}

static void stop_camera_threads(camera_thread_context_t contexts[],
                                pthread_t threads[],
                                int started_count)
{
    int i;

    for (i = 0; i < started_count; i++)
    {
        contexts[i].stop_requested = 1;
    }

    for (i = 0; i < started_count; i++)
    {
        pthread_join(threads[i], NULL);
    }
}
#else
static void maybe_run_simulated_camera_once(const amtech_config_t *config)
{
    runtime_camera_config_t cameras[AMTECH_RUNTIME_MAX_CAMERAS];
    int camera_count;
    int i;

    camera_count = runtime_build_camera_configs(config, cameras, AMTECH_RUNTIME_MAX_CAMERAS);
    if (camera_count <= 0)
    {
        return;
    }

    if (!runtime_camera_detection_should_run())
    {
        return;
    }

    for (i = 0; i < camera_count; i++)
    {
        camera_detection_result_t result;

        if (camera_detection_run_once_for_source(cameras[i].source,
                                                 cameras[i].event_type,
                                                 cameras[i].rtsp_url,
                                                 NULL,
                                                 &result) == 0)
        {
            runtime_camera_health_note(cameras[i].source, 1);
            runtime_process_camera_detection_result(&result);
        }
        else
        {
            runtime_camera_health_note(cameras[i].source, 0);
        }
    }
}

#ifdef AMTECH_RUNTIME_LOOP_TEST
void runtime_test_run_simulated_camera_once(const amtech_config_t *config)
{
    maybe_run_simulated_camera_once(config);
}
#endif
#endif

static void tick_schedule_elapsed_seconds(long long *last_tick_ms)
{
    long long now_ms = monotonic_ms();

    if (*last_tick_ms == 0)
    {
        *last_tick_ms = now_ms;
        return;
    }

    while (now_ms - *last_tick_ms >= 1000)
    {
        schedule_tick();
        *last_tick_ms += 1000;
    }
}

static int read_gpio_value_fd(int fd, int *raw_value)
{
    char value = 0;

    if (lseek(fd, 0, SEEK_SET) < 0)
    {
        printf("Runtime: failed to seek GPIO value fd: %s\n", strerror(errno));
        return -1;
    }

    if (read(fd, &value, 1) != 1)
    {
        printf("Runtime: failed to read GPIO value fd: %s\n", strerror(errno));
        return -1;
    }

    *raw_value = value == '0' ? 0 : 1;
    return 0;
}

static int open_gpio_value_fd(int pin)
{
    char path[128];
    int fd;
    int raw_value;

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    fd = open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("Runtime: failed to open %s: %s\n", path, strerror(errno));
        return -1;
    }

    if (read_gpio_value_fd(fd, &raw_value) != 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static int setup_gpio_watch(gpio_watch_t *watch)
{
    if (sensor_input_init(watch->pin) != 0)
    {
        return -1;
    }

    if (gpio_set_edge(watch->pin, watch->edge) != 0)
    {
        printf("Runtime: failed to configure %s edge for %s GPIO %d\n",
               watch->edge,
               watch->name,
               watch->pin);
        return -1;
    }

    watch->fd = open_gpio_value_fd(watch->pin);
    if (watch->fd < 0)
    {
        return -1;
    }

    watch->last_event_ms = 0;
    return 0;
}

static int confirm_watch_raw_value(gpio_watch_t *watch, int *confirmed_raw_value)
{
    printf("Runtime: %s GPIO %d confirming for %d ms\n",
           watch->name,
           watch->pin,
           AMTECH_DEBOUNCE_CONFIRM_MS);
    usleep(AMTECH_DEBOUNCE_CONFIRM_MS * 1000);

    if (read_gpio_value_fd(watch->fd, confirmed_raw_value) != 0)
    {
        return -1;
    }

    printf("Runtime: %s GPIO %d confirmed raw=%d after %d ms\n",
           watch->name,
           watch->pin,
           *confirmed_raw_value,
           AMTECH_DEBOUNCE_CONFIRM_MS);
    return 0;
}

static void close_gpio_watches(gpio_watch_t watches[], int count)
{
    int i;

    for (i = 0; i < count; i++)
    {
        if (watches[i].fd >= 0)
        {
            close(watches[i].fd);
            watches[i].fd = -1;
        }
    }
}

static void handle_sensor_event(gpio_watch_t *watch)
{
    long long now_ms = monotonic_ms();
    long long elapsed_ms = now_ms - watch->last_event_ms;
    int raw_value;
    int confirmed_raw_value;
    int panic_triggered;
    shutter_state_t shutter_state;

    if (watch->last_event_ms != 0 && elapsed_ms >= 0 && elapsed_ms < AMTECH_DEBOUNCE_CONFIRM_MS)
    {
        printf("Runtime: ignored %s GPIO %d bounce after %lld ms\n",
               watch->name,
               watch->pin,
               elapsed_ms);
        return;
    }
    watch->last_event_ms = now_ms;

    if (read_gpio_value_fd(watch->fd, &raw_value) != 0)
    {
        return;
    }

    printf("Runtime: %s GPIO %d edge event raw=%d\n", watch->name, watch->pin, raw_value);

    if (watch->role == WATCH_PANIC)
    {
        if (confirm_watch_raw_value(watch, &confirmed_raw_value) != 0)
        {
            return;
        }

        panic_triggered = runtime_active_high_confirmed_from_raw_sequence(raw_value, confirmed_raw_value);
        if (!panic_triggered)
        {
            printf("Runtime: ignored panic GPIO %d transient state\n", watch->pin);
        }
        alarm_logic_handle_panic(panic_triggered);
        return;
    }

    if (watch->role == WATCH_SMOKE)
    {
        if (raw_value != 0)
        {
            return;
        }

        if (confirm_watch_raw_value(watch, &confirmed_raw_value) != 0)
        {
            return;
        }

        if (runtime_active_low_confirmed_from_raw_sequence(raw_value, confirmed_raw_value))
        {
            alarm_logic_handle_smoke(1);
        }
        else
        {
            printf("Runtime: ignored smoke GPIO %d transient LOW blip\n", watch->pin);
        }
        return;
    }

    if (confirm_watch_raw_value(watch, &confirmed_raw_value) != 0)
    {
        return;
    }

    shutter_state = shutter_read_dual_state(watch->shutter_nc_pin, watch->shutter_no_pin);
    alarm_logic_handle_shutter_dual_named(shutter_state,
                                          watch->shutter_name,
                                          watch->shutter_event_type);
}

static int run_interrupt_loop(int force_armed, amtech_config_t *config)
{
    /*
     * Shutter uses both edges because wire-cut tamper from the closed state is
     * NC LOW -> HIGH with NO already HIGH, so falling-only would not wake us.
     */
    gpio_watch_t watches[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    struct pollfd poll_fds[AMTECH_RUNTIME_MAX_WATCHED_PINS];
    long long last_schedule_tick_ms = 0;
    long long last_alarm_tick_ms = 0;
    long long last_sms_poll_ms = 0;
    long long last_config_sync_ms = 0;
    runtime_device_command_context_t device_command_context;
    pthread_t device_command_thread;
    int device_command_thread_started = 0;
#ifndef SIMULATE_CAMERA
    camera_result_queue_t camera_queue;
    runtime_camera_config_t camera_configs[AMTECH_RUNTIME_MAX_CAMERAS];
    camera_thread_context_t camera_contexts[AMTECH_RUNTIME_MAX_CAMERAS];
    pthread_t camera_threads[AMTECH_RUNTIME_MAX_CAMERAS];
    pthread_mutex_t inference_mutex;
    int camera_count = 0;
    int camera_threads_started = 0;
#endif
    int watch_count;
    int i;

    if (start_device_command_thread(&device_command_context,
                                    &device_command_thread,
                                    config,
                                    config->shop_id) == 0)
    {
        device_command_thread_started = 1;
    }
    else
    {
        printf("Runtime: warning: app command polling disabled\n");
    }

#ifndef SIMULATE_CAMERA
    camera_queue_init(&camera_queue);
    pthread_mutex_init(&inference_mutex, NULL);
    camera_count = runtime_build_camera_configs(config, camera_configs, AMTECH_RUNTIME_MAX_CAMERAS);
    if (camera_count < 0)
    {
        stop_device_command_thread(&device_command_context, device_command_thread, device_command_thread_started);
        pthread_mutex_destroy(&inference_mutex);
        camera_queue_destroy(&camera_queue);
        return -1;
    }
    if (camera_count == 0)
    {
        printf("Runtime: camera detection disabled; no enabled camera has an RTSP URL\n");
    }
    for (i = 0; i < camera_count; i++)
    {
        if (start_camera_thread(&camera_configs[i],
                                &camera_queue,
                                &inference_mutex,
                                &camera_contexts[i],
                                &camera_threads[i]) != 0)
        {
            stop_device_command_thread(&device_command_context, device_command_thread, device_command_thread_started);
            stop_camera_threads(camera_contexts, camera_threads, camera_threads_started);
            pthread_mutex_destroy(&inference_mutex);
            camera_queue_destroy(&camera_queue);
            return -1;
        }
        camera_threads_started++;
    }
#else
    {
        runtime_camera_config_t camera_configs[AMTECH_RUNTIME_MAX_CAMERAS];
        int camera_count = runtime_build_camera_configs(config, camera_configs, AMTECH_RUNTIME_MAX_CAMERAS);

        if (camera_count <= 0)
        {
            printf("Runtime: camera detection disabled; no enabled camera has an RTSP URL\n");
        }
        else
        {
            printf("Runtime: SIMULATE_CAMERA enabled; no camera thread, ffmpeg, or RTSP access\n");
        }
    }
#endif

    watch_count = build_gpio_watches(config, watches, AMTECH_RUNTIME_MAX_WATCHED_PINS);
    if (watch_count < 0)
    {
#ifndef SIMULATE_CAMERA
        stop_camera_threads(camera_contexts, camera_threads, camera_threads_started);
        pthread_mutex_destroy(&inference_mutex);
        camera_queue_destroy(&camera_queue);
#endif
        stop_device_command_thread(&device_command_context, device_command_thread, device_command_thread_started);
        return -1;
    }

    for (i = 0; i < watch_count; i++)
    {
        if (setup_gpio_watch(&watches[i]) != 0)
        {
            close_gpio_watches(watches, watch_count);
#ifndef SIMULATE_CAMERA
            stop_camera_threads(camera_contexts, camera_threads, camera_threads_started);
            pthread_mutex_destroy(&inference_mutex);
            camera_queue_destroy(&camera_queue);
#endif
            stop_device_command_thread(&device_command_context, device_command_thread, device_command_thread_started);
            return -1;
        }

        poll_fds[i].fd = watches[i].fd;
        poll_fds[i].events = POLLPRI | POLLERR;
        poll_fds[i].revents = 0;
    }

    for (;;)
    {
        int ready;
        long long now_ms;

        now_ms = monotonic_ms();
        if (last_alarm_tick_ms != 0 && now_ms >= last_alarm_tick_ms)
        {
            unsigned int elapsed_ms = (unsigned int)(now_ms - last_alarm_tick_ms);
            alarm_logic_tick(elapsed_ms);
            runtime_static_calibration_tick(elapsed_ms);
        }
        last_alarm_tick_ms = now_ms;

        if (!force_armed)
        {
            tick_schedule_elapsed_seconds(&last_schedule_tick_ms);
            update_schedule_from_realtime(config);
        }

        if (last_sms_poll_ms == 0 || now_ms - last_sms_poll_ms >= AMTECH_SMS_COMMAND_POLL_MS)
        {
            runtime_poll_sms_remote_control(config);
            last_sms_poll_ms = now_ms;
        }

        apply_device_command(&device_command_context, config);

        if (last_config_sync_ms == 0 || now_ms - last_config_sync_ms >= AMTECH_CONFIG_SYNC_POLL_MS)
        {
            if (amtech_config_sync_poll(runtime_config_path(), config->shop_id, config) > 0)
            {
                runtime_apply_config_schedule(config);
            }
            last_config_sync_ms = now_ms;
        }

#ifdef SIMULATE_CAMERA
        maybe_run_simulated_camera_once(config);
#else
        {
            camera_detection_result_t camera_result;

            while (camera_queue_consume(&camera_queue, &camera_result))
            {
                runtime_process_camera_detection_result(&camera_result);
            }
        }
#endif

        ready = poll(poll_fds, watch_count, AMTECH_GPIO_POLL_TIMEOUT_MS);
        if (ready < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            printf("Runtime: poll failed: %s\n", strerror(errno));
            close_gpio_watches(watches, watch_count);
#ifndef SIMULATE_CAMERA
            stop_camera_threads(camera_contexts, camera_threads, camera_threads_started);
            pthread_mutex_destroy(&inference_mutex);
            camera_queue_destroy(&camera_queue);
#endif
            return -1;
        }

        if (ready == 0)
        {
            continue;
        }

        for (i = 0; i < watch_count; i++)
        {
            if (poll_fds[i].revents & (POLLPRI | POLLERR))
            {
                handle_sensor_event(&watches[i]);
            }
            poll_fds[i].revents = 0;
        }
    }
}
#endif

#ifdef SIMULATE_GPIO
static void runtime_iteration(int iteration, int force_armed, const amtech_config_t *config)
{
    int panic_triggered;
    int panic_raw_value;
    int should_be_armed;
    int camera_processed = 0;

    printf("Runtime: iteration %d\n", iteration);

    if (!force_armed)
    {
        schedule_tick();

        should_be_armed = schedule_should_be_armed(23, 30);
        runtime_set_armed(should_be_armed, config);
    }

    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NC_GPIO_PIN, iteration == 4 ? 1 : 0);
    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NO_GPIO_PIN, iteration == 4 ? 0 : 1);
    if (config->shutter_count >= 2)
    {
        sensor_input_set_simulated_raw_value(AMTECH_SHUTTER2_NC_GPIO_PIN, iteration == 5 ? 1 : 0);
        sensor_input_set_simulated_raw_value(AMTECH_SHUTTER2_NO_GPIO_PIN, iteration == 5 ? 0 : 1);
    }

    runtime_process_configured_shutters(config);

    if (config->panic_enabled)
    {
        panic_raw_value = iteration == 6 ? 1 : 0;
        sensor_input_set_simulated_raw_value(AMTECH_PANIC_GPIO_PIN, panic_raw_value);
        printf("Runtime: panic GPIO %d raw %d\n", AMTECH_PANIC_GPIO_PIN, panic_raw_value);
        panic_triggered = runtime_panic_triggered_from_raw(panic_raw_value);
        alarm_logic_handle_panic(panic_triggered);
    }

    if (config->smoke_enabled)
    {
        int smoke_initial_raw = iteration == 7 ? 0 : 1;
        int smoke_confirmed_raw = iteration == 7 ? 0 : 1;

        sensor_input_set_simulated_raw_value(AMTECH_SMOKE_GPIO_PIN, smoke_confirmed_raw);
        printf("Runtime: smoke GPIO %d initial raw %d confirmed raw %d\n",
               AMTECH_SMOKE_GPIO_PIN,
               smoke_initial_raw,
               smoke_confirmed_raw);
        alarm_logic_handle_smoke(runtime_active_low_confirmed_from_raw_sequence(smoke_initial_raw,
                                                                                smoke_confirmed_raw));
    }

#ifdef SIMULATE_CAMERA
    {
        runtime_camera_config_t camera_configs[AMTECH_RUNTIME_MAX_CAMERAS];
        int camera_count;
        int camera_index;

        camera_count = runtime_build_camera_configs(config, camera_configs, AMTECH_RUNTIME_MAX_CAMERAS);
        for (camera_index = 0; camera_index < camera_count; camera_index++)
        {
            camera_detection_result_t camera_result;

            if (camera_detection_run_once_for_source(camera_configs[camera_index].source,
                                                     camera_configs[camera_index].event_type,
                                                     camera_configs[camera_index].rtsp_url,
                                                     NULL,
                                                     &camera_result) == 0)
            {
                runtime_camera_health_note(camera_configs[camera_index].source, 1);
                runtime_process_camera_detection_result(&camera_result);
                camera_processed = 1;
            }
            else
            {
                runtime_camera_health_note(camera_configs[camera_index].source, 0);
            }
        }
    }
#endif

    if (!camera_processed)
    {
        alarm_logic_end_frame();
    }
    alarm_logic_tick(1000);
    runtime_static_calibration_tick(1000);
    runtime_poll_sms_remote_control(config);
}

#if defined(AMTECH_RUNTIME_LOOP_TEST) && defined(SIMULATE_CAMERA)
void runtime_test_run_simulated_camera_once(const amtech_config_t *config)
{
    runtime_camera_config_t camera_configs[AMTECH_RUNTIME_MAX_CAMERAS];
    int camera_count;
    int camera_index;

    if (!runtime_camera_detection_should_run())
    {
        return;
    }

    camera_count = runtime_build_camera_configs(config, camera_configs, AMTECH_RUNTIME_MAX_CAMERAS);
    for (camera_index = 0; camera_index < camera_count; camera_index++)
    {
        camera_detection_result_t camera_result;

        if (camera_detection_run_once_for_source(camera_configs[camera_index].source,
                                                 camera_configs[camera_index].event_type,
                                                 camera_configs[camera_index].rtsp_url,
                                                 NULL,
                                                 &camera_result) == 0)
        {
            runtime_camera_health_note(camera_configs[camera_index].source, 1);
            runtime_process_camera_detection_result(&camera_result);
        }
        else
        {
            runtime_camera_health_note(camera_configs[camera_index].source, 0);
        }
    }
}
#endif
#endif

#ifndef AMTECH_RUNTIME_LOOP_NO_MAIN
int main(int argc, char **argv)
{
    int force_armed;
    amtech_config_t config;
#ifdef SIMULATE_GPIO
    int i;
#endif

    force_armed = parse_force_armed_arg(argc, argv);
    if (force_armed < 0)
    {
        return 1;
    }

    if (amtech_config_load(runtime_config_path(), &config) != 0)
    {
        return 1;
    }

    alarm_logic_init(AMTECH_ALARM_GPIO_PIN);
    alarm_logic_set_shop_id(config.shop_id);
    if (force_armed)
    {
        /*
         * Testing-only override for bench/bring-up validation. Do not use this
         * in production because it deliberately bypasses the real arm schedule.
         */
        print_force_armed_warning();
        runtime_set_armed(1, &config);
    }
    else
    {
        runtime_set_armed(0, &config);
    }
#ifdef SIMULATE_GPIO
    sensor_input_init(AMTECH_SHUTTER_NC_GPIO_PIN);
    sensor_input_init(AMTECH_SHUTTER_NO_GPIO_PIN);
    if (config.shutter_count >= 2)
    {
        sensor_input_init(AMTECH_SHUTTER2_NC_GPIO_PIN);
        sensor_input_init(AMTECH_SHUTTER2_NO_GPIO_PIN);
    }
    if (config.panic_enabled)
    {
        sensor_input_init(AMTECH_PANIC_GPIO_PIN);
    }
    if (config.smoke_enabled)
    {
        sensor_input_init(AMTECH_SMOKE_GPIO_PIN);
    }
#endif
    runtime_apply_config_schedule(&config);
    if (modem_sms_receive_init() != 0)
    {
        printf("Runtime: warning: SMS remote control initialization failed; continuing without SMS control\n");
    }

#ifdef SIMULATE_GPIO
    for (i = 0; i < AMTECH_RUNTIME_TEST_ITERATIONS; i++)
    {
        runtime_iteration(i, force_armed, &config);
    }

    return 0;
#else
    return run_interrupt_loop(force_armed, &config);
#endif
}
#endif
