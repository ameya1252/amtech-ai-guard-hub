#ifndef AMTECH_CONFIG_SYNC_H
#define AMTECH_CONFIG_SYNC_H

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_CONFIG_SYNC_POLL_MS 300000U

int amtech_config_sync_parse_json(const char *json, amtech_config_t *config);
int amtech_config_sync_update_file(const char *path, const amtech_config_t *config);
int amtech_config_sync_poll(const char *config_path, const char *shop_id, amtech_config_t *config);

#ifdef SIMULATE_NETWORK
void amtech_config_sync_set_simulated_response(const char *json);
#endif

#ifdef __cplusplus
}
#endif

#endif
