#include "modem_state.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check_operator(const char *label, const char *response, const char *expected)
{
    const char *actual = modem_state_operator_name_for_cops_response(response);
    int matches = actual != NULL && strcmp(actual, expected) == 0;

    printf("%s: got %s, expected %s: %s\n",
           label,
           actual != NULL ? actual : "(null)",
           expected,
           matches ? "PASS" : "FAIL");

    if (!matches)
    {
        failures++;
    }
}

static void check_no_operator(const char *label, const char *response)
{
    const char *actual = modem_state_operator_name_for_cops_response(response);
    int matches = actual == NULL;

    printf("%s: got %s, expected (null): %s\n",
           label,
           actual != NULL ? actual : "(null)",
           matches ? "PASS" : "FAIL");

    if (!matches)
    {
        failures++;
    }
}

int main(void)
{
    check_operator("real hardware numeric COPS response maps 405864 to Jio",
                   "\r\n+COPS: 0,2,\"405864\",7\r\n\r\nOK\r\n",
                   "Jio");
    check_operator("numeric Jio lower range maps to Jio",
                   "\r\n+COPS: 0,2,\"405854\",7\r\n\r\nOK\r\n",
                   "Jio");
    check_operator("numeric Jio upper range maps to Jio",
                   "\r\n+COPS: 0,2,\"405874\",7\r\n\r\nOK\r\n",
                   "Jio");
    check_operator("legacy name-format COPS response still maps Jio",
                   "\r\n+COPS: 0,0,\"Jio\",7\r\n\r\nOK\r\n",
                   "Jio");
    check_no_operator("unknown numeric COPS response does not false-match",
                      "\r\n+COPS: 0,2,\"999999\",7\r\n\r\nOK\r\n");

    if (failures == 0)
    {
        printf("PASS: COPS parser behaved as expected\n");
        return 0;
    }

    printf("FAIL: COPS parser had %d failure(s)\n", failures);
    return 1;
}
