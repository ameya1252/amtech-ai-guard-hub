#include "alert_dispatch.h"

#include "config.h"
#include "modem_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALERT_CALL_ATTEMPTS_PER_CONTACT 2
#define ALERT_CALL_ATTEMPT_TIMEOUT_MS 30000U

static alert_call_escalation_state_t escalation_state = ALERT_CALL_ESCALATION_IDLE;
static char escalation_contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX];
static int escalation_contact_index = 0;
static int escalation_attempt = 0;
static unsigned int escalation_attempt_elapsed_ms = 0;

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

    if (strcmp(event_type, "intrusion-front") == 0)
    {
        return "AMTECH ALERT: Person detected on the front camera while armed.";
    }

    if (strcmp(event_type, "intrusion-parking") == 0)
    {
        return "AMTECH ALERT: Person detected on the parking camera while armed.";
    }

    if (strcmp(event_type, "smoke") == 0)
    {
        return "AMTECH ALERT: Smoke detected at your shop. Possible fire emergency.";
    }

    return "AMTECH ALERT: Security alarm triggered at your shop.";
}

static int contact_is_configured(const char *number)
{
    return number != NULL && number[0] != '\0';
}

static void clear_escalation(void)
{
    int i;

    escalation_state = ALERT_CALL_ESCALATION_IDLE;
    escalation_contact_index = 0;
    escalation_attempt = 0;
    escalation_attempt_elapsed_ms = 0;
    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        escalation_contacts[i][0] = '\0';
    }
}

static int start_current_call_attempt(void)
{
    const char *number;

    while (escalation_contact_index < AMTECH_ALERT_CONTACT_COUNT)
    {
        while (escalation_contact_index < AMTECH_ALERT_CONTACT_COUNT &&
               !contact_is_configured(escalation_contacts[escalation_contact_index]))
        {
            escalation_contact_index++;
            escalation_attempt = 0;
        }

        if (escalation_contact_index >= AMTECH_ALERT_CONTACT_COUNT)
        {
            break;
        }

        number = escalation_contacts[escalation_contact_index];
        printf("Alert dispatch: calling contact %d attempt %d: %s\n",
               escalation_contact_index + 1,
               escalation_attempt + 1,
               number);

        if (modem_make_voice_call(number) == 0)
        {
            escalation_state = ALERT_CALL_ESCALATION_WAITING;
            escalation_attempt_elapsed_ms = 0;
            return 0;
        }

        printf("Alert dispatch: failed to start voice call to contact %d attempt %d, treating as unanswered\n",
               escalation_contact_index + 1,
               escalation_attempt + 1);

        escalation_attempt++;
        if (escalation_attempt >= ALERT_CALL_ATTEMPTS_PER_CONTACT)
        {
            escalation_contact_index++;
            escalation_attempt = 0;
        }
    }

    escalation_state = ALERT_CALL_ESCALATION_DONE;
    escalation_attempt_elapsed_ms = 0;
    printf("Alert dispatch: call escalation complete, no more contacts\n");
    return 0;
}

static int advance_to_next_call_attempt(void)
{
    if (modem_voice_call_is_active())
    {
        modem_hangup_voice_call();
    }

    escalation_attempt++;
    if (escalation_attempt >= ALERT_CALL_ATTEMPTS_PER_CONTACT)
    {
        escalation_contact_index++;
        escalation_attempt = 0;
    }

    return start_current_call_attempt();
}

int alert_dispatch_send(const char *event_type)
{
    amtech_config_t config;
    const char *message;
    int result = 0;
    int i;

    if (amtech_config_load(alert_config_path(), &config) != 0)
    {
        printf("Alert dispatch: failed to load config for alert contact\n");
        return -1;
    }

    message = alert_dispatch_message_for_event(event_type);
    printf("Alert dispatch: sending %s alert SMS to all configured contacts\n",
           event_type != NULL ? event_type : "unknown");

    modem_hangup_voice_call();
    clear_escalation();
    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        snprintf(escalation_contacts[i],
                 sizeof(escalation_contacts[i]),
                 "%s",
                 config.alert_contacts[i]);

        if (!contact_is_configured(config.alert_contacts[i]))
        {
            continue;
        }

        if (modem_send_sms(config.alert_contacts[i], message) != 0)
        {
            printf("Alert dispatch: SMS failed for contact %d on %s\n",
                   i + 1,
                   event_type != NULL ? event_type : "unknown");
            result = -1;
        }
    }

    if (start_current_call_attempt() != 0)
    {
        printf("Alert dispatch: voice call failed for %s\n", event_type != NULL ? event_type : "unknown");
        result = -1;
    }

    return result;
}

void alert_dispatch_tick(unsigned int elapsed_ms)
{
    modem_call_status_t call_status;

    modem_hal_tick(elapsed_ms);

    if (elapsed_ms == 0 ||
        escalation_state == ALERT_CALL_ESCALATION_IDLE ||
        escalation_state == ALERT_CALL_ESCALATION_DONE ||
        escalation_state == ALERT_CALL_ESCALATION_FAILED)
    {
        return;
    }

    if (elapsed_ms > ALERT_CALL_ATTEMPT_TIMEOUT_MS - escalation_attempt_elapsed_ms)
    {
        escalation_attempt_elapsed_ms = ALERT_CALL_ATTEMPT_TIMEOUT_MS;
    }
    else
    {
        escalation_attempt_elapsed_ms += elapsed_ms;
    }

    call_status = modem_get_voice_call_status();

    if (call_status == MODEM_CALL_STATUS_ACTIVE)
    {
        if (escalation_attempt_elapsed_ms < ALERT_CALL_ATTEMPT_TIMEOUT_MS)
        {
            return;
        }
    }

    if (call_status == MODEM_CALL_STATUS_ENDED ||
        call_status == MODEM_CALL_STATUS_FAILED ||
        escalation_attempt_elapsed_ms >= ALERT_CALL_ATTEMPT_TIMEOUT_MS)
    {
        printf("Alert dispatch: contact %d attempt %d unanswered\n",
               escalation_contact_index + 1,
               escalation_attempt + 1);
        advance_to_next_call_attempt();
    }
}

alert_call_escalation_state_t alert_dispatch_get_call_escalation_state(void)
{
    return escalation_state;
}

int alert_dispatch_get_current_contact_index(void)
{
    return escalation_contact_index;
}

int alert_dispatch_get_current_attempt(void)
{
    return escalation_attempt;
}

void alert_dispatch_reset(void)
{
    clear_escalation();
    modem_hangup_voice_call();
}
