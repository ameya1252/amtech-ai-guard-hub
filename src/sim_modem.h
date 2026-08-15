#ifndef AMTECH_SIM_MODEM_H
#define AMTECH_SIM_MODEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_MODEM_DEFAULT_BAUD 115200

int sim_modem_get_device_path(char *path_buffer, size_t buffer_size);
int sim_modem_send_at(const char *command,
                      char *response_buffer,
                      size_t buffer_size,
                      int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
