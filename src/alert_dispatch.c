#include "alert_dispatch.h"

#include "amtech_log.h"
#include "config.h"
#include "modem_hal.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ALERT_CALL_ATTEMPTS_PER_CONTACT 2
#define ALERT_CALL_ATTEMPT_TIMEOUT_MS 30000U
#define ALERT_CALL_ANSWER_CONFIRM_MS 3000U
#define AMTECH_CALL_RETRY_GAP_MS 5000U
#define AMTECH_VOICEMAIL_SUSPECT_MS 2000U
#define ALERT_DISPATCH_EVENT_TYPE_MAX 32
#define ALERT_DISPATCH_MESSAGE_MAX 256

typedef struct
{
    char event_type[ALERT_DISPATCH_EVENT_TYPE_MAX];
    char message[ALERT_DISPATCH_MESSAGE_MAX];
    char contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX];
    unsigned int generation;
} alert_sms_fanout_request_t;

static alert_call_escalation_state_t escalation_state = ALERT_CALL_ESCALATION_IDLE;
static char escalation_contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX];
static int escalation_contact_index = 0;
static int escalation_attempt = 0;
static unsigned int escalation_attempt_elapsed_ms = 0;
static unsigned int escalation_active_elapsed_ms = 0;
static unsigned int escalation_retry_gap_elapsed_ms = 0;
static int escalation_active_seen = 0;
static int escalation_fast_active_suspected = 0;
static pthread_mutex_t escalation_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t dispatch_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dispatch_worker_cond = PTHREAD_COND_INITIALIZER;
static int dispatch_worker_started = 0;
static int dispatch_worker_pending = 0;
static int dispatch_worker_busy = 0;
static unsigned int dispatch_reset_generation = 0;
static unsigned int dispatch_worker_generation = 0;
static char dispatch_worker_event_type[ALERT_DISPATCH_EVENT_TYPE_MAX];

static pthread_mutex_t sms_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sms_worker_cond = PTHREAD_COND_INITIALIZER;
static int sms_worker_active_count = 0;

#ifdef SIMULATE_MODEM
static unsigned int simulated_dispatch_delay_ms = 0;
static void sleep_ms(unsigned int delay_ms)
{
    struct timespec ts;

    ts.tv_sec = delay_ms / 1000U;
    ts.tv_nsec = (long)(delay_ms % 1000U) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    {
    }
}
#endif

static unsigned int current_dispatch_generation(void);

static void mark_sms_worker_active(int active)
{
    pthread_mutex_lock(&sms_worker_mutex);
    if (active)
    {
        sms_worker_active_count++;
    }
    else if (sms_worker_active_count > 0)
    {
        sms_worker_active_count--;
    }
    pthread_cond_broadcast(&sms_worker_cond);
    pthread_mutex_unlock(&sms_worker_mutex);
}

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
        return "AMTECH ALERT: Person detected inside your shop. System temporarily disarmed; will re-arm after siren stops.";
    }

    if (strcmp(event_type, "intrusion-front") == 0)
    {
        return "AMTECH ALERT: Person detected on the front camera. System temporarily disarmed; will re-arm after siren stops.";
    }

    if (strcmp(event_type, "intrusion-parking") == 0)
    {
        return "AMTECH ALERT: Person detected on the parking camera. System temporarily disarmed; will re-arm after siren stops.";
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
    escalation_active_elapsed_ms = 0;
    escalation_retry_gap_elapsed_ms = 0;
    escalation_active_seen = 0;
    escalation_fast_active_suspected = 0;
    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        escalation_contacts[i][0] = '\0';
    }
}

static void reset_current_attempt_timing(void)
{
    escalation_attempt_elapsed_ms = 0;
    escalation_active_elapsed_ms = 0;
    escalation_retry_gap_elapsed_ms = 0;
    escalation_active_seen = 0;
    escalation_fast_active_suspected = 0;
}

static int escalation_state_is_active(alert_call_escalation_state_t state)
{
    return state == ALERT_CALL_ESCALATION_WAITING ||
           state == ALERT_CALL_ESCALATION_CONFIRMING_ANSWER ||
           state == ALERT_CALL_ESCALATION_RETRY_GAP;
}

