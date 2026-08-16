#include "runtime_loop.h"

#include <stdio.h>

int main(void)
{
    int rc;

    rc = runtime_test_camera_queue_fifo_drop_oldest();
    printf("camera FIFO queue drops oldest under pressure: got %d, expected 0: %s\n",
           rc,
           rc == 0 ? "PASS" : "FAIL");

    if (rc == 0)
    {
        printf("PASS: camera queue FIFO behavior is correct\n");
        return 0;
    }

    printf("FAIL: camera queue FIFO behavior is incorrect\n");
    return 1;
}
