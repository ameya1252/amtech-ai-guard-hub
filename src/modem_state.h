#ifndef AMTECH_MODEM_STATE_H
#define AMTECH_MODEM_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MODEM_STATE_POWER_OFF = 0,
    MODEM_STATE_BOOTING,
    MODEM_STATE_MODEM_READY,
    MODEM_STATE_SIM_READY,
    MODEM_STATE_REGISTERING,
    MODEM_STATE_REGISTERED,
    MODEM_STATE_PDP_ACTIVE,
    MODEM_STATE_CONNECTED,
    MODEM_STATE_FAILED
} modem_state_t;

void modem_state_init(void);
modem_state_t modem_state_get_current(void);
const char *modem_state_name(modem_state_t state);
int modem_state_tick(void);
int modem_state_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif
