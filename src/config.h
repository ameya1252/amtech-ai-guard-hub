#ifndef AMTECH_CONFIG_H
#define AMTECH_CONFIG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_DEFAULT_CONFIG_PATH "/root/amtech_config.txt"
#define AMTECH_DEFAULT_MODEM_DEVICE "/dev/ttyS5"
#define AMTECH_ALERT_CONTACT_COUNT 3
#define AMTECH_DEFAULT_ALERT_CONTACT_1 "+918550991121"
#define AMTECH_DEFAULT_ALERT_CONTACT_2 "+919922434811"
#define AMTECH_DEFAULT_ALERT_CONTACT_3 "+919922435710"
#define AMTECH_DEFAULT_SCHEDULE_ARM_HOUR 23
#define AMTECH_DEFAULT_SCHEDULE_ARM_MINUTE 0
#define AMTECH_DEFAULT_SCHEDULE_DISARM_HOUR 6
#define AMTECH_DEFAULT_SCHEDULE_DISARM_MINUTE 0
#define AMTECH_DEFAULT_BACKEND_BASE_URL "https://amtech-ai-guard-hub-production.up.railway.app"
#define AMTECH_MODEM_DEVICE_MAX 64
#define AMTECH_ALERT_CONTACT_NUMBER_MAX 24
#define AMTECH_CAMERA_RTSP_URL_MAX 256
#define AMTECH_BACKEND_URL_MAX 256
#define AMTECH_DEVICE_CONFIG_TOKEN_MAX 128

typedef struct
{
    int shutter_count;
    int panic_enabled;
    int smoke_enabled;
    int schedule_arm_hour;
    int schedule_arm_minute;
    int schedule_disarm_hour;
    int schedule_disarm_minute;
    char modem_device[AMTECH_MODEM_DEVICE_MAX];
    /*
     * V1 local placeholder. Later this should come from the app/backend's
     * stored per-shop emergency contact list instead of a device config file.
     * Contacts are ordered by call-escalation priority.
     */
    char alert_contacts[AMTECH_ALERT_CONTACT_COUNT][AMTECH_ALERT_CONTACT_NUMBER_MAX];
    int camera_enabled;
    char camera_rtsp_url[AMTECH_CAMERA_RTSP_URL_MAX];
    int camera2_enabled;
    char camera2_rtsp_url[AMTECH_CAMERA_RTSP_URL_MAX];
    char backend_base_url[AMTECH_BACKEND_URL_MAX];
    char device_config_token[AMTECH_DEVICE_CONFIG_TOKEN_MAX];
} amtech_config_t;

void amtech_config_set_defaults(amtech_config_t *config);
int amtech_config_load(const char *path, amtech_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
