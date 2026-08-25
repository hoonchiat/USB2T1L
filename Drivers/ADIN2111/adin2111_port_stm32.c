/**
 * @file    adin2111_port_stm32.c
 * @brief   STM32 HAL implementation of the ADIN2111 driver HW callbacks.
 *
 * SPI is used in blocking mode inside a FreeRTOS task; the task blocks (not the
 * whole system) for the duration of a transfer. At 21 MHz a full 1518-byte
 * frame takes ~0.6 ms which is acceptable. Swap to HAL_SPI_TransmitReceive_DMA
 * + task-notify if you need to overlap SPI with CPU work.
 */
#include "adin2111_port_stm32.h"
#include "app_config.h"

#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"

/* Defined in bsp.c */
extern SPI_HandleTypeDef hspi_adin;

#define ADIN_SPI_TIMEOUT_MS   100u

static int port_spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    (void)ctx;
    HAL_StatusTypeDef st;

    if (tx != NULL && rx != NULL) {
        st = HAL_SPI_TransmitReceive(&hspi_adin, (uint8_t *)tx, rx, len,
                                     ADIN_SPI_TIMEOUT_MS);
    } else if (tx != NULL) {
        st = HAL_SPI_Transmit(&hspi_adin, (uint8_t *)tx, len,
                              ADIN_SPI_TIMEOUT_MS);
    } else if (rx != NULL) {
        st = HAL_SPI_Receive(&hspi_adin, rx, len, ADIN_SPI_TIMEOUT_MS);
    } else {
        return -1;
    }
    return (st == HAL_OK) ? 0 : -1;
}

static void port_cs_assert(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(ADIN_CS_PORT, ADIN_CS_PIN, GPIO_PIN_RESET);
}

static void port_cs_deassert(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(ADIN_CS_PORT, ADIN_CS_PIN, GPIO_PIN_SET);
}

static void port_reset_assert(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(ADIN_RST_PORT, ADIN_RST_PIN, GPIO_PIN_RESET);
}

static void port_reset_deassert(void *ctx)
{
    (void)ctx;
    HAL_GPIO_WritePin(ADIN_RST_PORT, ADIN_RST_PIN, GPIO_PIN_SET);
}

static void port_delay_ms(uint32_t ms)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(ms == 0u ? 1u : ms));
    } else {
        HAL_Delay(ms);
    }
}

void adin2111_port_stm32_get_hal(adin2111_hal_t *hal)
{
    hal->spi_xfer       = port_spi_xfer;
    hal->cs_assert      = port_cs_assert;
    hal->cs_deassert    = port_cs_deassert;
    hal->reset_assert   = port_reset_assert;
    hal->reset_deassert = port_reset_deassert;
    hal->delay_ms       = port_delay_ms;
    hal->ctx            = NULL;
}
