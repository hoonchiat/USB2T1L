/**
 * @file    stm32f4xx_hal_timebase_tim.c
 * @brief   HAL time base using TIM6 (1 kHz), so SysTick is free for FreeRTOS.
 */
#include "stm32f4xx_hal.h"

TIM_HandleTypeDef htim6;

HAL_StatusTypeDef HAL_InitTick(uint32_t TickPriority)
{
    RCC_ClkInitTypeDef clkconfig;
    uint32_t uwTimclock;
    uint32_t uwAPB1Prescaler;
    uint32_t uwPrescalerValue;
    uint32_t pFLatency;
    HAL_StatusTypeDef status;

    __HAL_RCC_TIM6_CLK_ENABLE();

    HAL_RCC_GetClockConfig(&clkconfig, &pFLatency);
    uwAPB1Prescaler = clkconfig.APB1CLKDivider;
    uwTimclock = HAL_RCC_GetPCLK1Freq();
    if (uwAPB1Prescaler != RCC_HCLK_DIV1) {
        uwTimclock *= 2U;
    }
    /* Target 1 MHz TIM6 counter, 1 ms update period. */
    uwPrescalerValue = (uint32_t)((uwTimclock / 1000000U) - 1U);

    htim6.Instance           = TIM6;
    htim6.Init.Period        = (1000000U / 1000U) - 1U;
    htim6.Init.Prescaler     = uwPrescalerValue;
    htim6.Init.ClockDivision = 0;
    htim6.Init.CounterMode   = TIM_COUNTERMODE_UP;

    status = HAL_TIM_Base_Init(&htim6);
    if (status == HAL_OK) {
        status = HAL_TIM_Base_Start_IT(&htim6);
        if (status == HAL_OK) {
            if (TickPriority < (1UL << __NVIC_PRIO_BITS)) {
                HAL_NVIC_SetPriority(TIM6_DAC_IRQn, TickPriority, 0);
                uwTickPrio = TickPriority;
            } else {
                status = HAL_ERROR;
            }
            HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
        }
    }
    return status;
}

void HAL_SuspendTick(void)
{
    __HAL_TIM_DISABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_ResumeTick(void)
{
    __HAL_TIM_ENABLE_IT(&htim6, TIM_IT_UPDATE);
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) {
        HAL_IncTick();
    }
}
