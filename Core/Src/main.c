/**
 * @file    main.c
 * @brief   STM32F407 ADIN2111 <-> USB(CDC-ECM) Ethernet bridge, FreeRTOS.
 *
 * Boot flow:
 *   HAL_Init()          - reset peripherals, TIM6 HAL timebase
 *   bsp_init()          - 168 MHz clocks, SPI1, GPIO, LEDs
 *   net_bridge_init()   - ADIN2111 bring-up, USB device, bridge tasks
 *   vTaskStartScheduler - hand control to FreeRTOS
 */
#include "main.h"
#include "bsp.h"
#include "net_bridge.h"

#include "FreeRTOS.h"
#include "task.h"

int main(void)
{
    HAL_Init();
    bsp_init();

    if (!net_bridge_init()) {
        bsp_fatal("net_bridge_init");
    }

    vTaskStartScheduler();

    /* Only reached if the kernel could not start (out of heap). */
    bsp_fatal("scheduler");
    for (;;) {
    }
}

void Error_Handler(void)
{
    bsp_fatal("Error_Handler");
}

/* ------------------------------------------------------------------------- */
/* FreeRTOS hooks                                                            */
/* ------------------------------------------------------------------------- */

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    bsp_fatal("stack overflow");
}

void vApplicationMallocFailedHook(void)
{
    bsp_fatal("malloc failed");
}

#if (configCHECK_FOR_STACK_OVERFLOW == 0)
/* nothing */
#endif

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    bsp_fatal("assert_failed");
}
#endif
