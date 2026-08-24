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

static void tick_seconds(int seconds)
{
    int i;

    for (i = 0; i < seconds; i++)
    {
        tick_ms(1000);
    }
}

static void tick_call_retry_gap(void)
{
    tick_ms(5000);
}

static void tick_ended_attempt_to_next_call(void)
{
    tick_ms(1000);
    tick_call_retry_gap();
}

static void check_sms_fanout(void)
{
#ifdef SIMULATE_MODEM
    check_int("SMS fan-out worker finished", alert_dispatch_test_wait_idle(5000), 0);
    check_int("SMS fan-out count", modem_get_simulated_sms_count(), 3);
    check_string("SMS contact 1", modem_get_simulated_sms_number_at(0), "+911111111111");
    check_string("SMS contact 2", modem_get_simulated_sms_number_at(1), "+912222222222");
    check_string("SMS contact 3", modem_get_simulated_sms_number_at(2), "+913333333333");
#endif
}

static void run_slow_sustained_active_stops_escalation(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE};

    printf("\nScenario 1: slow sustained active call stops escalation as likely human answer\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 6);

    check_int("scenario 1 dispatch starts", alert_dispatch_send("panic"), 0);
    check_sms_fanout();
    check_int("scenario 1 first call count", modem_get_simulated_call_count(), 1);
    check_string("scenario 1 first call number", modem_get_simulated_call_number_at(0), "+911111111111");

    tick_seconds(6);
    check_int("scenario 1 confirmed answered",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_ANSWERED);
    check_int("scenario 1 no calls to contacts 2/3 after answer",
              modem_get_simulated_call_count(),
              1);

    tick_seconds(30);
    check_int("scenario 1 remains stopped after more ticks",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_ANSWERED);
    check_int("scenario 1 still no extra calls",
              modem_get_simulated_call_count(),
              1);
#endif
}

static void run_fast_sustained_active_does_not_stop_escalation(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE};

    printf("\nScenario 1b: fast sustained active call is suspected voicemail and does not stop escalation\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 30);

    check_int("scenario 1b dispatch starts", alert_dispatch_send("panic"), 0);
    check_sms_fanout();

    tick_seconds(4);
    check_int("scenario 1b sustained fast active still waiting, not answered",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_WAITING);
    check_int("scenario 1b still only first call before timeout",
              modem_get_simulated_call_count(),
              1);

    tick_seconds(26);
    check_int("scenario 1b fast active enters retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 1b fast active timed out and moved to retry",
              modem_get_simulated_call_count(),
              2);
    check_string("scenario 1b retry still contact 1",
                 modem_get_simulated_call_number_at(1),
                 "+911111111111");
#endif
}

static void run_contact1_twice_then_contact2_called(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ENDED};

    printf("\nScenario 2: contact 1 unanswered twice, contact 2 is called\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 4);

    check_int("scenario 2 dispatch starts", alert_dispatch_send("shutter-1"), 0);
    check_sms_fanout();

    tick_seconds(2);
    check_int("scenario 2 enters retry gap before contact 1 attempt 2",
              modem_get_simulated_call_count(),
              1);
    check_int("scenario 2 retry gap state",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 2 moved to contact 1 attempt 2 call count",
              modem_get_simulated_call_count(),
              2);
    check_string("scenario 2 second call still contact 1",
                 modem_get_simulated_call_number_at(1),
                 "+911111111111");

    tick_seconds(2);
    check_int("scenario 2 enters retry gap before contact 2",
              modem_get_simulated_call_count(),
              2);
    tick_call_retry_gap();
    check_int("scenario 2 moved to contact 2 call count",
              modem_get_simulated_call_count(),
              3);
    check_string("scenario 2 third call contact 2",
                 modem_get_simulated_call_number_at(2),
                 "+912222222222");
#endif
}

