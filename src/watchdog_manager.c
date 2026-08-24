#include "watchdog_manager.h"

#include <stdio.h>
#include <string.h>

#include <pthread.h>

#ifndef SIMULATE_WATCHDOG
#include <errno.h>
#include <fcntl.h>
#include <linux/watchdog.h>
#include <sys/ioctl.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

#define WATCHDOG_MANAGER_SLEEP_MS 1000U

typedef struct
{
    pthread_mutex_t mutex;
    int enabled;
    int running;
    int stop_requested;
    unsigned int heartbeat_age_ms;
    unsigned int feed_elapsed_ms;
    pthread_t thread;
#ifndef SIMULATE_WATCHDOG
    int fd;
#endif
#ifdef SIMULATE_WATCHDOG
    int open_count;
    int close_count;
    int feed_count;
#endif
} watchdog_manager_state_t;

static watchdog_manager_state_t watchdog_state = {
    PTHREAD_MUTEX_INITIALIZER,
    0,
    0,
    0,
    AMTECH_WATCHDOG_HEARTBEAT_STALE_MS,
    0,
    0,
#ifndef SIMULATE_WATCHDOG
    -1,
#endif
#ifdef SIMULATE_WATCHDOG
    0,
    0,
    0,
#endif
};

#ifndef SIMULATE_WATCHDOG
static int watchdog_open_device(void)
{
    int fd = open("/dev/watchdog0", O_WRONLY | O_CLOEXEC);

    if (fd < 0 && errno == ENOENT)
    {
        fd = open("/dev/watchdog", O_WRONLY | O_CLOEXEC);
    }

    if (fd < 0)
    {
        printf("Watchdog: failed to open /dev/watchdog0 or /dev/watchdog: %s\n",
               strerror(errno));
    }

    return fd;
}

static void watchdog_log_identity_and_timeout(int fd)
{
    struct watchdog_info info;
    int requested_timeout = AMTECH_WATCHDOG_REQUESTED_TIMEOUT_SECONDS;
    int effective_timeout = 0;

    memset(&info, 0, sizeof(info));
    if (ioctl(fd, WDIOC_GETSUPPORT, &info) == 0)
    {
        printf("Watchdog: identity=%s options=0x%x firmware=%u\n",
               info.identity,
               info.options,
               info.firmware_version);
    }
    else
    {
        printf("Watchdog: warning: WDIOC_GETSUPPORT failed: %s\n", strerror(errno));
    }

    if (ioctl(fd, WDIOC_SETTIMEOUT, &requested_timeout) != 0)
    {
        printf("Watchdog: warning: WDIOC_SETTIMEOUT %d failed: %s\n",
               AMTECH_WATCHDOG_REQUESTED_TIMEOUT_SECONDS,
               strerror(errno));
    }

    if (ioctl(fd, WDIOC_GETTIMEOUT, &effective_timeout) == 0)
    {
        printf("Watchdog: effective timeout=%d seconds requested=%d seconds\n",
               effective_timeout,
               AMTECH_WATCHDOG_REQUESTED_TIMEOUT_SECONDS);
    }
    else
    {
        printf("Watchdog: warning: WDIOC_GETTIMEOUT failed: %s\n", strerror(errno));
    }
}
#endif

static void watchdog_close_locked(void)
{
#ifdef SIMULATE_WATCHDOG
    watchdog_state.close_count++;
#else
    if (watchdog_state.fd >= 0)
    {
        /*
         * We intentionally do not write the magic close character here. Production
         * should treat the hardware watchdog as effectively non-stoppable once
         * enabled, matching the v1.26 nowayout-oriented requirement.
         */
        close(watchdog_state.fd);
        watchdog_state.fd = -1;
    }
#endif
}

