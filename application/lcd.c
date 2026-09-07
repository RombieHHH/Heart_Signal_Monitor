#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

void LCDTaskFunc(void *argument)
{
    (void)argument;

    for (;;)
    {
        /* TODO: Add LCD updates here. */
        osDelay(1);
    }
}
