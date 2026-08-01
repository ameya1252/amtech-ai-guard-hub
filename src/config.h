#ifndef AMTECH_CONFIG_H
#define AMTECH_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_DEFAULT_CONFIG_PATH "/root/amtech_config.txt"

typedef struct
{
    int shutter_count;
    int panic_enabled;
} amtech_config_t;

void amtech_config_set_defaults(amtech_config_t *config);
int amtech_config_load(const char *path, amtech_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