static int watchdog_feed_locked(void)
{
#ifdef SIMULATE_WATCHDOG
    watchdog_state.feed_count++;
    printf("Watchdog: simulated keepalive feed count=%d\n", watchdog_state.feed_count);
    return 0;
#else
    if (watchdog_state.fd < 0)
    {
        return -1;
    }

    if (ioctl(watchdog_state.fd, WDIOC_KEEPALIVE, 0) != 0)
    {
        printf("Watchdog: WDIOC_KEEPALIVE failed: %s\n", strerror(errno));
        return -1;
    }
    return 0;
#endif
}

static void watchdog_tick_locked(unsigned int elapsed_ms)
{
    if (!watchdog_state.running || watchdog_state.stop_requested)
    {
        return;
    }

    if (watchdog_state.heartbeat_age_ms >= AMTECH_WATCHDOG_HEARTBEAT_STALE_MS ||
        elapsed_ms >= AMTECH_WATCHDOG_HEARTBEAT_STALE_MS - watchdog_state.heartbeat_age_ms)
    {
        watchdog_state.heartbeat_age_ms = AMTECH_WATCHDOG_HEARTBEAT_STALE_MS;
    }
    else
    {
        watchdog_state.heartbeat_age_ms += elapsed_ms;
    }

    if (watchdog_state.feed_elapsed_ms >= AMTECH_WATCHDOG_FEED_INTERVAL_MS ||
        elapsed_ms >= AMTECH_WATCHDOG_FEED_INTERVAL_MS - watchdog_state.feed_elapsed_ms)
    {
        watchdog_state.feed_elapsed_ms = AMTECH_WATCHDOG_FEED_INTERVAL_MS;
    }
    else
    {
        watchdog_state.feed_elapsed_ms += elapsed_ms;
    }

    if (watchdog_state.feed_elapsed_ms < AMTECH_WATCHDOG_FEED_INTERVAL_MS)
    {
        return;
    }

    if (watchdog_state.heartbeat_age_ms >= AMTECH_WATCHDOG_HEARTBEAT_STALE_MS)
    {
        printf("Watchdog: withholding keepalive; runtime heartbeat stale for %u ms\n",
               watchdog_state.heartbeat_age_ms);
        return;
    }

    if (watchdog_feed_locked() == 0)
    {
        watchdog_state.feed_elapsed_ms = 0;
    }
}

#ifndef SIMULATE_WATCHDOG
static void *watchdog_thread_main(void *arg)
{
    (void)arg;

    for (;;)
    {
        int should_stop;

        sleep(WATCHDOG_MANAGER_SLEEP_MS / 1000U);
        pthread_mutex_lock(&watchdog_state.mutex);
        should_stop = watchdog_state.stop_requested;
        if (!should_stop)
        {
            watchdog_tick_locked(WATCHDOG_MANAGER_SLEEP_MS);
        }
        pthread_mutex_unlock(&watchdog_state.mutex);

        if (should_stop)
        {
            break;
        }
    }

    return NULL;
}
#endif

int watchdog_manager_start(int enabled)
{
    if (!enabled)
    {
        printf("Watchdog: disabled by config\n");
        return 0;
    }

    pthread_mutex_lock(&watchdog_state.mutex);
    if (watchdog_state.running)
    {
        pthread_mutex_unlock(&watchdog_state.mutex);
        return 0;
    }

#ifdef SIMULATE_WATCHDOG
    watchdog_state.open_count++;
#else
    watchdog_state.fd = watchdog_open_device();
    if (watchdog_state.fd < 0)
    {
        pthread_mutex_unlock(&watchdog_state.mutex);
        return -1;
    }
    watchdog_log_identity_and_timeout(watchdog_state.fd);
#endif

    watchdog_state.enabled = 1;
    watchdog_state.running = 1;
    watchdog_state.stop_requested = 0;
    watchdog_state.heartbeat_age_ms = 0;
    watchdog_state.feed_elapsed_ms = 0;

#ifndef SIMULATE_WATCHDOG
    if (pthread_create(&watchdog_state.thread, NULL, watchdog_thread_main, NULL) != 0)
    {
        printf("Watchdog: failed to start manager thread\n");
        watchdog_state.running = 0;
        watchdog_state.stop_requested = 1;
        watchdog_close_locked();
        pthread_mutex_unlock(&watchdog_state.mutex);
        return -1;
    }
#endif

    printf("Watchdog: manager started, feed_interval_ms=%u stale_heartbeat_ms=%u\n",
           AMTECH_WATCHDOG_FEED_INTERVAL_MS,
           AMTECH_WATCHDOG_HEARTBEAT_STALE_MS);
    pthread_mutex_unlock(&watchdog_state.mutex);
    return 0;
}

