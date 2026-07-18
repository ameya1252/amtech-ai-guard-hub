#include "alarm_logic.h"
#include "schedule.h"

#include <stdio.h>

static int failures = 0;

static void check_int(const char *label, int actual, int expected)
{
    const char *result = actual == expected ? "PASS" : "FAIL";

    printf("%s: got %d, expected %d: %s\n", label, actual, expected, result);

    if (actual != expected)
    {
        failures++;
    }
}

int main(void)
{
    int i;

    schedule_set_armed_window(23, 0, 6, 0);

    check_int("22:59 outside overnight window", schedule_should_be_armed(22, 59), 0);
    check_int("23:00 starts overnight window", schedule_should_be_armed(23, 0), 1);
    check_int("23:30 inside overnight window", schedule_should_be_armed(23, 30), 1);
    check_int("00:00 inside overnight window", schedule_should_be_armed(0, 0), 1);
    check_int("05:59 inside overnight window", schedule_should_be_armed(5, 59), 1);
    check_int("06:00 ends overnight window", schedule_should_be_armed(6, 0), 0);
    check_int("12:00 outside overnight window", schedule_should_be_armed(12, 0), 0);

    schedule_set_armed_window(9, 30, 18, 0);

    check_int("09:29 outside daytime window", schedule_should_be_armed(9, 29), 0);
    check_int("09:30 starts daytime window", schedule_should_be_armed(9, 30), 1);
    check_int("17:59 inside daytime window", schedule_should_be_armed(17, 59), 1);
    check_int("18:00 ends daytime window", schedule_should_be_armed(18, 0), 0);

    alarm_logic_init(42);
    alarm_logic_set_armed(0);
    schedule_arm_requested();

    check_int("exit delay starts at 30", schedule_exit_delay_remaining(), 30);
    check_int("not armed immediately after request", alarm_logic_is_armed(), 0);

    for (i = 0; i < SCHEDULE_EXIT_DELAY_SECONDS - 1; i++)
    {
        schedule_tick();
    }

    check_int("one second remaining after 29 ticks", schedule_exit_delay_remaining(), 1);
    check_int("still disarmed before final tick", alarm_logic_is_armed(), 0);

    schedule_tick();

    check_int("exit delay complete after 30 ticks", schedule_exit_delay_remaining(), 0);
    check_int("armed after final tick", alarm_logic_is_armed(), 1);

    if (failures == 0)
    {
        printf("PASS: schedule logic behaved as expected\n");
        return 0;
    }

    printf("FAIL: schedule logic had %d failure(s)\n", failures);
    return 1;
}
