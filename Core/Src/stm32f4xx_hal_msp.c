/**
 * @file    stm32f4xx_hal_msp.c
 * @brief   HAL MSP init. Sets the NVIC priority grouping to 4 bits of
 *          pre-emption priority, which is what the FreeRTOS Cortex-M4 port
 *          assumes (configPRIO_BITS = 4, no sub-priority).
 *
 * Peripheral-specific MSP init (SPI pins/clock, USB pins/clock) is done in
 * bsp.c and usbd_conf.c respectively.
 */
#include "main.h"

void HAL_MspInit(void)
{
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);
}