void watchdog_manager_stop(void)
{
#ifndef SIMULATE_WATCHDOG
    pthread_t thread;
    int join_needed = 0;
#endif

    pthread_mutex_lock(&watchdog_state.mutex);
    if (!watchdog_state.running)
    {
        pthread_mutex_unlock(&watchdog_state.mutex);
        return;
    }

    watchdog_state.stop_requested = 1;
#ifndef SIMULATE_WATCHDOG
    thread = watchdog_state.thread;
    join_needed = 1;
#else
    watchdog_state.running = 0;
    watchdog_close_locked();
#endif
    pthread_mutex_unlock(&watchdog_state.mutex);

#ifndef SIMULATE_WATCHDOG
    if (join_needed)
    {
        pthread_join(thread, NULL);
    }

    pthread_mutex_lock(&watchdog_state.mutex);
    watchdog_state.running = 0;
    watchdog_close_locked();
    pthread_mutex_unlock(&watchdog_state.mutex);
#endif
}

void watchdog_manager_note_runtime_heartbeat(void)
{
    pthread_mutex_lock(&watchdog_state.mutex);
    if (watchdog_state.running)
    {
        watchdog_state.heartbeat_age_ms = 0;
    }
    pthread_mutex_unlock(&watchdog_state.mutex);
}

int watchdog_manager_is_running(void)
{
    int running;

    pthread_mutex_lock(&watchdog_state.mutex);
    running = watchdog_state.running;
    pthread_mutex_unlock(&watchdog_state.mutex);
    return running;
}

#ifdef SIMULATE_WATCHDOG
void watchdog_manager_reset_simulated_state(void)
{
    pthread_mutex_lock(&watchdog_state.mutex);
    watchdog_state.enabled = 0;
    watchdog_state.running = 0;
    watchdog_state.stop_requested = 0;
    watchdog_state.heartbeat_age_ms = AMTECH_WATCHDOG_HEARTBEAT_STALE_MS;
    watchdog_state.feed_elapsed_ms = 0;
    watchdog_state.open_count = 0;
    watchdog_state.close_count = 0;
    watchdog_state.feed_count = 0;
    pthread_mutex_unlock(&watchdog_state.mutex);
}

int watchdog_manager_get_simulated_feed_count(void)
{
    int count;

    pthread_mutex_lock(&watchdog_state.mutex);
    count = watchdog_state.feed_count;
    pthread_mutex_unlock(&watchdog_state.mutex);
    return count;
}

int watchdog_manager_get_simulated_open_count(void)
{
    int count;

    pthread_mutex_lock(&watchdog_state.mutex);
    count = watchdog_state.open_count;
    pthread_mutex_unlock(&watchdog_state.mutex);
    return count;
}

int watchdog_manager_get_simulated_close_count(void)
{
    int count;

    pthread_mutex_lock(&watchdog_state.mutex);
    count = watchdog_state.close_count;
    pthread_mutex_unlock(&watchdog_state.mutex);
    return count;
}

void watchdog_manager_test_advance_ms(unsigned int elapsed_ms)
{
    pthread_mutex_lock(&watchdog_state.mutex);
    watchdog_tick_locked(elapsed_ms);
    pthread_mutex_unlock(&watchdog_state.mutex);
}
#endif
