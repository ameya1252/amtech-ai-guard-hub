#ifndef AMTECH_SCHEDULE_H
#define AMTECH_SCHEDULE_H

#ifdef __cplusplus
extern "C" {
#endif

#define SCHEDULE_EXIT_DELAY_SECONDS 30

void schedule_set_armed_window(int arm_hour, int arm_minute, int disarm_hour, int disarm_minute);
int schedule_should_be_armed(int current_hour, int current_minute);
void schedule_arm_requested(void);
void schedule_tick(void);
int schedule_exit_delay_remaining(void);

#ifdef __cplusplus
}
#endif

#endif
