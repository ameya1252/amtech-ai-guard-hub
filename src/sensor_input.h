#ifndef AMTECH_SENSOR_INPUT_H
#define AMTECH_SENSOR_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef SIMULATE_GPIO
extern int sensor_input_simulated_state;
#endif

int sensor_input_init(int pin);
int sensor_input_read(int pin);

#ifdef __cplusplus
}
#endif

#endif
