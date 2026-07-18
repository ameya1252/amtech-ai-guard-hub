#ifndef AMTECH_ALARM_LOGIC_H
#define AMTECH_ALARM_LOGIC_H

#ifdef __cplusplus
extern "C" {
#endif

void alarm_logic_init(int gpio_pin);
void alarm_logic_set_armed(int armed);
void alarm_logic_toggle_armed(void);
int alarm_logic_is_armed(void);
int alarm_logic_is_triggered(void);
void alarm_logic_handle_detection(int class_id, const char *class_name, float confidence);
void alarm_logic_handle_shutter_sensor(int triggered);
void alarm_logic_end_frame(void);
void trigger_alarm(void);

#ifdef __cplusplus
}
#endif

#endif
