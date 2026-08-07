#ifndef AMTECH_NOTIFY_CLIENT_H
#define AMTECH_NOTIFY_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

int notify_send_alert(const char *shop_id, const char *event_type);

#ifdef SIMULATE_NETWORK
int notify_get_simulated_send_count(void);
void notify_reset_simulated_send_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
