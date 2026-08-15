#include "modem_hal.h"
#include "modem_state.h"

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

    check_int("HAL SMS stub is not implemented", modem_send_sms("+911234567890", "test"), -1);
    check_int("HAL voice stub is not implemented", modem_make_voice_call("+911234567890"), -1);

    if (failures == 0)
    {
        printf("PASS: modem HAL simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: modem HAL simulation had %d failure(s)\n", failures);
    return 1;
}
