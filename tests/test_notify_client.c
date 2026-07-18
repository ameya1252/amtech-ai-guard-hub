#include "notify_client.h"

#include <stdio.h>

int main(void)
{
    int ret = notify_send_alert("amtech-demo-shop", "test");

    if (ret == 0)
    {
        printf("PASS: notify client simulation behaved as expected\n");
        return 0;
    }

    printf("FAIL: notify client simulation failed\n");
    return 1;
}
