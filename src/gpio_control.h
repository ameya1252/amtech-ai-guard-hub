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

#ifdef __cplusplus
}
#endif

#endif
