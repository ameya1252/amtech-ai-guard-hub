#include "modem_state.h"

#include "sim_modem.h"

#include <stdio.h>
#include <string.h>

#define MODEM_STATE_RESPONSE_SIZE 512
#define MODEM_STATE_IP_SIZE 64
#define MODEM_STATE_AT_TIMEOUT_MS 1000
#define MODEM_STATE_MAX_RETRIES 5

typedef struct {
    const char *name;
    const char *match_fragment;
    const char *apn;
    const char *pdp_type;
    int configure_jio_ims_context;
} operator_profile_t;

static const operator_profile_t operator_profiles[] = {
    {"Jio", "Jio", "jionet", "IPV4V6", 1},
    {"Airtel", "Airtel", "airtelgprs.com", "IPV4V6", 0},
    {"Vi", "Vi", "www", "IPV4V6", 0},
    {"BSNL", "BSNL", "bsnlnet", "IPV4V6", 0},
};

static modem_state_t current_state = MODEM_STATE_POWER_OFF;
static int retry_count = 0;
static const operator_profile_t *selected_operator = NULL;
static char assigned_ip[MODEM_STATE_IP_SIZE];

static int response_has_ok(const char *response)
{
    return response != NULL && strstr(response, "OK") != NULL && strstr(response, "ERROR") == NULL;
}

static void transition_to(modem_state_t next_state)
{
    printf("Modem state: %s -> %s\n", modem_state_name(current_state), modem_state_name(next_state));
    current_state = next_state;
    retry_count = 0;
}

static int fail_or_retry(const char *operation)
{
    retry_count++;
    printf("Modem state: %s failed in %s, retry %d/%d\n",
           operation,
           modem_state_name(current_state),
           retry_count,
           MODEM_STATE_MAX_RETRIES);

    if (retry_count >= MODEM_STATE_MAX_RETRIES)
    {
        transition_to(MODEM_STATE_FAILED);
        return -1;
    }

    return 0;
}

static int send_command_contains(const char *command, const char *expected)
{
    char response[MODEM_STATE_RESPONSE_SIZE];

    if (sim_modem_send_at(command, response, sizeof(response), MODEM_STATE_AT_TIMEOUT_MS) != 0)
    {
        return -1;
    }

    if (!response_has_ok(response))
    {
        return -1;
    }

    if (expected != NULL && strstr(response, expected) == NULL)
    {
        return -1;
    }

    return 0;
}

static int cereg_response_is_registered(const char *response)
{
    const char *cereg;
    const char *comma;
    int stat = -1;

    if (response == NULL)
    {
        return 0;
    }

    cereg = strstr(response, "+CEREG:");
    if (cereg == NULL)
    {
        return 0;
    }

    comma = strchr(cereg, ',');
    if (comma == NULL)
    {
        return 0;
    }

    if (sscanf(comma + 1, "%d", &stat) != 1)
    {
        return 0;
    }

    return stat == 1 || stat == 5;
}

static int network_is_registered(void)
{
    char response[MODEM_STATE_RESPONSE_SIZE];

    if (sim_modem_send_at("AT+CEREG?", response, sizeof(response), MODEM_STATE_AT_TIMEOUT_MS) != 0)
    {
        return -1;
    }

    if (!response_has_ok(response))
    {
        return -1;
    }

    return cereg_response_is_registered(response) ? 1 : 0;
}

static const operator_profile_t *detect_operator(void)
{
    char response[MODEM_STATE_RESPONSE_SIZE];
    size_t index;

    if (sim_modem_send_at("AT+COPS?", response, sizeof(response), MODEM_STATE_AT_TIMEOUT_MS) != 0)
    {
        return NULL;
    }

    if (!response_has_ok(response))
    {
        return NULL;
    }

    for (index = 0; index < sizeof(operator_profiles) / sizeof(operator_profiles[0]); index++)
    {
        if (strstr(response, operator_profiles[index].match_fragment) != NULL)
        {
            return &operator_profiles[index];
        }
    }

    return NULL;
}

