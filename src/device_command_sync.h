#ifndef AMTECH_DEVICE_COMMAND_SYNC_H
#define AMTECH_DEVICE_COMMAND_SYNC_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_DEVICE_COMMAND_POLL_MS 10000U
#define AMTECH_DEVICE_COMMAND_ID_MAX 128
#define AMTECH_DEVICE_COMMAND_SHOP_ID_MAX 128

typedef enum
{
    AMTECH_DEVICE_COMMAND_NONE = 0,
    AMTECH_DEVICE_COMMAND_ARM,
    AMTECH_DEVICE_COMMAND_DISARM
} amtech_device_command_type_t;

typedef struct
{
    amtech_device_command_type_t type;
    char id[AMTECH_DEVICE_COMMAND_ID_MAX];
} amtech_device_command_t;

int amtech_device_command_fetch(const amtech_config_t *config,
                                const char *shop_id,
                                amtech_device_command_t *command);
int amtech_device_command_ack(const amtech_config_t *config,
                              const char *shop_id,
                              const amtech_device_command_t *command);

#ifdef SIMULATE_NETWORK
void amtech_device_command_set_simulated_response(const char *json);
int amtech_device_command_simulated_ack_count(void);
void amtech_device_command_simulated_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
