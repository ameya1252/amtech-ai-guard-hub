#include "alarm_logic.h"

#include "alert_dispatch.h"
#include "gpio_control.h"
#include "notify_client.h"

#include <stdio.h>
#include <string.h>

#define PERSON_CLASS_ID 0
#define PERSON_CONFIDENCE_THRESHOLD 0.6f
#define REQUIRED_CONSECUTIVE_FRAMES 3
#define SHOP_ID_MAX_SIZE 64
#define AMTECH_STROBE_GPIO_PIN 48

static int alarm_gpio_pin = -1;
static int strobe_gpio_pin = AMTECH_STROBE_GPIO_PIN;
static int siren_active = 0;
static unsigned int siren_elapsed_ms = 0;
static int notification_sent_this_incident = 0;
static unsigned int notification_elapsed_ms = 0;
static int armed = 0;
static int consecutive_person_frames = 0;
static int person_seen_this_frame = 0;
static int alarm_triggered = 0;
static char alarm_shop_id[SHOP_ID_MAX_SIZE] = "amtech-demo-shop";
static const char *pending_alarm_event_type = "intrusion";

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
    notification_sent_this_incident = 0;
    notification_elapsed_ms = 0;
    armed = 0;
    consecutive_person_frames = 0;
    person_seen_this_frame = 0;
    alarm_triggered = 0;
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
    consecutive_person_frames = 0;
    person_seen_this_frame = 0;

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
    int should_send_notification;

    should_send_notification = !notification_sent_this_incident ||
                               notification_elapsed_ms >= AMTECH_NOTIFICATION_COOLDOWN_MS;
    alarm_triggered = 1;
    printf("Alarm: triggered\n");

    reactivate_alarm_outputs();

    if (should_send_notification)
    {
        notify_send_alert(alarm_shop_id, pending_alarm_event_type);
        alert_dispatch_send(pending_alarm_event_type);
        notification_sent_this_incident = 1;
        notification_elapsed_ms = 0;
    }
    else
    {
        printf("Alarm: notification suppressed by cooldown\n");
    }
}

void alarm_logic_reset(void)
{
    alarm_triggered = 0;
    siren_active = 0;
    siren_elapsed_ms = 0;
    notification_sent_this_incident = 0;
    notification_elapsed_ms = 0;
    consecutive_person_frames = 0;
    person_seen_this_frame = 0;
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
        notification_sent_this_incident &&
        notification_elapsed_ms < AMTECH_NOTIFICATION_COOLDOWN_MS)
    {
        if (elapsed_ms > AMTECH_NOTIFICATION_COOLDOWN_MS - notification_elapsed_ms)
        {
            notification_elapsed_ms = AMTECH_NOTIFICATION_COOLDOWN_MS;
        }
        else
        {
            notification_elapsed_ms += elapsed_ms;
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
    int is_person = 0;

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

    printf("Alarm: person detected confidence=%.3f state=%s\n",
           confidence, armed ? "ARMED" : "DISARMED");

    if (!armed)
    {
        return;
    }

    person_seen_this_frame = 1;
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
    if (!armed)
    {
        person_seen_this_frame = 0;
        consecutive_person_frames = 0;
        return;
    }

    if (person_seen_this_frame)
    {
        consecutive_person_frames++;
    }
    else
    {
        consecutive_person_frames = 0;
    }

    printf("Alarm: consecutive person frames=%d\n", consecutive_person_frames);

    if (consecutive_person_frames >= REQUIRED_CONSECUTIVE_FRAMES)
    {
        pending_alarm_event_type = "intrusion";
        trigger_alarm();
    }

    person_seen_this_frame = 0;
}
