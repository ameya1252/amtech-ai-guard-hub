#include "alarm_logic.h"

#include "alert_dispatch.h"
#include "gpio_control.h"

#include <stdio.h>
#include <string.h>

#define PERSON_CLASS_ID 0
#define PERSON_CONFIDENCE_THRESHOLD 0.25f
#define REQUIRED_CONSECUTIVE_FRAMES 1
#define SHOP_ID_MAX_SIZE 64
#define AMTECH_STROBE_GPIO_PIN 48
#define AMTECH_DETECTION_SOURCE_MAX 4
#define AMTECH_DETECTION_EVENT_TYPE_MAX 32

typedef struct
{
    int used;
    char event_type[AMTECH_DETECTION_EVENT_TYPE_MAX];
    int consecutive_person_frames;
    int person_seen_this_frame;
} detection_source_state_t;

static int alarm_gpio_pin = -1;
static int strobe_gpio_pin = AMTECH_STROBE_GPIO_PIN;
static int siren_active = 0;
static unsigned int siren_elapsed_ms = 0;
static int alert_dispatch_sent_this_incident = 0;
static unsigned int alert_dispatch_elapsed_ms = 0;
static int armed = 0;
static int alarm_triggered = 0;
static char alarm_shop_id[SHOP_ID_MAX_SIZE] = "amtech-demo-shop";
static const char *pending_alarm_event_type = "intrusion";
static detection_source_state_t detection_sources[AMTECH_DETECTION_SOURCE_MAX];

static void reset_detection_sources(void)
{
    memset(detection_sources, 0, sizeof(detection_sources));
}

static detection_source_state_t *get_detection_source(const char *event_type)
{
    int i;
    int first_free = -1;

    if (event_type == NULL || event_type[0] == '\0')
    {
        event_type = "intrusion";
    }

    for (i = 0; i < AMTECH_DETECTION_SOURCE_MAX; i++)
    {
        if (detection_sources[i].used &&
            strcmp(detection_sources[i].event_type, event_type) == 0)
        {
            return &detection_sources[i];
        }

        if (!detection_sources[i].used && first_free < 0)
        {
            first_free = i;
        }
    }

    if (first_free < 0)
    {
        printf("Alarm: no free detection source slot for %s\n", event_type);
        return NULL;
    }

    detection_sources[first_free].used = 1;
    snprintf(detection_sources[first_free].event_type,
             sizeof(detection_sources[first_free].event_type),
             "%s",
             event_type);
    detection_sources[first_free].consecutive_person_frames = 0;
    detection_sources[first_free].person_seen_this_frame = 0;
    return &detection_sources[first_free];
}

static void clear_detection_frame_flags(void)
{
    int i;

    for (i = 0; i < AMTECH_DETECTION_SOURCE_MAX; i++)
    {
        detection_sources[i].person_seen_this_frame = 0;
        detection_sources[i].consecutive_person_frames = 0;
    }
}

static int init_alarm_output_off(int gpio_pin, const char *name)
{
    if (gpio_export(gpio_pin) != 0)
    {
        printf("Alarm: failed to export %s GPIO %d\n", name, gpio_pin);
        return -1;
    }

    if (gpio_set_output_value(gpio_pin, 1) != 0)
    {
        printf("Alarm: failed to set %s GPIO %d as output HIGH\n", name, gpio_pin);
        return -1;
    }

    return 0;
}

static void reactivate_alarm_outputs(void)
{
    if (alarm_gpio_pin >= 0)
    {
        gpio_write_value(alarm_gpio_pin, 0);
        siren_active = 1;
        siren_elapsed_ms = 0;
    }

    if (strobe_gpio_pin >= 0)
    {
        gpio_write_value(strobe_gpio_pin, 0);
    }
}

