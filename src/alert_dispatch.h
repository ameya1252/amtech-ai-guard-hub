#ifndef AMTECH_ALERT_DISPATCH_H
#define AMTECH_ALERT_DISPATCH_H

#ifdef __cplusplus
extern "C" {
#endif

int alert_dispatch_send(const char *event_type);
void alert_dispatch_tick(unsigned int elapsed_ms);
const char *alert_dispatch_message_for_event(const char *event_type);

typedef enum
{
    ALERT_CALL_ESCALATION_IDLE = 0,
    ALERT_CALL_ESCALATION_WAITING,
    ALERT_CALL_ESCALATION_CONFIRMING_ANSWER,
    ALERT_CALL_ESCALATION_ANSWERED,
    ALERT_CALL_ESCALATION_DONE,
    ALERT_CALL_ESCALATION_FAILED
} alert_call_escalation_state_t;

alert_call_escalation_state_t alert_dispatch_get_call_escalation_state(void);
int alert_dispatch_get_current_contact_index(void);
int alert_dispatch_get_current_attempt(void);
void alert_dispatch_reset(void);

#ifdef __cplusplus
}
#endif

#endif
