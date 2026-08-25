/**
 * @file    adin2111_port_stm32.h
 * @brief   STM32 HAL glue that fills an adin2111_hal_t for the driver core.
 */
#ifndef ADIN2111_PORT_STM32_H
#define ADIN2111_PORT_STM32_H

#include "adin2111.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Populate @p hal with STM32 SPI/GPIO callbacks. The SPI peripheral and the
 * CS/INT/RST GPIOs must already be initialised (see bsp_init()).
 */
void adin2111_port_stm32_get_hal(adin2111_hal_t *hal);

#ifdef __cplusplus
}
#endif

#endif /* ADIN2111_PORT_STM32_H */
