#include "schedule.h"

#include "alarm_logic.h"

#include <stdio.h>

static int arm_time_minutes = 23 * 60;
static int disarm_time_minutes = 6 * 60;
static int exit_delay_remaining = 0;

static int is_valid_time(int hour, int minute)
{
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

static int to_minutes(int hour, int minute)
{
    return hour * 60 + minute;
}

void schedule_set_armed_window(int arm_hour, int arm_minute, int disarm_hour, int disarm_minute)
{
    if (!is_valid_time(arm_hour, arm_minute) || !is_valid_time(disarm_hour, disarm_minute))
    {
        printf("Schedule: invalid armed window %02d:%02d-%02d:%02d\n",
               arm_hour, arm_minute, disarm_hour, disarm_minute);
        return;
    }

    arm_time_minutes = to_minutes(arm_hour, arm_minute);
    disarm_time_minutes = to_minutes(disarm_hour, disarm_minute);

    printf("Schedule: armed window set to %02d:%02d-%02d:%02d\n",
           arm_hour, arm_minute, disarm_hour, disarm_minute);
}

int schedule_should_be_armed(int current_hour, int current_minute)
{
    int current_time_minutes;

    if (!is_valid_time(current_hour, current_minute))
    {
        printf("Schedule: invalid current time %02d:%02d\n", current_hour, current_minute);
        return 0;
    }

    current_time_minutes = to_minutes(current_hour, current_minute);

    if (arm_time_minutes == disarm_time_minutes)
    {
        return 0;
    }

    if (arm_time_minutes < disarm_time_minutes)
    {
        return current_time_minutes >= arm_time_minutes &&
               current_time_minutes < disarm_time_minutes;
    }

    return current_time_minutes >= arm_time_minutes ||
           current_time_minutes < disarm_time_minutes;
}

void schedule_arm_requested(void)
{
    exit_delay_remaining = SCHEDULE_EXIT_DELAY_SECONDS;
    printf("Schedule: arm requested, exit delay %d seconds\n", exit_delay_remaining);
}

void schedule_tick(void)
{
    if (exit_delay_remaining <= 0)
    {
        return;
    }

    exit_delay_remaining--;

    if (exit_delay_remaining == 0)
    {
        printf("Schedule: exit delay complete, arming system\n");
        alarm_logic_set_armed(1);
    }
}

int schedule_exit_delay_remaining(void)
{
    return exit_delay_remaining;
}
