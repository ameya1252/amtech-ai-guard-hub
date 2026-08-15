#ifndef AMTECH_ALARM_LOGIC_H
#define AMTECH_ALARM_LOGIC_H

#include "sensor_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_SIREN_DURATION_MS 5000U
#define AMTECH_NOTIFICATION_COOLDOWN_MS 30000U

void alarm_logic_init(int gpio_pin);
void alarm_logic_set_shop_id(const char *shop_id);
void alarm_logic_set_armed(int armed);
void alarm_logic_toggle_armed(void);
int alarm_logic_is_armed(void);
int alarm_logic_is_triggered(void);
void alarm_logic_reset(void);
void alarm_logic_tick(unsigned int elapsed_ms);
void alarm_logic_handle_detection(int class_id, const char *class_name, float confidence);
void alarm_logic_handle_shutter_sensor(int triggered);
void alarm_logic_handle_shutter_dual(shutter_state_t state);
void alarm_logic_handle_shutter_dual_named(shutter_state_t state,
                                           const char *shutter_name,
                                           const char *event_type);
void alarm_logic_handle_panic(int triggered);
void alarm_logic_handle_smoke(int triggered);
void alarm_logic_end_frame(void);
void trigger_alarm(void);

#ifdef __cplusplus
}
#endif

#endif
