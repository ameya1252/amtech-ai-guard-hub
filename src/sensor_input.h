#ifndef AMTECH_SENSOR_INPUT_H
#define AMTECH_SENSOR_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SIMULATE_GPIO
extern int sensor_input_simulated_state;
void sensor_input_set_simulated_raw_value(int pin, int raw_value);
#endif

typedef enum
{
    SHUTTER_CLOSED = 0,
    SHUTTER_OPEN,
    SHUTTER_TAMPER,
    SHUTTER_FAULT
} shutter_state_t;

int sensor_input_init(int pin);
int sensor_input_read(int pin);
shutter_state_t shutter_read_dual_state(int nc_pin, int no_pin);
const char *shutter_state_to_string(shutter_state_t state);

#ifdef __cplusplus
}
#endif

#endif
