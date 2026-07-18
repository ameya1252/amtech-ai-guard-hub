#include "notify_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(SIMULATE_NETWORK)
#include <curl/curl.h>
#endif

#define DEFAULT_BACKEND_URL "http://127.0.0.1:8000/alert"

static const char *backend_url(void)
{
    const char *url = getenv("AMTECH_BACKEND_ALERT_URL");
    return (url != NULL && url[0] != '\0') ? url : DEFAULT_BACKEND_URL;
}

static void current_timestamp(char *buffer, size_t buffer_size)
{
    time_t now = time(NULL);
    struct tm tm_now;

#if defined(_WIN32)
    gmtime_s(&tm_now, &now);
#else
    gmtime_r(&now, &tm_now);
#endif

    strftime(buffer, buffer_size, "%Y-%m-%dT%H:%M:%SZ", &tm_now);
}

static int build_alert_json(char *buffer, size_t buffer_size, const char *shop_id, const char *event_type)
{
    char timestamp[32];

    current_timestamp(timestamp, sizeof(timestamp));

    return snprintf(buffer, buffer_size,
                    "{\"shop_id\":\"%s\",\"event_type\":\"%s\",\"timestamp\":\"%s\"}",
                    shop_id, event_type, timestamp);
}

int notify_send_alert(const char *shop_id, const char *event_type)
{
    char payload[512];
    int written;

    if (shop_id == NULL || event_type == NULL)
    {
        printf("Notify: shop_id and event_type are required\n");
        return -1;
    }

    written = build_alert_json(payload, sizeof(payload), shop_id, event_type);
    if (written < 0 || (size_t)written >= sizeof(payload))
    {
        printf("Notify: alert JSON payload too large\n");
        return -1;
    }

#if defined(SIMULATE_NETWORK)
    printf("Notify: would POST %s to %s\n", payload, backend_url());
    return 0;
#else
    CURL *curl;
    CURLcode result;
    struct curl_slist *headers = NULL;
    long http_code = 0;

    curl = curl_easy_init();
    if (curl == NULL)
    {
        printf("Notify: curl_easy_init failed\n");
        return -1;
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, backend_url());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

    result = curl_easy_perform(curl);
    if (result != CURLE_OK)
    {
        printf("Notify: HTTP POST failed: %s\n", curl_easy_strerror(result));
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return -1;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (http_code < 200 || http_code >= 300)
    {
        printf("Notify: backend returned HTTP %ld\n", http_code);
        return -1;
    }

    printf("Notify: alert sent, backend returned HTTP %ld\n", http_code);
    return 0;
#endif
}
