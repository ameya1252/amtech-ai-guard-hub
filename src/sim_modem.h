#ifndef AMTECH_SIM_MODEM_H
#define AMTECH_SIM_MODEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_MODEM_DEFAULT_DEVICE "/dev/ttyS5"
#define SIM_MODEM_DEFAULT_BAUD 115200

int sim_modem_send_at(const char *command,
                      char *response_buffer,
                      size_t buffer_size,
                      int timeout_ms);
int sim_modem_connect_data(const char *apn);
int sim_modem_get_ip(char *ip_buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif
