#ifndef AMTECH_RUNTIME_LOOP_H
#define AMTECH_RUNTIME_LOOP_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_RUNTIME_MAX_WATCHED_PINS 5

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

#ifdef __cplusplus
}
#endif

#endif
