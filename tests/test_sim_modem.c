#include "sim_modem.h"

#include <stdio.h>
#include <string.h>

#define TEST_RESPONSE_SIZE 256

static int check_command(const char *command, const char *expected_fragment)
{
    char response[TEST_RESPONSE_SIZE];
    int result;

    result = sim_modem_send_at(command, response, sizeof(response), 1000);
    if (result != 0)
    {
        printf("%s send failed: FAIL\n", command);
        return 1;
    }

    if (strstr(response, expected_fragment) == NULL)
    {
        printf("%s response missing %s: %s: FAIL\n", command, expected_fragment, response);
        return 1;
    }

    printf("%s response contains %s: PASS\n", command, expected_fragment);
    printf("%s response: %s\n", command, response);
    return 0;
}

int main(void)
{
    int failures = 0;
    char ip_address[64];

    failures += check_command("AT", "OK");
    failures += check_command("AT+CSQ", "+CSQ:");
    failures += check_command("AT+CREG?", "+CREG:");

    if (sim_modem_connect_data("airtelgprs.com") != 0)
    {
        printf("sim_modem_connect_data failed: FAIL\n");
        failures++;
    }
    else
    {
        printf("sim_modem_connect_data succeeded: PASS\n");
    }

    if (sim_modem_get_ip(ip_address, sizeof(ip_address)) != 0)
    {
        printf("sim_modem_get_ip failed: FAIL\n");
        failures++;
    }
    else if (strcmp(ip_address, "10.83.214.110") != 0)
    {
        printf("sim_modem_get_ip returned %s, expected 10.83.214.110: FAIL\n", ip_address);
        failures++;
    }
    else
    {
        printf("sim_modem_get_ip returned %s: PASS\n", ip_address);
    }

    if (failures == 0)
    {
        printf("PASS: SIM modem AT command simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: SIM modem AT command simulation had %d failure(s)\n", failures);
    return 1;
}