void modem_state_init(void)
{
    current_state = MODEM_STATE_POWER_OFF;
    retry_count = 0;
    selected_operator = NULL;
    assigned_ip[0] = '\0';
}

modem_state_t modem_state_get_current(void)
{
    return current_state;
}

const char *modem_state_name(modem_state_t state)
{
    switch (state)
    {
    case MODEM_STATE_POWER_OFF:
        return "POWER_OFF";
    case MODEM_STATE_BOOTING:
        return "BOOTING";
    case MODEM_STATE_MODEM_READY:
        return "MODEM_READY";
    case MODEM_STATE_SIM_READY:
        return "SIM_READY";
    case MODEM_STATE_REGISTERING:
        return "REGISTERING";
    case MODEM_STATE_REGISTERED:
        return "REGISTERED";
    case MODEM_STATE_PDP_ACTIVE:
        return "PDP_ACTIVE";
    case MODEM_STATE_CONNECTED:
        return "CONNECTED";
    case MODEM_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

int modem_state_tick(void)
{
    int registration_status;

    switch (current_state)
    {
    case MODEM_STATE_POWER_OFF:
        transition_to(MODEM_STATE_BOOTING);
        return 0;

    case MODEM_STATE_BOOTING:
        if (send_command_contains("AT", NULL) == 0)
        {
            transition_to(MODEM_STATE_MODEM_READY);
            return 0;
        }
        return fail_or_retry("AT");

    case MODEM_STATE_MODEM_READY:
        if (send_command_contains("AT+CPIN?", "READY") == 0)
        {
            transition_to(MODEM_STATE_SIM_READY);
            return 0;
        }
        return fail_or_retry("AT+CPIN?");

    case MODEM_STATE_SIM_READY:
        transition_to(MODEM_STATE_REGISTERING);
        return 0;

    case MODEM_STATE_REGISTERING:
        registration_status = network_is_registered();
        if (registration_status == 1)
        {
            transition_to(MODEM_STATE_REGISTERED);
            return 0;
        }
        if (registration_status == 0)
        {
            return fail_or_retry("AT+CEREG? not registered yet");
        }
        return fail_or_retry("AT+CEREG?");

    case MODEM_STATE_REGISTERED:
        selected_operator = detect_operator();
        if (selected_operator == NULL)
        {
            return fail_or_retry("AT+COPS?");
        }

        printf("Modem state: detected operator %s, APN=%s, PDP=%s\n",
               selected_operator->name,
               selected_operator->apn,
               selected_operator->pdp_type);

        if (selected_operator->configure_jio_ims_context)
        {
            /*
             * Jio requires a separate IMS PDP profile in addition to the data
             * profile. Voice/SMS usage is still deferred until hardware
             * validation, but the context is configured here so the modem has
             * the expected carrier profile.
             */
            if (sim_modem_configure_pdp_context(8, selected_operator->pdp_type, "IMS") != 0)
            {
                return fail_or_retry("Jio IMS PDP context configuration");
            }
        }

        if (sim_modem_connect_data_profile(selected_operator->apn, selected_operator->pdp_type) == 0)
        {
            transition_to(MODEM_STATE_PDP_ACTIVE);
            return 0;
        }
        return fail_or_retry("PDP activation");

    case MODEM_STATE_PDP_ACTIVE:
        if (sim_modem_get_ip(assigned_ip, sizeof(assigned_ip)) == 0)
        {
            printf("Modem state: connected with IP %s\n", assigned_ip);
            transition_to(MODEM_STATE_CONNECTED);
            return 0;
        }
        return fail_or_retry("IP address confirmation");

    case MODEM_STATE_CONNECTED:
        return 0;

    case MODEM_STATE_FAILED:
        return -1;

    default:
        transition_to(MODEM_STATE_FAILED);
        return -1;
    }
}

int modem_state_is_connected(void)
{
    return current_state == MODEM_STATE_CONNECTED;
}
