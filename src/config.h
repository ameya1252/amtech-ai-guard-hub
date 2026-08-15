#ifndef AMTECH_CONFIG_H
#define AMTECH_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_DEFAULT_CONFIG_PATH "/root/amtech_config.txt"
#define AMTECH_DEFAULT_MODEM_DEVICE "/dev/ttyS5"
#define AMTECH_MODEM_DEVICE_MAX 64

typedef struct
{
    int shutter_count;
    int panic_enabled;
    int smoke_enabled;
    char modem_device[AMTECH_MODEM_DEVICE_MAX];
} amtech_config_t;

void amtech_config_set_defaults(amtech_config_t *config);
int amtech_config_load(const char *path, amtech_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