static void move_to_next_attempt_with_settle_gap(void)
{
    escalation_attempt++;
    if (escalation_attempt >= ALERT_CALL_ATTEMPTS_PER_CONTACT)
    {
        escalation_contact_index++;
        escalation_attempt = 0;
    }

    escalation_state = ALERT_CALL_ESCALATION_RETRY_GAP;
    escalation_retry_gap_elapsed_ms = 0;
    reset_current_attempt_timing();
    amtech_logf("Alert dispatch",
                "waiting %u ms and settling modem before next call attempt",
                AMTECH_CALL_RETRY_GAP_MS);
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
        amtech_logf("Alert dispatch",
                    "calling contact %d attempt %d: %s",
                    escalation_contact_index + 1,
                    escalation_attempt + 1,
                    number);

        if (modem_make_voice_call(number) == 0)
        {
            escalation_state = ALERT_CALL_ESCALATION_WAITING;
            reset_current_attempt_timing();
            return 0;
        }

        amtech_logf("Alert dispatch",
                    "failed to start voice call to contact %d attempt %d, treating as unanswered",
                    escalation_contact_index + 1,
                    escalation_attempt + 1);

        move_to_next_attempt_with_settle_gap();
        return 0;
    }

    escalation_state = ALERT_CALL_ESCALATION_DONE;
    reset_current_attempt_timing();
    amtech_logf("Alert dispatch", "call escalation complete, no more contacts");
    return 0;
}

static void *sms_fanout_worker_main(void *arg)
{
    alert_sms_fanout_request_t *request = (alert_sms_fanout_request_t *)arg;
    int i;

    if (request == NULL)
    {
        return NULL;
    }

    amtech_logf("Alert dispatch",
                "sending %s alert SMS to all configured contacts",
                request->event_type);

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        if (!contact_is_configured(request->contacts[i]))
        {
            continue;
        }

        if (current_dispatch_generation() != request->generation)
        {
            amtech_logf("Alert dispatch",
                        "SMS fan-out for %s canceled because incident reset",
                        request->event_type);
            break;
        }

        if (modem_send_sms(request->contacts[i], request->message) != 0)
        {
            amtech_logf("Alert dispatch",
                        "SMS failed for contact %d on %s",
                        i + 1,
                        request->event_type);
        }
    }

    free(request);
    mark_sms_worker_active(0);
    return NULL;
}

static int start_sms_fanout(const char *event_type,
                            const char *message,
                            char contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX],
                            unsigned int generation)
{
    alert_sms_fanout_request_t *request;
    pthread_t thread;
    int rc;
    int i;

    request = (alert_sms_fanout_request_t *)calloc(1, sizeof(*request));
    if (request == NULL)
    {
        amtech_logf("Alert dispatch", "failed to allocate SMS fan-out request");
        return -1;
    }

    snprintf(request->event_type,
             sizeof(request->event_type),
             "%s",
             event_type != NULL && event_type[0] != '\0' ? event_type : "unknown");
    snprintf(request->message, sizeof(request->message), "%s", message);
    request->generation = generation;
    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        snprintf(request->contacts[i],
                 sizeof(request->contacts[i]),
                 "%s",
                 contacts[i]);
    }

    mark_sms_worker_active(1);
    rc = pthread_create(&thread, NULL, sms_fanout_worker_main, request);
    if (rc != 0)
    {
        mark_sms_worker_active(0);
        free(request);
        amtech_logf("Alert dispatch", "failed to start SMS fan-out worker: %s", strerror(rc));
        return -1;
    }

    pthread_detach(thread);
    return 0;
}

static int advance_to_next_call_attempt(void)
{
    if (modem_voice_call_is_active())
    {
        modem_hangup_voice_call();
    }

    move_to_next_attempt_with_settle_gap();
    return 0;
}

static int alert_dispatch_send_with_generation(const char *event_type, unsigned int start_generation)
{
    amtech_config_t config;
    const char *message;
    char local_contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX];
    int result = 0;
    int i;

    if (amtech_config_load(alert_config_path(), &config) != 0)
    {
        amtech_logf("Alert dispatch", "failed to load config for alert contact");
        return -1;
    }

    if (current_dispatch_generation() != start_generation)
    {
        amtech_logf("Alert dispatch",
                    "dispatch for %s canceled before call/SMS because incident reset",
                    event_type != NULL ? event_type : "unknown");
        return -1;
    }

    message = alert_dispatch_message_for_event(event_type);

    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        snprintf(local_contacts[i],
                 sizeof(local_contacts[i]),
                 "%s",
                 config.alert_contacts[i]);
    }

    pthread_mutex_lock(&escalation_mutex);
    if (escalation_state_is_active(escalation_state))
    {
        amtech_logf("Alert dispatch",
                    "call escalation already active at contact %d attempt %d; suppressing new dispatch for %s",
                    escalation_contact_index + 1,
                    escalation_attempt + 1,
                    event_type != NULL ? event_type : "unknown");
        pthread_mutex_unlock(&escalation_mutex);
        return -1;
    }

    modem_hangup_voice_call();
    clear_escalation();
    for (i = 0; i < AMTECH_ALERT_CONTACT_COUNT; i++)
    {
        snprintf(escalation_contacts[i],
                 sizeof(escalation_contacts[i]),
                 "%s",
                 local_contacts[i]);
    }

    if (start_current_call_attempt() != 0)
    {
        amtech_logf("Alert dispatch",
                    "voice call failed for %s",
                    event_type != NULL ? event_type : "unknown");
        result = -1;
    }
    pthread_mutex_unlock(&escalation_mutex);

    /*
     * Voice escalation is started before SMS fan-out so a slow/failed SMS path
     * cannot delay the first emergency call. SMS still goes to every configured
     * contact once, but it runs on its own worker.
     */
    if (start_sms_fanout(event_type, message, local_contacts, start_generation) != 0)
    {
        result = -1;
    }

    return result;
}

int alert_dispatch_send(const char *event_type)
{
    return alert_dispatch_send_with_generation(event_type, current_dispatch_generation());
}

static void *alert_dispatch_worker_main(void *arg)
{
    (void)arg;

    for (;;)
    {
        char event_type[ALERT_DISPATCH_EVENT_TYPE_MAX];
        unsigned int request_generation;

        pthread_mutex_lock(&dispatch_worker_mutex);
        while (!dispatch_worker_pending)
        {
            pthread_cond_wait(&dispatch_worker_cond, &dispatch_worker_mutex);
        }

        snprintf(event_type, sizeof(event_type), "%s", dispatch_worker_event_type);
        request_generation = dispatch_worker_generation;
        dispatch_worker_pending = 0;
        dispatch_worker_busy = 1;
        pthread_mutex_unlock(&dispatch_worker_mutex);

#ifdef SIMULATE_MODEM
        if (simulated_dispatch_delay_ms > 0)
        {
            amtech_logf("Alert dispatch",
                        "test delay before dispatch %u ms",
                        simulated_dispatch_delay_ms);
            sleep_ms(simulated_dispatch_delay_ms);
        }
#endif

        alert_dispatch_send_with_generation(event_type, request_generation);

        pthread_mutex_lock(&dispatch_worker_mutex);
        dispatch_worker_busy = 0;
        pthread_cond_broadcast(&dispatch_worker_cond);
        pthread_mutex_unlock(&dispatch_worker_mutex);
    }

    return NULL;
}

static int ensure_dispatch_worker_started(void)
{
    pthread_t thread;
    int rc;

    pthread_mutex_lock(&dispatch_worker_mutex);
    if (dispatch_worker_started)
    {
        pthread_mutex_unlock(&dispatch_worker_mutex);
        return 0;
    }
    pthread_mutex_unlock(&dispatch_worker_mutex);

    rc = pthread_create(&thread, NULL, alert_dispatch_worker_main, NULL);
    if (rc != 0)
    {
        amtech_logf("Alert dispatch", "failed to start async worker: %s", strerror(rc));
        return -1;
    }
    pthread_detach(thread);

    pthread_mutex_lock(&dispatch_worker_mutex);
    dispatch_worker_started = 1;
    pthread_mutex_unlock(&dispatch_worker_mutex);
    return 0;
}

static unsigned int current_dispatch_generation(void)
{
    unsigned int generation;

    pthread_mutex_lock(&dispatch_worker_mutex);
    generation = dispatch_reset_generation;
    pthread_mutex_unlock(&dispatch_worker_mutex);
    return generation;
}

