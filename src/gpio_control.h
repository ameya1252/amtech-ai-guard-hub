#ifndef AMTECH_GPIO_CONTROL_H
#define AMTECH_GPIO_CONTROL_H

#ifdef __cplusplus
extern "C" {
#endif

int gpio_export(int pin);
int gpio_set_output(int pin);
int gpio_set_output_value(int pin, int value);
int gpio_set_edge(int pin, const char *edge);
int gpio_write_value(int pin, int value);

#ifdef SIMULATE_GPIO
int gpio_get_simulated_value(int pin);
void gpio_reset_simulated_values(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
