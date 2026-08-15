#include "alert_dispatch.h"
#include "config.h"
#include "modem_hal.h"

#include <stdio.h>
#include <stdlib.h>
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

static void check_string(const char *label, const char *actual, const char *expected)
{
    int matches = strcmp(actual, expected) == 0;
    const char *result = matches ? "PASS" : "FAIL";

    printf("%s: got %s, expected %s: %s\n", label, actual, expected, result);
    if (!matches)
    {
        failures++;
    }
}

static void check_message(const char *event_type, const char *expected)
{
    char label[80];

    snprintf(label, sizeof(label), "%s message", event_type);
    check_string(label, alert_dispatch_message_for_event(event_type), expected);
}

static void write_test_config(const char *config_path)
{
    FILE *fp = fopen(config_path, "w");

    if (fp == NULL)
    {
        printf("FAIL: could not write alert dispatch config\n");
        failures++;
        return;
    }

    fprintf(fp,
            "ALERT_CONTACT_1=+911111111111\n"
            "ALERT_CONTACT_2=+912222222222\n"
            "ALERT_CONTACT_3=+913333333333\n");
    fclose(fp);
}

static void tick_ms(unsigned int elapsed_ms)
{
    alert_dispatch_tick(elapsed_ms);
}

static void check_sms_fanout(void)
{
#ifdef SIMULATE_MODEM
    check_int("SMS fan-out count", modem_get_simulated_sms_count(), 3);
    check_string("SMS contact 1", modem_get_simulated_sms_number_at(0), "+911111111111");
    check_string("SMS contact 2", modem_get_simulated_sms_number_at(1), "+912222222222");
    check_string("SMS contact 3", modem_get_simulated_sms_number_at(2), "+913333333333");
#endif
}

static void run_contact1_answered_first_attempt(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE};

    printf("\nScenario 1: contact 1 answered on first attempt\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 4);

    check_int("scenario 1 dispatch starts", alert_dispatch_send("panic"), 0);
    check_sms_fanout();
    check_int("scenario 1 first call count", modem_get_simulated_call_count(), 1);
    check_string("scenario 1 first call number", modem_get_simulated_call_number_at(0), "+911111111111");

    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);

    check_int("scenario 1 answered state",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_ANSWERED);
    check_int("scenario 1 no calls to contacts 2 or 3",
              modem_get_simulated_call_count(),
              1);
#endif
}

static void run_contact1_twice_then_contact2_answered(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE};

    printf("\nScenario 2: contact 1 unanswered twice, contact 2 answered\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 8);

    check_int("scenario 2 dispatch starts", alert_dispatch_send("shutter-1"), 0);
    check_sms_fanout();

    tick_ms(1000);
    tick_ms(1000);
    check_int("scenario 2 moved to contact 1 attempt 2 call count",
              modem_get_simulated_call_count(),
              2);
    check_string("scenario 2 second call still contact 1",
                 modem_get_simulated_call_number_at(1),
                 "+911111111111");

    tick_ms(1000);
    tick_ms(1000);
    check_int("scenario 2 moved to contact 2 call count",
              modem_get_simulated_call_count(),
              3);
    check_string("scenario 2 third call contact 2",
                 modem_get_simulated_call_number_at(2),
                 "+912222222222");

    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);
    check_int("scenario 2 contact 2 answered",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_ANSWERED);
    check_int("scenario 2 no contact 3 call",
              modem_get_simulated_call_count(),
              3);
#endif
}

static void run_brief_active_false_positive(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ENDED};

    printf("\nScenario 3: brief active state is treated as unanswered\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 3);

    check_int("scenario 3 dispatch starts", alert_dispatch_send("intrusion"), 0);
    check_sms_fanout();

    tick_ms(1000);
    tick_ms(1000);
    check_int("scenario 3 enters answer confirmation",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_CONFIRMING_ANSWER);
    tick_ms(1000);

    check_int("scenario 3 did not stop as answered",
              alert_dispatch_get_call_escalation_state() == ALERT_CALL_ESCALATION_ANSWERED,
              0);
    check_int("scenario 3 started next attempt after brief active ended",
              modem_get_simulated_call_count(),
              2);
    check_string("scenario 3 next attempt is contact 1 retry",
                 modem_get_simulated_call_number_at(1),
                 "+911111111111");
#endif
}

static void run_all_contacts_exhausted(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ENDED};

    printf("\nScenario 4: all contacts exhausted without answer\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 6);

    check_int("scenario 4 dispatch starts", alert_dispatch_send("smoke"), 0);
    check_sms_fanout();

    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);
    tick_ms(1000);

    check_int("scenario 4 six attempts made",
              modem_get_simulated_call_count(),
              6);
    check_string("scenario 4 call 1 contact 1", modem_get_simulated_call_number_at(0), "+911111111111");
    check_string("scenario 4 call 2 contact 1", modem_get_simulated_call_number_at(1), "+911111111111");
    check_string("scenario 4 call 3 contact 2", modem_get_simulated_call_number_at(2), "+912222222222");
    check_string("scenario 4 call 4 contact 2", modem_get_simulated_call_number_at(3), "+912222222222");
    check_string("scenario 4 call 5 contact 3", modem_get_simulated_call_number_at(4), "+913333333333");
    check_string("scenario 4 call 6 contact 3", modem_get_simulated_call_number_at(5), "+913333333333");
    check_int("scenario 4 ends cleanly",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_DONE);

    tick_ms(1000);
    check_int("scenario 4 no extra calls after done",
              modem_get_simulated_call_count(),
              6);
#endif
}

int main(void)
{
    const char *config_path = "/tmp/amtech_alert_dispatch_config_for_test.txt";

    write_test_config(config_path);
    setenv("AMTECH_CONFIG_PATH", config_path, 1);

#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    check_message("panic",
                  "AMTECH ALERT: Panic button pressed at your shop. Immediate attention needed.");
    check_message("shutter-1",
                  "AMTECH ALERT: Shutter 1 intrusion detected at your shop.");
    check_message("shutter-2",
                  "AMTECH ALERT: Shutter 2 intrusion detected at your shop.");
    check_message("intrusion",
                  "AMTECH ALERT: Person detected inside your shop while armed.");
    check_message("smoke",
                  "AMTECH ALERT: Smoke detected at your shop. Possible fire emergency.");

    run_contact1_answered_first_attempt();
    run_contact1_twice_then_contact2_answered();
    run_brief_active_false_positive();
    run_all_contacts_exhausted();

    unsetenv("AMTECH_CONFIG_PATH");
    remove(config_path);

    if (failures == 0)
    {
        printf("PASS: alert dispatch simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: alert dispatch simulation had %d failure(s)\n", failures);
    return 1;
}