int alert_dispatch_request_async(const char *event_type)
{
    if (event_type == NULL || event_type[0] == '\0')
    {
        event_type = "intrusion";
    }

    if (ensure_dispatch_worker_started() != 0)
    {
        return -1;
    }

    pthread_mutex_lock(&escalation_mutex);
    if (escalation_state_is_active(escalation_state))
    {
        amtech_logf("Alert dispatch",
                    "call escalation already active at contact %d attempt %d; suppressing async request for %s",
                    escalation_contact_index + 1,
                    escalation_attempt + 1,
                    event_type);
        pthread_mutex_unlock(&escalation_mutex);
        return -1;
    }
    pthread_mutex_unlock(&escalation_mutex);

    pthread_mutex_lock(&dispatch_worker_mutex);
    if (dispatch_worker_pending || dispatch_worker_busy)
    {
        amtech_logf("Alert dispatch",
                    "async dispatch already busy; suppressing new request for %s",
                    event_type);
        pthread_mutex_unlock(&dispatch_worker_mutex);
        return -1;
    }

    snprintf(dispatch_worker_event_type,
             sizeof(dispatch_worker_event_type),
             "%s",
             event_type);
    dispatch_worker_generation = dispatch_reset_generation;
    dispatch_worker_pending = 1;
    pthread_cond_signal(&dispatch_worker_cond);
    pthread_mutex_unlock(&dispatch_worker_mutex);
    return 0;
}

void alert_dispatch_tick(unsigned int elapsed_ms)
{
    modem_call_status_t call_status;

    modem_hal_tick(elapsed_ms);

    pthread_mutex_lock(&escalation_mutex);

    if (elapsed_ms == 0 ||
        escalation_state == ALERT_CALL_ESCALATION_IDLE ||
        escalation_state == ALERT_CALL_ESCALATION_ANSWERED ||
        escalation_state == ALERT_CALL_ESCALATION_DONE ||
        escalation_state == ALERT_CALL_ESCALATION_FAILED)
    {
        pthread_mutex_unlock(&escalation_mutex);
        return;
    }

    if (escalation_state == ALERT_CALL_ESCALATION_RETRY_GAP)
    {
        if (elapsed_ms > AMTECH_CALL_RETRY_GAP_MS - escalation_retry_gap_elapsed_ms)
        {
            escalation_retry_gap_elapsed_ms = AMTECH_CALL_RETRY_GAP_MS;
        }
        else
        {
            escalation_retry_gap_elapsed_ms += elapsed_ms;
        }

        if (escalation_retry_gap_elapsed_ms < AMTECH_CALL_RETRY_GAP_MS)
        {
            pthread_mutex_unlock(&escalation_mutex);
            return;
        }

        if (modem_prepare_for_next_voice_call() != 0)
        {
            escalation_retry_gap_elapsed_ms = 0;
            amtech_logf("Alert dispatch",
                        "modem not settled for next call yet; retrying settle in %u ms",
                        AMTECH_CALL_RETRY_GAP_MS);
            pthread_mutex_unlock(&escalation_mutex);
            return;
        }

        start_current_call_attempt();
        pthread_mutex_unlock(&escalation_mutex);
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
        if (!escalation_active_seen)
        {
            escalation_active_seen = 1;
            escalation_active_elapsed_ms = 0;

            /*
             * AT+CLCC reports the call as connected, but it cannot prove whether
             * a human answered or voicemail/IVR picked up. A very fast active
             * state is treated as suspected voicemail and must not stop contact
             * escalation. This timing heuristic can be wrong in both directions,
             * but it avoids letting voicemail block lower-priority contacts.
             */
            if (escalation_attempt_elapsed_ms < AMTECH_VOICEMAIL_SUSPECT_MS)
            {
                escalation_fast_active_suspected = 1;
                amtech_logf("Alert dispatch",
                            "contact %d attempt %d connected after %u ms, suspected voicemail/IVR",
                            escalation_contact_index + 1,
                            escalation_attempt + 1,
                            escalation_attempt_elapsed_ms);
            }
            else
            {
                escalation_state = ALERT_CALL_ESCALATION_CONFIRMING_ANSWER;
                amtech_logf("Alert dispatch",
                            "contact %d attempt %d connected after %u ms, confirming likely human answer",
                            escalation_contact_index + 1,
                            escalation_attempt + 1,
                            escalation_attempt_elapsed_ms);
            }
        }
        else if (!escalation_fast_active_suspected)
        {
            if (elapsed_ms > ALERT_CALL_ANSWER_CONFIRM_MS - escalation_active_elapsed_ms)
            {
                escalation_active_elapsed_ms = ALERT_CALL_ANSWER_CONFIRM_MS;
            }
            else
            {
                escalation_active_elapsed_ms += elapsed_ms;
            }

            if (escalation_active_elapsed_ms >= ALERT_CALL_ANSWER_CONFIRM_MS)
            {
                escalation_state = ALERT_CALL_ESCALATION_ANSWERED;
                amtech_logf("Alert dispatch",
                            "contact %d attempt %d confirmed likely human answer, stopping escalation",
                            escalation_contact_index + 1,
                            escalation_attempt + 1);
                pthread_mutex_unlock(&escalation_mutex);
                return;
            }
        }

        if (escalation_attempt_elapsed_ms < ALERT_CALL_ATTEMPT_TIMEOUT_MS)
        {
            pthread_mutex_unlock(&escalation_mutex);
            return;
        }
    }
    else
    {
        escalation_active_elapsed_ms = 0;
        if (escalation_state == ALERT_CALL_ESCALATION_CONFIRMING_ANSWER)
        {
            escalation_state = ALERT_CALL_ESCALATION_WAITING;
        }
    }

    if (call_status == MODEM_CALL_STATUS_ENDED ||
        call_status == MODEM_CALL_STATUS_FAILED ||
        escalation_attempt_elapsed_ms >= ALERT_CALL_ATTEMPT_TIMEOUT_MS)
    {
        amtech_logf("Alert dispatch",
                    "contact %d attempt %d unanswered",
                    escalation_contact_index + 1,
                    escalation_attempt + 1);
        advance_to_next_call_attempt();
    }

    pthread_mutex_unlock(&escalation_mutex);
}

