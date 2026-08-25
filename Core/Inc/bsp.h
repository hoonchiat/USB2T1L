/**
 * @file    bsp.h
 * @brief   Board support: clocks, SPI to the ADIN2111, GPIO, LEDs.
 */
#ifndef BSP_H
#define BSP_H

#include <stdint.h>
#include <stdbool.h>
#include "stm32f4xx_hal.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SPI peripheral handle used by the ADIN2111 driver port. */
extern SPI_HandleTypeDef hspi_adin;

#if (ADIN_SPI_USE_DMA)
/** SPI TX/RX DMA handles (linked to hspi_adin); referenced by the DMA ISRs. */
extern DMA_HandleTypeDef hdma_adin_spi_tx;
extern DMA_HandleTypeDef hdma_adin_spi_rx;
#endif

/** Configure system clock (168 MHz, 48 MHz USB), GPIO, SPI and LEDs. Call
 *  once after HAL_Init(), before creating tasks. */
void bsp_init(void);

/** Fill @p mac with the device MAC (locally administered, derived from the
 *  STM32 unique ID so multiple boards differ). */
void bsp_get_mac_address(uint8_t mac[6]);

/** Drive the link LED. */
void bsp_led_link(bool up);

/** Pulse/toggle the activity LED (called per forwarded frame). */
void bsp_led_activity(void);

/** Unrecoverable error: latch LEDs and spin (also breakpoints under debug). */
void bsp_fatal(const char *msg);

#ifdef __cplusplus
}
#endif

#endif /* BSP_H */
