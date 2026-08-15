#include "alert_dispatch.h"

#include "config.h"
#include "modem_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *alert_config_path(void)
{
    const char *path = getenv("AMTECH_CONFIG_PATH");

    if (path != NULL && path[0] != '\0')
    {
        return path;
    }

    return AMTECH_DEFAULT_CONFIG_PATH;
}

const char *alert_dispatch_message_for_event(const char *event_type)
{
    if (event_type == NULL)
    {
        return "AMTECH ALERT: Security alarm triggered at your shop.";
    }

    if (strcmp(event_type, "panic") == 0)
    {
        return "AMTECH ALERT: Panic button pressed at your shop. Immediate attention needed.";
    }

    if (strcmp(event_type, "shutter-1") == 0)
    {
        return "AMTECH ALERT: Shutter 1 intrusion detected at your shop.";
    }

    if (strcmp(event_type, "shutter-2") == 0)
    {
        return "AMTECH ALERT: Shutter 2 intrusion detected at your shop.";
    }

    if (strcmp(event_type, "shutter") == 0)
    {
        return "AMTECH ALERT: Shutter intrusion detected at your shop.";
    }

    if (strcmp(event_type, "intrusion") == 0)
    {
        return "AMTECH ALERT: Person detected inside your shop while armed.";
    }

    if (strcmp(event_type, "smoke") == 0)
    {
        return "AMTECH ALERT: Smoke detected at your shop. Possible fire emergency.";
    }

    return "AMTECH ALERT: Security alarm triggered at your shop.";
}

int alert_dispatch_send(const char *event_type)
{
    amtech_config_t config;
    const char *message;
    int result = 0;

    if (amtech_config_load(alert_config_path(), &config) != 0)
    {
        printf("Alert dispatch: failed to load config for alert contact\n");
        return -1;
    }

    message = alert_dispatch_message_for_event(event_type);
    printf("Alert dispatch: sending %s alert to %s\n",
           event_type != NULL ? event_type : "unknown",
           config.alert_contact_number);

    if (modem_send_sms(config.alert_contact_number, message) != 0)
    {
        printf("Alert dispatch: SMS failed for %s\n", event_type != NULL ? event_type : "unknown");
        result = -1;
    }

    if (modem_make_voice_call(config.alert_contact_number) != 0)
    {
        printf("Alert dispatch: voice call failed for %s\n", event_type != NULL ? event_type : "unknown");
        result = -1;
    }

    return result;
}

void alert_dispatch_tick(unsigned int elapsed_ms)
{
    modem_hal_tick(elapsed_ms);
}