void alarm_logic_init(int gpio_pin)
{
    alarm_gpio_pin = gpio_pin;
    strobe_gpio_pin = AMTECH_STROBE_GPIO_PIN;
    siren_active = 0;
    siren_elapsed_ms = 0;
    alert_dispatch_sent_this_incident = 0;
    alert_dispatch_elapsed_ms = 0;
    armed = 0;
    alarm_triggered = 0;
    reset_detection_sources();
    alert_dispatch_reset();

    if (init_alarm_output_off(alarm_gpio_pin, "siren") != 0)
    {
        return;
    }

    if (init_alarm_output_off(strobe_gpio_pin, "strobe") != 0)
    {
        return;
    }
}

void alarm_logic_set_shop_id(const char *shop_id)
{
    if (shop_id == NULL || shop_id[0] == '\0')
    {
        printf("Alarm: ignoring empty shop_id\n");
        return;
    }

    snprintf(alarm_shop_id, sizeof(alarm_shop_id), "%s", shop_id);
    printf("Alarm: shop_id set to %s\n", alarm_shop_id);
}

void alarm_logic_set_armed(int next_armed)
{
    armed = next_armed ? 1 : 0;
    clear_detection_frame_flags();

    printf("Alarm: system %s\n", armed ? "ARMED" : "DISARMED");
}

void alarm_logic_toggle_armed(void)
{
    alarm_logic_set_armed(!armed);
}

int alarm_logic_is_armed(void)
{
    return armed;
}

int alarm_logic_is_triggered(void)
{
    return alarm_triggered;
}

void trigger_alarm(void)
{
    int should_send_alert_dispatch;

    should_send_alert_dispatch = !alert_dispatch_sent_this_incident ||
                                 alert_dispatch_elapsed_ms >= AMTECH_ALERT_DISPATCH_COOLDOWN_MS;
    alarm_triggered = 1;
    printf("Alarm: triggered\n");

    reactivate_alarm_outputs();

    if (should_send_alert_dispatch)
    {
        alert_dispatch_send(pending_alarm_event_type);
        alert_dispatch_sent_this_incident = 1;
        alert_dispatch_elapsed_ms = 0;
    }
    else
    {
        printf("Alarm: alert dispatch suppressed by cooldown\n");
    }
}

void alarm_logic_reset(void)
{
    alarm_triggered = 0;
    siren_active = 0;
    siren_elapsed_ms = 0;
    alert_dispatch_sent_this_incident = 0;
    alert_dispatch_elapsed_ms = 0;
    reset_detection_sources();
    pending_alarm_event_type = "intrusion";

    if (alarm_gpio_pin >= 0)
    {
        gpio_write_value(alarm_gpio_pin, 1);
    }

    if (strobe_gpio_pin >= 0)
    {
        gpio_write_value(strobe_gpio_pin, 1);
    }

    alert_dispatch_reset();
    printf("Alarm: reset\n");
}

void alarm_logic_tick(unsigned int elapsed_ms)
{
    if (elapsed_ms == 0)
    {
        return;
    }

    alert_dispatch_tick(elapsed_ms);

    if (alarm_triggered &&
        alert_dispatch_sent_this_incident &&
        alert_dispatch_elapsed_ms < AMTECH_ALERT_DISPATCH_COOLDOWN_MS)
    {
        if (elapsed_ms > AMTECH_ALERT_DISPATCH_COOLDOWN_MS - alert_dispatch_elapsed_ms)
        {
            alert_dispatch_elapsed_ms = AMTECH_ALERT_DISPATCH_COOLDOWN_MS;
        }
        else
        {
            alert_dispatch_elapsed_ms += elapsed_ms;
        }
    }

    if (alarm_triggered && siren_active)
    {
        if (elapsed_ms > AMTECH_SIREN_DURATION_MS - siren_elapsed_ms)
        {
            siren_elapsed_ms = AMTECH_SIREN_DURATION_MS;
        }
        else
        {
            siren_elapsed_ms += elapsed_ms;
        }

        if (siren_elapsed_ms >= AMTECH_SIREN_DURATION_MS)
        {
            if (alarm_gpio_pin >= 0)
            {
                gpio_write_value(alarm_gpio_pin, 1);
            }
            siren_active = 0;
            printf("Alarm: siren auto-stopped after %u ms\n", AMTECH_SIREN_DURATION_MS);
        }
    }
}

