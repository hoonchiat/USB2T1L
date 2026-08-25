/**
 * @file    adin2111_port_stm32.c
 * @brief   STM32 HAL implementation of the ADIN2111 driver HW callbacks.
 *
 * SPI transport has two modes (selected by ADIN_SPI_USE_DMA):
 *
 *  - DMA (default): once the FreeRTOS scheduler is running, each transfer is
 *    handed to the SPI TX/RX DMA streams and the calling task blocks on a
 *    binary semaphore until the DMA transfer-complete ISR releases it. The CPU
 *    is free for other tasks during the (up to ~0.6 ms) frame burst instead of
 *    busy-polling the SPI data register.
 *
 *  - Blocking: used as a fallback and, always, before the scheduler starts
 *    (device bring-up in adin2111_init), where a semaphore cannot block.
 *
 * A single completion semaphore is sufficient because all ADIN access is
 * serialised by net_bridge's s_adin_lock mutex: only one SPI transfer is ever
 * in flight. To keep the transmit path free of SPI overrun handling, DMA
 * transfers always run full-duplex (TransmitReceive); write transactions read
 * the miso bytes into a scratch sink that is then discarded.
 */
#include "adin2111_port_stm32.h"
#include "app_config.h"

#include "stm32f4xx_hal.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* Defined in bsp.c */
extern SPI_HandleTypeDef hspi_adin;

#define ADIN_SPI_TIMEOUT_MS   100u

#if (ADIN_SPI_USE_DMA)
/* Largest single SPI transfer = command header + a full frame body, rounded
 * up. Sized against NET_MAX_FRAME_LEN so a max frame always fits. */
#define ADIN_SPI_XFER_MAX     (NET_MAX_FRAME_LEN + 16u)

static SemaphoreHandle_t s_spi_done;              /* given by DMA cplt ISR    */
static volatile int      s_spi_error;             /* set by SPI error ISR     */
static uint8_t           s_rx_sink[ADIN_SPI_XFER_MAX]; /* discard MISO on write */

static int port_spi_xfer_dma(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    uint8_t *rxbuf = rx;
    if (rxbuf == NULL) {
        if (len > sizeof(s_rx_sink)) {
            return -1;
        }
        rxbuf = s_rx_sink;   /* write: receive into the sink and ignore it   */
    }

    s_spi_error = 0;
    (void)xSemaphoreTake(s_spi_done, 0);   /* drop any stale signal          */

    if (HAL_SPI_TransmitReceive_DMA(&hspi_adin, (uint8_t *)tx, rxbuf, len)
            != HAL_OK) {
        return -1;
    }
    if (xSemaphoreTake(s_spi_done, pdMS_TO_TICKS(ADIN_SPI_TIMEOUT_MS))
            != pdTRUE) {
        /* Abort (not just DMAStop) so the SPI state returns to READY and the
         * next transfer is not rejected as BUSY. */
        (void)HAL_SPI_Abort(&hspi_adin);
        return -1;
    }
    if (s_spi_error) {
        return -1;
    }
    /* RX-DMA completion means all bytes are in, but make sure the shift
     * register is idle before the driver releases CS. */
    uint32_t guard = 100000u;
    while (__HAL_SPI_GET_FLAG(&hspi_adin, SPI_FLAG_BSY) && (guard-- != 0u)) {
    }
    return 0;
}
#endif /* ADIN_SPI_USE_DMA */

static int port_spi_xfer_blocking(const uint8_t *tx, uint8_t *rx, uint16_t len)
{
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

static int port_spi_xfer(void *ctx, const uint8_t *tx, uint8_t *rx, uint16_t len)
{
    (void)ctx;
#if (ADIN_SPI_USE_DMA)
    /* DMA needs the scheduler for the blocking wait (init runs before it), and
     * only pays off for frame-sized bursts (small register access stays polled). */
    if (len >= ADIN_SPI_DMA_MIN_LEN &&
        xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return port_spi_xfer_dma(tx, rx, len);
    }
#endif
    return port_spi_xfer_blocking(tx, rx, len);
}

/* --- SPI DMA completion / error callbacks (ISR context) ------------------ */
#if (ADIN_SPI_USE_DMA)
void HAL_SPI_TxRxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == ADIN_SPI) {
        BaseType_t woken = pdFALSE;
        (void)xSemaphoreGiveFromISR(s_spi_done, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi->Instance == ADIN_SPI) {
        s_spi_error = 1;
        BaseType_t woken = pdFALSE;
        (void)xSemaphoreGiveFromISR(s_spi_done, &woken);
        portYIELD_FROM_ISR(woken);
    }
}
#endif /* ADIN_SPI_USE_DMA */

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
#if (ADIN_SPI_USE_DMA)
    /* Created here (task context, before the scheduler starts) because the SPI
     * class Init/ISR path must never allocate kernel objects. */
    if (s_spi_done == NULL) {
        s_spi_done = xSemaphoreCreateBinary();
    }
#endif
    hal->spi_xfer       = port_spi_xfer;
    hal->cs_assert      = port_cs_assert;
    hal->cs_deassert    = port_cs_deassert;
    hal->reset_assert   = port_reset_assert;
    hal->reset_deassert = port_reset_deassert;
    hal->delay_ms       = port_delay_ms;
    hal->ctx            = NULL;
}
