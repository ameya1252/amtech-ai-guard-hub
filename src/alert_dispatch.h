#ifndef AMTECH_ALERT_DISPATCH_H
#define AMTECH_ALERT_DISPATCH_H

#ifdef __cplusplus
extern "C" {
#endif

int alert_dispatch_send(const char *event_type);
void alert_dispatch_tick(unsigned int elapsed_ms);
const char *alert_dispatch_message_for_event(const char *event_type);

#ifdef __cplusplus
}
#endif

#endif