static void run_brief_active_then_ended_advances(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_RINGING,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ENDED};

    printf("\nScenario 3: brief active state followed by end advances\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 3);

    check_int("scenario 3 dispatch starts", alert_dispatch_send("intrusion"), 0);
    check_sms_fanout();

    tick_seconds(2);
    check_int("scenario 3 starts confirming a slow active call",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_CONFIRMING_ANSWER);
    tick_ms(1000);

    check_int("scenario 3 enters retry gap after brief active ended",
              modem_get_simulated_call_count(),
              1);
    tick_call_retry_gap();
    check_int("scenario 3 started next attempt after retry gap",
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

    tick_ended_attempt_to_next_call();
    tick_ended_attempt_to_next_call();
    tick_ended_attempt_to_next_call();
    tick_ended_attempt_to_next_call();
    tick_ended_attempt_to_next_call();
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
    tick_call_retry_gap();
    check_int("scenario 4 ends cleanly",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_DONE);

    check_int("scenario 4 no extra calls after done",
              modem_get_simulated_call_count(),
              6);
#endif
}

static void run_voicemail_on_contact2_does_not_block_contact3(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE};

    printf("\nScenario 5: contact 2 voicemail does not block contact 3\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 32);

    check_int("scenario 5 dispatch starts", alert_dispatch_send("shutter-2"), 0);
    check_sms_fanout();

    tick_ended_attempt_to_next_call();
    tick_ended_attempt_to_next_call();
    check_int("scenario 5 reached contact 2",
              modem_get_simulated_call_count(),
              3);
    check_string("scenario 5 third call is contact 2",
                 modem_get_simulated_call_number_at(2),
                 "+912222222222");

    tick_seconds(30);
    check_int("scenario 5 contact 2 voicemail enters retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 5 contact 2 voicemail times out and retries contact 2",
              modem_get_simulated_call_count(),
              4);
    check_string("scenario 5 fourth call is contact 2 retry",
                 modem_get_simulated_call_number_at(3),
                 "+912222222222");

    tick_seconds(30);
    check_int("scenario 5 contact 2 retry voicemail enters retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 5 contact 2 retry voicemail times out and reaches contact 3",
              modem_get_simulated_call_count(),
              5);
    check_string("scenario 5 fifth call is contact 3",
                 modem_get_simulated_call_number_at(4),
                 "+913333333333");
#endif
}

static void run_dial_start_failure_advances_escalation(void)
{
#ifdef SIMULATE_MODEM
    modem_call_status_t statuses[] = {
        MODEM_CALL_STATUS_ENDED,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE,
        MODEM_CALL_STATUS_ACTIVE};
    int call_start_results[] = {
        0,
        -1,
        0};

    printf("\nScenario 6: dial-start failure is treated as unanswered and escalation continues\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_call_status_sequence(statuses, 4);
    modem_set_simulated_call_start_results(call_start_results, 3);

    check_int("scenario 6 dispatch starts", alert_dispatch_send("shutter-2"), 0);
    check_sms_fanout();

    tick_ms(1000);
    check_int("scenario 6 first call ended into retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 6 failed retry attempted after settle gap",
              modem_get_simulated_call_count(),
              2);
    check_int("scenario 6 failed retry enters another settle gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 6 failed retry then advances to contact 2",
              modem_get_simulated_call_count(),
              3);
    check_string("scenario 6 first call contact 1",
                 modem_get_simulated_call_number_at(0),
                 "+911111111111");
    check_string("scenario 6 failed retry was contact 1",
                 modem_get_simulated_call_number_at(1),
                 "+911111111111");
    check_string("scenario 6 third call advances to contact 2",
                 modem_get_simulated_call_number_at(2),
                 "+912222222222");
    check_int("scenario 6 escalation remains active after dial failure",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_WAITING);

    tick_seconds(30);
    check_int("scenario 6 contact 2 timeout enters retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 6 contact 2 active call still continues to retry",
              modem_get_simulated_call_count(),
              4);
#endif
}

static void run_slow_sms_fanout_does_not_delay_first_call(void)
{
#ifdef SIMULATE_MODEM
    printf("\nScenario 7: slow SMS fan-out does not delay first voice call\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_sms_send_delay_ms(1000);

    check_int("scenario 7 dispatch starts", alert_dispatch_send("panic"), 0);
    check_int("scenario 7 first call starts before SMS fan-out finishes",
              modem_get_simulated_call_count(),
              1);
    check_string("scenario 7 first call is contact 1",
                 modem_get_simulated_call_number_at(0),
                 "+911111111111");
    check_int("scenario 7 SMS worker eventually finishes", alert_dispatch_test_wait_idle(5000), 0);
    check_int("scenario 7 all contacts still receive SMS",
              modem_get_simulated_sms_count(),
              3);
    modem_set_simulated_sms_send_delay_ms(0);
#endif
}

static void run_sms_total_failure_does_not_interrupt_call_sequence(void)
{
#ifdef SIMULATE_MODEM
    int sms_results[] = {
        -1,
        -1,
        -1,
        -1,
        -1,
        -1};
    int attempt;

    printf("\nScenario 8: total SMS failure does not interrupt full call escalation\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();
    modem_set_simulated_sms_send_results(sms_results, 6);

    check_int("scenario 8 dispatch starts", alert_dispatch_send("panic"), 0);
    check_int("scenario 8 first call starts immediately despite SMS work",
              modem_get_simulated_call_count(),
              1);
    check_string("scenario 8 first call is contact 1",
                 modem_get_simulated_call_number_at(0),
                 "+911111111111");

    check_int("scenario 8 SMS worker finishes after failed retries",
              alert_dispatch_test_wait_idle(5000),
              0);
    check_int("scenario 8 no SMS sends succeeded",
              modem_get_simulated_sms_count(),
              0);
    check_int("scenario 8 all SMS retry attempts were made",
              modem_get_simulated_sms_attempt_count(),
              6);
    check_int("scenario 8 SMS TX fault is active",
              modem_sms_tx_fault_active(),
              1);

    for (attempt = 1; attempt <= 5; attempt++)
    {
        char label[96];

        tick_seconds(30);
        snprintf(label, sizeof(label), "scenario 8 attempt %d timeout enters retry gap", attempt);
        check_int(label,
                  alert_dispatch_get_call_escalation_state(),
                  ALERT_CALL_ESCALATION_RETRY_GAP);
        snprintf(label, sizeof(label), "scenario 8 attempt %d sent ATH before next call", attempt);
        check_int(label, modem_get_simulated_hangup_count(), attempt);

        tick_call_retry_gap();
        snprintf(label, sizeof(label), "scenario 8 call count after retry gap %d", attempt);
        check_int(label, modem_get_simulated_call_count(), attempt + 1);
    }

    tick_seconds(30);
    check_int("scenario 8 final attempt timeout enters retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    check_int("scenario 8 final attempt sent ATH",
              modem_get_simulated_hangup_count(),
              6);
    tick_call_retry_gap();

    check_int("scenario 8 exactly six call attempts made",
              modem_get_simulated_call_count(),
              6);
    check_string("scenario 8 call 1 contact 1", modem_get_simulated_call_number_at(0), "+911111111111");
    check_string("scenario 8 call 2 contact 1", modem_get_simulated_call_number_at(1), "+911111111111");
    check_string("scenario 8 call 3 contact 2", modem_get_simulated_call_number_at(2), "+912222222222");
    check_string("scenario 8 call 4 contact 2", modem_get_simulated_call_number_at(3), "+912222222222");
    check_string("scenario 8 call 5 contact 3", modem_get_simulated_call_number_at(4), "+913333333333");
    check_string("scenario 8 call 6 contact 3", modem_get_simulated_call_number_at(5), "+913333333333");
    check_int("scenario 8 escalation ends cleanly after all contacts",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_DONE);
#endif
}

static void run_retrigger_does_not_reset_active_call_escalation(void)
{
#ifdef SIMULATE_MODEM
    printf("\nScenario 9: retrigger while call escalation is active does not reset to contact 1\n");
    modem_reset_simulated_state();
    alert_dispatch_reset();

    check_int("scenario 9 first dispatch starts", alert_dispatch_send("shutter-1"), 0);
    check_sms_fanout();
    check_int("scenario 9 first call is contact 1 attempt 1",
              modem_get_simulated_call_count(),
              1);
    check_string("scenario 9 first call number",
                 modem_get_simulated_call_number_at(0),
                 "+911111111111");

    check_int("scenario 9 retrigger dispatch suppressed while first call active",
              alert_dispatch_send("intrusion-front"),
              -1);
    check_int("scenario 9 retrigger did not start extra call",
              modem_get_simulated_call_count(),
              1);

    tick_seconds(30);
    check_int("scenario 9 first attempt entered retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);

    check_int("scenario 9 retrigger dispatch suppressed during retry gap",
              alert_dispatch_send("intrusion-front"),
              -1);
    check_int("scenario 9 still no reset before retry gap expires",
              modem_get_simulated_call_count(),
              1);

    tick_call_retry_gap();
    check_int("scenario 9 original escalation continues to contact 1 attempt 2",
              modem_get_simulated_call_count(),
              2);
    check_string("scenario 9 second call still contact 1",
                 modem_get_simulated_call_number_at(1),
                 "+911111111111");

    tick_seconds(30);
    check_int("scenario 9 second attempt entered retry gap",
              alert_dispatch_get_call_escalation_state(),
              ALERT_CALL_ESCALATION_RETRY_GAP);
    tick_call_retry_gap();
    check_int("scenario 9 original escalation reaches contact 2",
              modem_get_simulated_call_count(),
              3);
    check_string("scenario 9 third call is contact 2",
                 modem_get_simulated_call_number_at(2),
                 "+912222222222");
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
                  "AMTECH ALERT: Person detected inside your shop. System temporarily disarmed; will re-arm after siren stops.");
    check_message("intrusion-front",
                  "AMTECH ALERT: Person detected on the front camera. System temporarily disarmed; will re-arm after siren stops.");
    check_message("intrusion-parking",
                  "AMTECH ALERT: Person detected on the parking camera. System temporarily disarmed; will re-arm after siren stops.");
    check_message("smoke",
                  "AMTECH ALERT: Smoke detected at your shop. Possible fire emergency.");

    run_slow_sustained_active_stops_escalation();
    run_fast_sustained_active_does_not_stop_escalation();
    run_contact1_twice_then_contact2_called();
    run_brief_active_then_ended_advances();
    run_all_contacts_exhausted();
    run_voicemail_on_contact2_does_not_block_contact3();
    run_dial_start_failure_advances_escalation();
    run_slow_sms_fanout_does_not_delay_first_call();
    run_sms_total_failure_does_not_interrupt_call_sequence();
    run_retrigger_does_not_reset_active_call_escalation();

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
