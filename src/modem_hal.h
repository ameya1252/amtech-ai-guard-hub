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
int modem_register_network(void);
int modem_send_sms(const char *number, const char *message);
int modem_make_voice_call(const char *number);
void modem_hal_tick(unsigned int elapsed_ms);
int modem_voice_call_is_active(void);

#ifdef SIMULATE_MODEM
int modem_get_simulated_sms_count(void);
int modem_get_simulated_call_count(void);
int modem_get_simulated_hangup_count(void);
const char *modem_get_simulated_last_sms_number(void);
const char *modem_get_simulated_last_sms_message(void);
const char *modem_get_simulated_last_call_number(void);
void modem_reset_simulated_state(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
