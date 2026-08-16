#include "alarm_logic.h"
#include "camera_detection.h"
#include "config.h"
#include "gpio_control.h"
#include "modem_hal.h"
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
#ifndef SIMULATE_CAMERA
#include <pthread.h>
#endif
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
#define AMTECH_SHOP_ID "amtech-demo-shop"
#define AMTECH_RUNTIME_TEST_ITERATIONS 10
#define AMTECH_DEBOUNCE_CONFIRM_MS 200
#define AMTECH_GPIO_POLL_TIMEOUT_MS 100
#define AMTECH_SMS_COMMAND_POLL_MS 5000
#define AMTECH_CAMERA_RETRY_DELAY_SECONDS 1
#define AMTECH_CAMERA_QUEUE_CAPACITY 8

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

#ifndef SIMULATE_GPIO
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

void runtime_process_camera_detection_result(const camera_detection_result_t *result)
{
    if (result == NULL)
    {
        return;
    }

    printf("Runtime: camera source=%s event=%s frame person=%d confidence=%.3f\n",
           result->source,
           result->event_type,
           result->person_detected,
           result->max_confidence);

    if (result->person_detected)
    {
        alarm_logic_handle_detection_source(0,
                                            "person",
                                            result->max_confidence,
                                            result->event_type);
    }

    alarm_logic_end_frame_source(result->event_type);
}

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

static int sms_sender_is_authorized(const amtech_config_t *config, const char *sender)
{
    int i;

    if (config == NULL || sender == NULL)
    {
        return 0;
    }

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        if (strcmp(sender, config->alert_contacts[i]) == 0)
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
        alarm_logic_set_armed(1);
        modem_send_sms(sms->sender, "System ARMED");
        printf("Runtime: accepted SMS ARM command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "DISARM") == 0)
    {
        alarm_logic_set_armed(0);
        modem_send_sms(sms->sender, "System DISARMED");
        printf("Runtime: accepted SMS DISARM command from %s\n", sms->sender);
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

    if (modem_voice_call_is_active())
    {
        return 0;
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

static void update_schedule_from_realtime(void)
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

    alarm_logic_set_armed(schedule_should_be_armed(now_local.tm_hour, now_local.tm_min));
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

static int sms_sender_is_authorized(const amtech_config_t *config, const char *sender)
{
    int i;

    if (config == NULL || sender == NULL)
    {
        return 0;
    }

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        if (strcmp(sender, config->alert_contacts[i]) == 0)
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
        alarm_logic_set_armed(1);
        modem_send_sms(sms->sender, "System ARMED");
        printf("Runtime: accepted SMS ARM command from %s\n", sms->sender);
        return 1;
    }

    if (strcmp(command, "DISARM") == 0)
    {
        alarm_logic_set_armed(0);
        modem_send_sms(sms->sender, "System DISARMED");
        printf("Runtime: accepted SMS DISARM command from %s\n", sms->sender);
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

    if (modem_voice_call_is_active())
    {
        return 0;
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

        if (camera_detection_run_once_for_source(context->source,
                                                 context->event_type,
                                                 context->rtsp_url,
                                                 context->inference_mutex,
                                                 &result) == 0)
        {
            camera_queue_publish(context->queue, &result);
        }
        else
        {
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

    for (i = 0; i < camera_count; i++)
    {
        camera_detection_result_t result;

        if (camera_detection_run_once_for_source(cameras[i].source,
                                                 cameras[i].event_type,
                                                 cameras[i].rtsp_url,
                                                 NULL,
                                                 &result) == 0)
        {
            runtime_process_camera_detection_result(&result);
        }
    }
}
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

static int run_interrupt_loop(int force_armed, const amtech_config_t *config)
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

#ifndef SIMULATE_CAMERA
    camera_queue_init(&camera_queue);
    pthread_mutex_init(&inference_mutex, NULL);
    camera_count = runtime_build_camera_configs(config, camera_configs, AMTECH_RUNTIME_MAX_CAMERAS);
    if (camera_count < 0)
    {
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
            alarm_logic_tick((unsigned int)(now_ms - last_alarm_tick_ms));
        }
        last_alarm_tick_ms = now_ms;

        if (!force_armed)
        {
            tick_schedule_elapsed_seconds(&last_schedule_tick_ms);
            update_schedule_from_realtime();
        }

        if (last_sms_poll_ms == 0 || now_ms - last_sms_poll_ms >= AMTECH_SMS_COMMAND_POLL_MS)
        {
            runtime_poll_sms_remote_control(config);
            last_sms_poll_ms = now_ms;
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
        alarm_logic_set_armed(should_be_armed);
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
                runtime_process_camera_detection_result(&camera_result);
                camera_processed = 1;
            }
        }
    }
#endif

    if (!camera_processed)
    {
        alarm_logic_end_frame();
    }
    alarm_logic_tick(1000);
    runtime_poll_sms_remote_control(config);
}
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
    alarm_logic_set_shop_id(AMTECH_SHOP_ID);
    if (force_armed)
    {
        /*
         * Testing-only override for bench/bring-up validation. Do not use this
         * in production because it deliberately bypasses the real arm schedule.
         */
        print_force_armed_warning();
        alarm_logic_set_armed(1);
    }
    else
    {
        alarm_logic_set_armed(0);
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
    schedule_set_armed_window(23, 0, 6, 0);
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
