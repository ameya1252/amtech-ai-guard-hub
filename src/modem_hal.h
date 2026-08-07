#ifndef AMTECH_MODEM_HAL_H
#define AMTECH_MODEM_HAL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Application code should call only this HAL, never raw AT helpers.
 * If the modem changes later, update this layer plus modem_state/sim_modem;
 * the rest of the Guard Hub application should not know modem-specific AT
 * commands.
 */
int modem_get_registration_status(void);
int modem_connect_data(void);
int modem_send_sms(const char *number, const char *message);
int modem_make_voice_call(const char *number);

#ifdef __cplusplus
}
#endif

#endif
