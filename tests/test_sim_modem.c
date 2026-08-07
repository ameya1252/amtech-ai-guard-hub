#include "sim_modem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_RESPONSE_SIZE 256
#define TEST_PATH_SIZE 64

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

static int check_device_path(const char *label, const char *expected)
{
    char device_path[TEST_PATH_SIZE];

    if (sim_modem_get_device_path(device_path, sizeof(device_path)) != 0)
    {
        printf("%s failed to resolve modem device path: FAIL\n", label);
        return 1;
    }

    if (strcmp(device_path, expected) != 0)
    {
        printf("%s got %s, expected %s: FAIL\n", label, device_path, expected);
        return 1;
    }

    printf("%s got %s: PASS\n", label, device_path);
    return 0;
}

int main(void)
{
    int failures = 0;
    char ip_address[64];
    const char *custom_config_path = "/tmp/amtech_modem_device_config_for_test.txt";
    FILE *fp;

    unsetenv("AMTECH_CONFIG_PATH");

    failures += check_device_path("default modem device", "/dev/ttyS5");

    fp = fopen(custom_config_path, "w");
    if (fp == NULL)
    {
        printf("could not write custom modem config: FAIL\n");
        return 1;
    }
    fprintf(fp, "MODEM_DEVICE=/dev/ttyS1\n");
    fclose(fp);

    setenv("AMTECH_CONFIG_PATH", custom_config_path, 1);
    failures += check_device_path("configured modem device", "/dev/ttyS1");

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
        unsetenv("AMTECH_CONFIG_PATH");
        remove(custom_config_path);
        printf("PASS: SIM modem AT command simulation behaved as expected\n");
        return 0;
    }

    unsetenv("AMTECH_CONFIG_PATH");
    remove(custom_config_path);
    printf("FAIL: SIM modem AT command simulation had %d failure(s)\n", failures);
    return 1;
}
