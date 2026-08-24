#ifndef AMTECH_WATCHDOG_MANAGER_H
#define AMTECH_WATCHDOG_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#define AMTECH_WATCHDOG_REQUESTED_TIMEOUT_SECONDS 60
#define AMTECH_WATCHDOG_FEED_INTERVAL_MS 10000U
#define AMTECH_WATCHDOG_HEARTBEAT_STALE_MS 15000U

int watchdog_manager_start(int enabled);
void watchdog_manager_stop(void);
void watchdog_manager_note_runtime_heartbeat(void);
int watchdog_manager_is_running(void);

#ifdef SIMULATE_WATCHDOG
void watchdog_manager_reset_simulated_state(void);
int watchdog_manager_get_simulated_feed_count(void);
int watchdog_manager_get_simulated_open_count(void);
int watchdog_manager_get_simulated_close_count(void);
void watchdog_manager_test_advance_ms(unsigned int elapsed_ms);
#endif

#ifdef __cplusplus
}
#endif

#endif
