#include "modem_state.h"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

static void check_state(const char *label, modem_state_t expected)
{
    modem_state_t actual = modem_state_get_current();
    const char *result = actual == expected ? "PASS" : "FAIL";

    printf("%s: got %s, expected %s: %s\n",
           label,
           modem_state_name(actual),
           modem_state_name(expected),
           result);

    if (actual != expected)
    {
        failures++;
    }
}

static void check_int(const char *label, int actual, int expected)
{
    const char *result = actual == expected ? "PASS" : "FAIL";

    printf("%s: got %d, expected %d: %s\n", label, actual, expected, result);

    if (actual != expected)
    {
        failures++;
    }
}

static void tick_expect(const char *label, modem_state_t expected)
{
    int result = modem_state_tick();

    printf("%s tick result: %d\n", label, result);
    check_state(label, expected);
}

static void check_failure_path(void)
{
    int index;

    setenv("AMTECH_SIM_MODEM_FAIL_COMMAND", "AT+CPIN?", 1);
    modem_state_init();

    tick_expect("failure path starts booting", MODEM_STATE_BOOTING);
    tick_expect("failure path reaches modem ready", MODEM_STATE_MODEM_READY);

    for (index = 1; index < 5; index++)
    {
        int result = modem_state_tick();

        printf("CPIN failure retry %d tick result: %d\n", index, result);
        check_state("CPIN failure stays in MODEM_READY before retry limit", MODEM_STATE_MODEM_READY);
    }

    {
        int result = modem_state_tick();

        printf("CPIN failure retry limit tick result: %d\n", result);
        check_state("CPIN failure reaches FAILED after retry limit", MODEM_STATE_FAILED);
    }

    check_int("registration blocked in FAILED", modem_state_is_registered(), 0);
    check_int("FAILED state tick returns error", modem_state_tick(), -1);
    check_state("FAILED state remains terminal", MODEM_STATE_FAILED);

    unsetenv("AMTECH_SIM_MODEM_FAIL_COMMAND");
}

int main(void)
{
    unsetenv("AMTECH_SIM_MODEM_FAIL_COMMAND");

    modem_state_init();

    check_state("initial state", MODEM_STATE_POWER_OFF);
    check_int("registration unavailable before boot", modem_state_is_registered(), 0);

    tick_expect("power off advances to booting", MODEM_STATE_BOOTING);
    check_int("registration unavailable while booting", modem_state_is_registered(), 0);

    tick_expect("AT OK advances to modem ready", MODEM_STATE_MODEM_READY);
    check_int("registration unavailable before SIM ready", modem_state_is_registered(), 0);

    tick_expect("CPIN READY advances to SIM ready", MODEM_STATE_SIM_READY);
    tick_expect("SIM ready starts registration polling", MODEM_STATE_REGISTERING);
    check_int("registration unavailable while polling network", modem_state_is_registered(), 0);

    tick_expect("CEREG home registration advances to registered", MODEM_STATE_REGISTERED);
    check_int("SMS/voice gate opens after registered", modem_state_is_registered(), 1);

    tick_expect("registered remains stable", MODEM_STATE_REGISTERED);

    check_failure_path();

    if (failures == 0)
    {
        printf("PASS: modem registration state machine simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: modem registration state machine simulation had %d failure(s)\n", failures);
    return 1;
}
