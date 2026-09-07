#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

void LEDTaskFunc(void *argument)
{
    (void)argument;

    for (;;)
    {
        /* TODO: Add LED control here. */
        osDelay(1);
    }
}
