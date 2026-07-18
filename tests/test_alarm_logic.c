#include "alarm_logic.h"

int main(void)
{
    alarm_logic_init(42);
    alarm_logic_set_armed(1);

    alarm_logic_handle_detection(0, "person", 0.75f);
    alarm_logic_end_frame();

    alarm_logic_handle_detection(0, "person", 0.80f);
    alarm_logic_end_frame();

    alarm_logic_handle_detection(0, "person", 0.90f);
    alarm_logic_end_frame();

    alarm_logic_set_armed(0);
    alarm_logic_handle_detection(0, "person", 0.95f);
    alarm_logic_end_frame();

    return 0;
}