void alarm_logic_handle_detection(int class_id, const char *class_name, float confidence)
{
    alarm_logic_handle_detection_source(class_id, class_name, confidence, "intrusion");
}

void alarm_logic_handle_detection_source(int class_id,
                                         const char *class_name,
                                         float confidence,
                                         const char *event_type)
{
    int is_person = 0;
    detection_source_state_t *source;

    if (class_id == PERSON_CLASS_ID)
    {
        is_person = 1;
    }
    else if (class_name != NULL && strcmp(class_name, "person") == 0)
    {
        is_person = 1;
    }

    if (!is_person || confidence <= PERSON_CONFIDENCE_THRESHOLD)
    {
        return;
    }

    printf("Alarm: person detected source=%s confidence=%.3f state=%s\n",
           event_type != NULL && event_type[0] != '\0' ? event_type : "intrusion",
           confidence,
           armed ? "ARMED" : "DISARMED");

    if (!armed)
    {
        return;
    }

    source = get_detection_source(event_type);
    if (source == NULL)
    {
        return;
    }

    source->person_seen_this_frame = 1;
}

void alarm_logic_handle_shutter_sensor(int triggered)
{
    if (!triggered)
    {
        return;
    }

    printf("Alarm: shutter sensor triggered state=%s\n", armed ? "ARMED" : "DISARMED");

    if (!armed)
    {
        return;
    }

    pending_alarm_event_type = "shutter";
    trigger_alarm();
}

void alarm_logic_handle_shutter_dual(shutter_state_t state)
{
    alarm_logic_handle_shutter_dual_named(state, "shutter", "shutter");
}

void alarm_logic_handle_shutter_dual_named(shutter_state_t state,
                                           const char *shutter_name,
                                           const char *event_type)
{
    switch (state)
    {
    case SHUTTER_CLOSED:
        return;
    case SHUTTER_OPEN:
        printf("Alarm: %s open state=%s\n", shutter_name, armed ? "ARMED" : "DISARMED");
        if (!armed)
        {
            return;
        }
        pending_alarm_event_type = event_type;
        trigger_alarm();
        return;
    case SHUTTER_TAMPER:
        printf("Alarm: %s wire tamper detected\n", shutter_name);
        pending_alarm_event_type = event_type;
        trigger_alarm();
        return;
    case SHUTTER_FAULT:
        printf("Alarm: %s hardware fault detected\n", shutter_name);
        return;
    default:
        printf("Alarm: %s unknown state %d\n", shutter_name, state);
        return;
    }
}

void alarm_logic_handle_panic(int triggered)
{
    if (!triggered)
    {
        return;
    }

    printf("Alarm: PANIC button triggered\n");
    pending_alarm_event_type = "panic";
    trigger_alarm();
}

void alarm_logic_handle_smoke(int triggered)
{
    if (!triggered)
    {
        return;
    }

    printf("Alarm: SMOKE detector triggered\n");
    pending_alarm_event_type = "smoke";
    trigger_alarm();
}

void alarm_logic_end_frame(void)
{
    alarm_logic_end_frame_source("intrusion");
}

void alarm_logic_end_frame_source(const char *event_type)
{
    detection_source_state_t *source;

    source = get_detection_source(event_type);
    if (source == NULL)
    {
        return;
    }

    if (!armed)
    {
        source->person_seen_this_frame = 0;
        source->consecutive_person_frames = 0;
        return;
    }

    if (source->person_seen_this_frame)
    {
        source->consecutive_person_frames++;
    }
    else
    {
        source->consecutive_person_frames = 0;
    }

    printf("Alarm: %s consecutive person frames=%d\n",
           source->event_type,
           source->consecutive_person_frames);

    if (source->consecutive_person_frames >= REQUIRED_CONSECUTIVE_FRAMES)
    {
        pending_alarm_event_type = source->event_type;
        trigger_alarm();
    }

    source->person_seen_this_frame = 0;
}
