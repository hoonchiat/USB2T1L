/**
 * @file    stm32f4xx_it.c
 * @brief   Cortex-M4 and peripheral interrupt handlers.
 *
 * NOTE: SVC_Handler, PendSV_Handler and SysTick_Handler are intentionally NOT
 * defined here. FreeRTOSConfig.h maps them to the kernel handlers
 * (vPortSVCHandler / xPortPendSVHandler / xPortSysTickHandler). The HAL time
 * base runs on TIM6 (see stm32f4xx_hal_timebase_tim.c) so the two schedulers
 * do not fight over SysTick.
 */
#include "main.h"
#include "stm32f4xx_it.h"
#include "app_config.h"
#include "bsp.h"
#include "net_bridge.h"

/* Defined in stm32f4xx_hal_timebase_tim.c */
extern TIM_HandleTypeDef htim6;
/* Defined in usbd_conf.c */
extern PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* ------------------------------------------------------------------------- */
/* Cortex-M faults                                                           */
/* ------------------------------------------------------------------------- */

void NMI_Handler(void)
{
    for (;;) {
    }
}

void HardFault_Handler(void)
{
    bsp_fatal("HardFault");
}

void MemManage_Handler(void)
{
    bsp_fatal("MemManage");
}

void BusFault_Handler(void)
{
    bsp_fatal("BusFault");
}

void UsageFault_Handler(void)
{
    bsp_fatal("UsageFault");
}

void DebugMon_Handler(void)
{
}

/* ------------------------------------------------------------------------- */
/* Peripheral interrupts                                                     */
/* ------------------------------------------------------------------------- */

/* HAL time base tick. */
void TIM6_DAC_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim6);
}

/* USB OTG FS global interrupt. */
void OTG_FS_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&hpcd_USB_OTG_FS);
}

#if (ADIN_SPI_USE_DMA)
/* ADIN2111 SPI DMA streams + SPI error interrupt. The DMA transfer-complete
 * ISR is what fires HAL_SPI_TxRxCpltCallback -> releases the waiting task. */
void ADIN_SPI_DMA_TX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adin_spi_tx);
}

void ADIN_SPI_DMA_RX_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adin_spi_rx);
}

void ADIN_SPI_IRQHandler(void)
{
    HAL_SPI_IRQHandler(&hspi_adin);
}
#endif /* ADIN_SPI_USE_DMA */

/* ADIN2111 INT line (falling edge). Expands to e.g. EXTI4_IRQHandler. */
void ADIN_INT_EXTI_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(ADIN_INT_PIN);
}

/* EXTI callback dispatched by HAL_GPIO_EXTI_IRQHandler. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == ADIN_INT_PIN) {
        net_bridge_adin_int_isr();
    }
}
