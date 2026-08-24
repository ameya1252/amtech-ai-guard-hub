#include "watchdog_manager.h"

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
    watchdog_manager_reset_simulated_state();

    check_int("disabled watchdog start succeeds", watchdog_manager_start(0), 0);
    check_int("disabled watchdog does not run", watchdog_manager_is_running(), 0);
    check_int("disabled watchdog does not open", watchdog_manager_get_simulated_open_count(), 0);

    check_int("enabled watchdog start succeeds", watchdog_manager_start(1), 0);
    check_int("enabled watchdog is running", watchdog_manager_is_running(), 1);
    check_int("enabled watchdog opens once", watchdog_manager_get_simulated_open_count(), 1);

    watchdog_manager_note_runtime_heartbeat();
    watchdog_manager_test_advance_ms(AMTECH_WATCHDOG_FEED_INTERVAL_MS);
    check_int("fresh heartbeat feeds watchdog", watchdog_manager_get_simulated_feed_count(), 1);

    watchdog_manager_test_advance_ms(AMTECH_WATCHDOG_HEARTBEAT_STALE_MS);
    watchdog_manager_test_advance_ms(AMTECH_WATCHDOG_FEED_INTERVAL_MS);
    check_int("stale heartbeat withholds watchdog feed", watchdog_manager_get_simulated_feed_count(), 1);

    watchdog_manager_note_runtime_heartbeat();
    watchdog_manager_test_advance_ms(AMTECH_WATCHDOG_FEED_INTERVAL_MS);
    check_int("fresh heartbeat resumes watchdog feed", watchdog_manager_get_simulated_feed_count(), 2);

    watchdog_manager_stop();
    check_int("watchdog stopped", watchdog_manager_is_running(), 0);
    check_int("watchdog close counted", watchdog_manager_get_simulated_close_count(), 1);

    if (failures == 0)
    {
        printf("PASS: watchdog manager simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: watchdog manager simulation had %d failure(s)\n", failures);
    return 1;
}
