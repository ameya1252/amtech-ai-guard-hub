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

int main(void)
{
    const char *config_path = "/tmp/amtech_alert_dispatch_config_for_test.txt";
    FILE *fp;

    fp = fopen(config_path, "w");
    if (fp == NULL)
    {
        printf("FAIL: could not write alert dispatch config\n");
        return 1;
    }
    fprintf(fp, "ALERT_CONTACT_NUMBER=+911111111111\n");
    fclose(fp);
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

    check_int("alert dispatch sends panic", alert_dispatch_send("panic"), 0);
#ifdef SIMULATE_MODEM
    check_int("panic SMS count", modem_get_simulated_sms_count(), 1);
    check_int("panic call count", modem_get_simulated_call_count(), 1);
    check_string("panic SMS number", modem_get_simulated_last_sms_number(), "+911111111111");
    check_string("panic call number", modem_get_simulated_last_call_number(), "+911111111111");
    check_string("panic SMS text",
                 modem_get_simulated_last_sms_message(),
                 "AMTECH ALERT: Panic button pressed at your shop. Immediate attention needed.");
#endif

    alert_dispatch_tick(45000);
#ifdef SIMULATE_MODEM
    check_int("alert dispatch tick safety hangup", modem_get_simulated_hangup_count(), 1);
#endif

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
