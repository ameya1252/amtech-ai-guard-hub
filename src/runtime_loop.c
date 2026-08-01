#include "alarm_logic.h"
#include "schedule.h"
#include "sensor_input.h"

#include <stdio.h>

#define AMTECH_ALARM_GPIO_PIN 49
#define AMTECH_STROBE_GPIO_PIN 48
#define AMTECH_SHUTTER_NC_GPIO_PIN 33
#define AMTECH_SHUTTER_NO_GPIO_PIN 40
#define AMTECH_PANIC_GPIO_PIN 32
#define AMTECH_SHOP_ID "amtech-demo-shop"
#define AMTECH_RUNTIME_TEST_ITERATIONS 10

static void runtime_iteration(int iteration)
{
    shutter_state_t shutter_state;
    int panic_triggered;
    int should_be_armed;

    printf("Runtime: iteration %d\n", iteration);

    schedule_tick();

    should_be_armed = schedule_should_be_armed(23, 30);
    alarm_logic_set_armed(should_be_armed);

#ifdef SIMULATE_GPIO
    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NC_GPIO_PIN, iteration == 4 ? 1 : 0);
    sensor_input_set_simulated_raw_value(AMTECH_SHUTTER_NO_GPIO_PIN, iteration == 4 ? 0 : 1);
#endif

    shutter_state = shutter_read_dual_state(AMTECH_SHUTTER_NC_GPIO_PIN, AMTECH_SHUTTER_NO_GPIO_PIN);
    alarm_logic_handle_shutter_dual(shutter_state);

#ifdef SIMULATE_GPIO
    sensor_input_simulated_state = iteration == 6 ? 0 : 1;
#endif

    panic_triggered = sensor_input_read(AMTECH_PANIC_GPIO_PIN);
    alarm_logic_handle_panic(panic_triggered);

    /*
     * TODO: Capture camera frame, run RKNN YOLO inference, pass each detection to
     * alarm_logic_handle_detection(), then call alarm_logic_end_frame().
     */
    alarm_logic_end_frame();
}

int main(void)
{
    int i;

    alarm_logic_init(AMTECH_ALARM_GPIO_PIN);
    alarm_logic_set_shop_id(AMTECH_SHOP_ID);
    alarm_logic_set_armed(0);
    sensor_input_init(AMTECH_SHUTTER_NC_GPIO_PIN);
    sensor_input_init(AMTECH_SHUTTER_NO_GPIO_PIN);
    sensor_input_init(AMTECH_PANIC_GPIO_PIN);
    schedule_set_armed_window(23, 0, 6, 0);

#ifdef SIMULATE_GPIO
    for (i = 0; i < AMTECH_RUNTIME_TEST_ITERATIONS; i++)
#else
    for (;;)
#endif
    {
        runtime_iteration(i);
    }

    return 0;
}
