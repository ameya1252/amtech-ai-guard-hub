#ifndef AMTECH_NOTIFY_CLIENT_H
#define AMTECH_NOTIFY_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

int notify_send_alert(const char *shop_id, const char *event_type);

#ifdef __cplusplus
}
#endif

#endif
