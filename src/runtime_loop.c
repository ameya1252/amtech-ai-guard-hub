#include "alarm_logic.h"
#include "schedule.h"
#include "sensor_input.h"

#include <stdio.h>

#define AMTECH_ALARM_GPIO_PIN 42
#define AMTECH_SHUTTER_GPIO_PIN 27
#define AMTECH_SHOP_ID "amtech-demo-shop"
#define AMTECH_RUNTIME_TEST_ITERATIONS 10

static void runtime_iteration(int iteration)
{
    int shutter_triggered;
    int should_be_armed;

    printf("Runtime: iteration %d\n", iteration);

    schedule_tick();

    should_be_armed = schedule_should_be_armed(23, 30);
    alarm_logic_set_armed(should_be_armed);

#ifdef SIMULATE_GPIO
    sensor_input_simulated_state = iteration == 4 ? 1 : 0;
#endif

    shutter_triggered = sensor_input_read(AMTECH_SHUTTER_GPIO_PIN);
    alarm_logic_handle_shutter_sensor(shutter_triggered);

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
    sensor_input_init(AMTECH_SHUTTER_GPIO_PIN);
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
