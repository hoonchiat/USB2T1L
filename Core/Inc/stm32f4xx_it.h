/**
 * @file    stm32f4xx_it.h
 * @brief   Interrupt handler prototypes.
 */
#ifndef STM32F4xx_IT_H
#define STM32F4xx_IT_H

#include "app_config.h"   /* for ADIN_INT_EXTI_IRQHandler mapping */

#ifdef __cplusplus
extern "C" {
#endif

void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void DebugMon_Handler(void);

void TIM6_DAC_IRQHandler(void);
void OTG_FS_IRQHandler(void);
void ADIN_INT_EXTI_IRQHandler(void);
#if (ADIN_SPI_USE_DMA)
void ADIN_SPI_DMA_TX_IRQHandler(void);
void ADIN_SPI_DMA_RX_IRQHandler(void);
void ADIN_SPI_IRQHandler(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* STM32F4xx_IT_H */
