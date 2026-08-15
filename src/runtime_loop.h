#ifndef AMTECH_RUNTIME_LOOP_H
#define AMTECH_RUNTIME_LOOP_H

#include "config.h"
#include "sensor_input.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_RUNTIME_MAX_WATCHED_PINS 6

typedef struct
{
    int pin;
    const char *name;
    const char *edge;
} runtime_watched_pin_t;

int runtime_build_watched_pins(const amtech_config_t *config,
                               runtime_watched_pin_t watched_pins[],
                               int max_watched_pins);
int runtime_process_configured_shutters(const amtech_config_t *config);
int runtime_panic_triggered_from_raw(int raw_value);
int runtime_active_high_confirmed_from_raw_sequence(int initial_raw_value, int confirmed_raw_value);
int runtime_active_low_confirmed_from_raw_sequence(int initial_raw_value, int confirmed_raw_value);
shutter_state_t runtime_confirmed_shutter_state_from_raw_sequence(int initial_nc_raw,
                                                                  int initial_no_raw,
                                                                  int confirmed_nc_raw,
                                                                  int confirmed_no_raw);

#ifdef __cplusplus
}
#endif

#endif