alert_call_escalation_state_t alert_dispatch_get_call_escalation_state(void)
{
    alert_call_escalation_state_t state;

    pthread_mutex_lock(&escalation_mutex);
    state = escalation_state;
    pthread_mutex_unlock(&escalation_mutex);
    return state;
}

int alert_dispatch_get_current_contact_index(void)
{
    int index;

    pthread_mutex_lock(&escalation_mutex);
    index = escalation_contact_index;
    pthread_mutex_unlock(&escalation_mutex);
    return index;
}

int alert_dispatch_get_current_attempt(void)
{
    int attempt;

    pthread_mutex_lock(&escalation_mutex);
    attempt = escalation_attempt;
    pthread_mutex_unlock(&escalation_mutex);
    return attempt;
}

void alert_dispatch_reset(void)
{
    pthread_mutex_lock(&dispatch_worker_mutex);
    dispatch_worker_pending = 0;
    dispatch_worker_event_type[0] = '\0';
    dispatch_reset_generation++;
    pthread_mutex_unlock(&dispatch_worker_mutex);

    pthread_mutex_lock(&escalation_mutex);
    clear_escalation();
    modem_hangup_voice_call();
    pthread_mutex_unlock(&escalation_mutex);
}

#ifdef SIMULATE_MODEM
void alert_dispatch_test_set_send_delay_ms(unsigned int delay_ms)
{
    simulated_dispatch_delay_ms = delay_ms;
}

int alert_dispatch_test_wait_idle(unsigned int timeout_ms)
{
    unsigned int waited_ms = 0;

    for (;;)
    {
        int idle;

        pthread_mutex_lock(&dispatch_worker_mutex);
        idle = !dispatch_worker_pending && !dispatch_worker_busy;
        pthread_mutex_unlock(&dispatch_worker_mutex);

        pthread_mutex_lock(&sms_worker_mutex);
        idle = idle && sms_worker_active_count == 0;
        pthread_mutex_unlock(&sms_worker_mutex);

        if (idle)
        {
            return 0;
        }

        if (waited_ms >= timeout_ms)
        {
            return -1;
        }

        sleep_ms(10);
        waited_ms += 10;
    }
}
#endif
