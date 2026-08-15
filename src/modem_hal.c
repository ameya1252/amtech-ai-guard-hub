#include "modem_hal.h"

#include "modem_state.h"

#include <stdio.h>

int modem_get_registration_status(void)
{
    return (int)modem_state_get_current();
}

int modem_register_network(void)
{
    int tick_result;

    if (modem_state_is_registered())
    {
        return 1;
    }

    tick_result = modem_state_tick();
    if (tick_result != 0)
    {
        return -1;
    }

    return modem_state_is_registered() ? 1 : 0;
}

int modem_send_sms(const char *number, const char *message)
{
    (void)number;
    (void)message;

    printf("Modem HAL: SMS sending is not implemented yet\n");
    return -1;
}

int modem_make_voice_call(const char *number)
{
    (void)number;

    printf("Modem HAL: voice calling is not implemented yet\n");
    return -1;
}
