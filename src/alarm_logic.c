#include "alarm_logic.h"

#include "gpio_control.h"
#include "notify_client.h"

#include <stdio.h>
#include <string.h>

#define PERSON_CLASS_ID 0
#define PERSON_CONFIDENCE_THRESHOLD 0.6f
#define REQUIRED_CONSECUTIVE_FRAMES 3
#define SHOP_ID_MAX_SIZE 64

static int alarm_gpio_pin = -1;
static int armed = 0;
static int consecutive_person_frames = 0;
static int person_seen_this_frame = 0;
static int alarm_triggered = 0;
static char alarm_shop_id[SHOP_ID_MAX_SIZE] = "amtech-demo-shop";
static const char *pending_alarm_event_type = "intrusion";

void alarm_logic_init(int gpio_pin)
{
    alarm_gpio_pin = gpio_pin;
    armed = 0;
    consecutive_person_frames = 0;
    person_seen_this_frame = 0;
    alarm_triggered = 0;

    if (gpio_export(alarm_gpio_pin) != 0)
    {
        printf("Alarm: failed to export GPIO %d\n", alarm_gpio_pin);
        return;
    }

    if (gpio_set_output(alarm_gpio_pin) != 0)
    {
        printf("Alarm: failed to set GPIO %d as output\n", alarm_gpio_pin);
        return;
    }

    gpio_write_value(alarm_gpio_pin, 0);
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
    if (alarm_triggered)
    {
        return;
    }

    alarm_triggered = 1;
    printf("Alarm: triggered\n");

    if (alarm_gpio_pin >= 0)
    {
        gpio_write_value(alarm_gpio_pin, 1);
    }

    notify_send_alert(alarm_shop_id, pending_alarm_event_type);
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
