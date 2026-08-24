#include "modem_hal.h"
#include "modem_state.h"

#ifdef SIMULATE_GPIO
#include "gpio_control.h"
#endif

#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    int result;
    int ticks;

    unsetenv("AMTECH_SIM_MODEM_FAIL_COMMAND");
    modem_state_init();

#if defined(SIMULATE_MODEM) && defined(SIMULATE_GPIO)
    gpio_reset_simulated_values();
    modem_reset_simulated_state();
    check_int("HAL modem reset GPIO init succeeds", modem_reset_control_init(), 0);
    check_int("HAL modem reset GPIO initializes LOW/normal", gpio_get_simulated_value(57), 0);
    check_int("HAL modem hard reset succeeds", modem_hard_reset(), 0);
    check_int("HAL modem hard reset releases GPIO LOW/normal", gpio_get_simulated_value(57), 0);
    check_int("HAL modem hard reset restarts modem state", modem_get_registration_status(), MODEM_STATE_POWER_OFF);
#endif

    check_int("HAL initial status is POWER_OFF",
              modem_get_registration_status(),
              MODEM_STATE_POWER_OFF);

    result = modem_register_network();
    check_int("HAL first registration tick is still in progress", result, 0);
    check_int("HAL status advanced to BOOTING",
              modem_get_registration_status(),
              MODEM_STATE_BOOTING);

    for (ticks = 0; ticks < 16 && result == 0; ticks++)
    {
        result = modem_register_network();
        printf("HAL registration tick %d result=%d status=%s\n",
               ticks + 2,
               result,
               modem_state_name((modem_state_t)modem_get_registration_status()));
    }

    check_int("HAL reports registered", result, 1);
    check_int("HAL final status is REGISTERED",
              modem_get_registration_status(),
              MODEM_STATE_REGISTERED);

#ifdef SIMULATE_MODEM
    modem_reset_simulated_state();
#endif

    check_int("HAL SMS simulation succeeds", modem_send_sms("+911234567890", "test"), 0);
    check_int("HAL SMS attempts after first success", modem_get_simulated_sms_attempt_count(), 1);
    check_int("HAL SMS TX fault clear after first success", modem_sms_tx_fault_active(), 0);

#ifdef SIMULATE_MODEM
    {
        int fail_then_success[] = {-1, 0};
        int fail_twice[] = {-1, -1};

        modem_set_simulated_sms_send_results(fail_then_success, 2);
        check_int("HAL SMS retry succeeds after one failure", modem_send_sms("+911234567891", "retry"), 0);
        check_int("HAL SMS attempts include retry success", modem_get_simulated_sms_attempt_count(), 3);
        check_int("HAL SMS TX fault remains clear after retry success", modem_sms_tx_fault_active(), 0);

        modem_set_simulated_sms_send_results(fail_twice, 2);
        check_int("HAL SMS fails after retry exhausted", modem_send_sms("+911234567892", "fail"), -1);
        check_int("HAL SMS attempts include retry failure", modem_get_simulated_sms_attempt_count(), 5);
        check_int("HAL SMS TX fault active after retry failure", modem_sms_tx_fault_active(), 1);

        modem_set_simulated_sms_send_results(NULL, 0);
        check_int("HAL later SMS success clears TX fault", modem_send_sms("+911234567893", "clear"), 0);
        check_int("HAL SMS TX fault cleared by later success", modem_sms_tx_fault_active(), 0);
    }
#endif

    check_int("HAL voice call simulation starts", modem_make_voice_call("+911234567890"), 0);
    check_int("HAL voice call active after start", modem_voice_call_is_active(), 1);
    modem_hal_tick(44000);
    check_int("HAL voice call still active before safety timeout", modem_voice_call_is_active(), 1);
    modem_hal_tick(1000);
    check_int("HAL voice call cleared at safety timeout", modem_voice_call_is_active(), 0);
#ifdef SIMULATE_MODEM
    check_int("HAL simulated SMS count", modem_get_simulated_sms_count(), 3);
    check_int("HAL simulated call count", modem_get_simulated_call_count(), 1);
    check_int("HAL simulated hangup count", modem_get_simulated_hangup_count(), 1);
#endif

#if defined(SIMULATE_MODEM) && defined(SIMULATE_GPIO)
    {
        int fail_six_times[] = {-1, -1, -1, -1, -1, -1};

        modem_reset_simulated_state();
        gpio_reset_simulated_values();
        check_int("HAL reset policy GPIO init succeeds", modem_reset_control_init(), 0);
        modem_set_simulated_sms_send_results(fail_six_times, 6);
        check_int("HAL reset policy first SMS failure", modem_send_sms("+911111111111", "fail1"), -1);
        check_int("HAL reset policy no reset after first high-level failure",
                  modem_get_simulated_hard_reset_count(),
                  0);
        check_int("HAL reset policy second SMS failure", modem_send_sms("+911111111111", "fail2"), -1);
        check_int("HAL reset policy no reset after second high-level failure",
                  modem_get_simulated_hard_reset_count(),
                  0);
        check_int("HAL reset policy third SMS failure", modem_send_sms("+911111111111", "fail3"), -1);
        check_int("HAL reset policy triggers one hard reset after bounded failures",
                  modem_get_simulated_hard_reset_count(),
                  1);
        check_int("HAL reset policy leaves reset GPIO LOW/normal",
                  gpio_get_simulated_value(57),
                  0);

        modem_set_simulated_sms_send_results(fail_six_times, 6);
        check_int("HAL reset cooldown SMS failure 1", modem_send_sms("+911111111111", "cooldown1"), -1);
        check_int("HAL reset cooldown SMS failure 2", modem_send_sms("+911111111111", "cooldown2"), -1);
        check_int("HAL reset cooldown SMS failure 3", modem_send_sms("+911111111111", "cooldown3"), -1);
        check_int("HAL reset cooldown suppresses immediate second hard reset",
                  modem_get_simulated_hard_reset_count(),
                  1);
        modem_hal_tick(300000);
        modem_set_simulated_sms_send_results(fail_six_times, 2);
        check_int("HAL reset cooldown later SMS failure retriggers reset",
                  modem_send_sms("+911111111111", "after cooldown"),
                  -1);
        check_int("HAL reset cooldown allows reset after cooldown",
                  modem_get_simulated_hard_reset_count(),
                  2);
    }

    {
        int fail_six_times[] = {-1, -1, -1, -1, -1, -1};

        modem_reset_simulated_state();
        gpio_reset_simulated_values();
        check_int("HAL deferred reset GPIO init succeeds", modem_reset_control_init(), 0);
        check_int("HAL deferred reset voice call starts", modem_make_voice_call("+911111111111"), 0);
        modem_set_simulated_sms_send_results(fail_six_times, 6);
        check_int("HAL deferred reset SMS failure 1", modem_send_sms("+912222222222", "fail1"), -1);
        check_int("HAL deferred reset SMS failure 2", modem_send_sms("+912222222222", "fail2"), -1);
        check_int("HAL deferred reset SMS failure 3", modem_send_sms("+912222222222", "fail3"), -1);
        check_int("HAL deferred reset does not reset during active call",
                  modem_get_simulated_hard_reset_count(),
                  0);
        modem_hal_tick(45000);
        check_int("HAL deferred reset ends active call at timeout", modem_voice_call_is_active(), 0);
        check_int("HAL deferred reset runs after call ends",
                  modem_get_simulated_hard_reset_count(),
                  1);
    }
#endif

    if (failures == 0)
    {
        printf("PASS: modem HAL simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: modem HAL simulation had %d failure(s)\n", failures);
    return 1;
}
