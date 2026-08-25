/**
 * @file    bsp.c
 * @brief   Board support implementation for the STM32F407 ADIN2111 bridge.
 */
#include "bsp.h"
#include "app_config.h"

SPI_HandleTypeDef hspi_adin;

#if (ADIN_SPI_USE_DMA)
DMA_HandleTypeDef hdma_adin_spi_tx;
DMA_HandleTypeDef hdma_adin_spi_rx;
#endif

/* ------------------------------------------------------------------------- */
/* Clock tree: HSE -> PLL -> 168 MHz SYSCLK, 48 MHz for USB OTG FS           */
/* ------------------------------------------------------------------------- */

static void system_clock_config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    osc.OscillatorType       = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState             = RCC_HSE_ON;
    osc.PLL.PLLState         = RCC_PLL_ON;
    osc.PLL.PLLSource        = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLM             = BOARD_PLL_M;
    osc.PLL.PLLN             = BOARD_PLL_N;
    osc.PLL.PLLP             = RCC_PLLP_DIV2;   /* BOARD_PLL_P = 2 */
    osc.PLL.PLLQ             = BOARD_PLL_Q;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        bsp_fatal("OscConfig");
    }

    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;   /* 168 MHz */
    clk.APB1CLKDivider = RCC_HCLK_DIV4;     /* 42 MHz  */
    clk.APB2CLKDivider = RCC_HCLK_DIV2;     /* 84 MHz  */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_5) != HAL_OK) {
        bsp_fatal("ClockConfig");
    }
}

/* ------------------------------------------------------------------------- */
/* GPIO: ADIN CS/RST outputs, INT EXTI input, status LEDs                    */
/* ------------------------------------------------------------------------- */

static void gpio_config(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* Chip select: idle high. */
    HAL_GPIO_WritePin(ADIN_CS_PORT, ADIN_CS_PIN, GPIO_PIN_SET);
    g.Pin   = ADIN_CS_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(ADIN_CS_PORT, &g);

    /* Hardware reset: idle high (deasserted). */
    HAL_GPIO_WritePin(ADIN_RST_PORT, ADIN_RST_PIN, GPIO_PIN_SET);
    g.Pin   = ADIN_RST_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ADIN_RST_PORT, &g);

    /* INT: active-low, open-drain output on the ADIN -> falling-edge EXTI. */
    g.Pin  = ADIN_INT_PIN;
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(ADIN_INT_PORT, &g);

    HAL_NVIC_SetPriority(ADIN_INT_EXTI_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(ADIN_INT_EXTI_IRQn);

    /* Status LEDs. */
    g.Pin   = LED_LINK_PIN | LED_ACT_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &g);
    HAL_GPIO_WritePin(LED_PORT, LED_LINK_PIN | LED_ACT_PIN, GPIO_PIN_RESET);
}

/* ------------------------------------------------------------------------- */
/* SPI1: master, mode 0, 8-bit, software NSS                                 */
/* ------------------------------------------------------------------------- */

static void spi_config(void)
{
    GPIO_InitTypeDef g = {0};

    ADIN_SPI_CLK_ENABLE();

    g.Pin       = ADIN_SCK_PIN;
    g.Mode      = GPIO_MODE_AF_PP;
    g.Pull      = GPIO_NOPULL;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = ADIN_SPI_AF;
    HAL_GPIO_Init(ADIN_SCK_PORT, &g);

    g.Pin = ADIN_MISO_PIN;
    HAL_GPIO_Init(ADIN_MISO_PORT, &g);

    g.Pin = ADIN_MOSI_PIN;
    HAL_GPIO_Init(ADIN_MOSI_PORT, &g);

    hspi_adin.Instance               = ADIN_SPI;
    hspi_adin.Init.Mode              = SPI_MODE_MASTER;
    hspi_adin.Init.Direction         = SPI_DIRECTION_2LINES;
    hspi_adin.Init.DataSize          = SPI_DATASIZE_8BIT;
    hspi_adin.Init.CLKPolarity       = SPI_POLARITY_LOW;   /* CPOL = 0 */
    hspi_adin.Init.CLKPhase          = SPI_PHASE_1EDGE;    /* CPHA = 0 */
    hspi_adin.Init.NSS               = SPI_NSS_SOFT;
    hspi_adin.Init.BaudRatePrescaler = ADIN_SPI_PRESCALER;
    hspi_adin.Init.FirstBit          = SPI_FIRSTBIT_MSB;
    hspi_adin.Init.TIMode            = SPI_TIMODE_DISABLE;
    hspi_adin.Init.CRCCalculation    = SPI_CRCCALCULATION_DISABLE;
    hspi_adin.Init.CRCPolynomial     = 7;
    if (HAL_SPI_Init(&hspi_adin) != HAL_OK) {
        bsp_fatal("SPI_Init");
    }
}

/* ------------------------------------------------------------------------- */
/* SPI DMA: TX/RX streams on DMA2, linked to the SPI handle                   */
/* ------------------------------------------------------------------------- */
#if (ADIN_SPI_USE_DMA)
static void spi_dma_config(void)
{
    ADIN_SPI_DMA_CLK_ENABLE();

    /* Memory <-> SPI data register, byte wide, normal (single-shot) mode. */
    hdma_adin_spi_tx.Instance                 = ADIN_SPI_DMA_TX_INSTANCE;
    hdma_adin_spi_tx.Init.Channel             = ADIN_SPI_DMA_TX_CHANNEL;
    hdma_adin_spi_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    hdma_adin_spi_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adin_spi_tx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adin_spi_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_adin_spi_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_adin_spi_tx.Init.Mode                = DMA_NORMAL;
    hdma_adin_spi_tx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adin_spi_tx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adin_spi_tx) != HAL_OK) {
        bsp_fatal("DMA_TX_Init");
    }
    __HAL_LINKDMA(&hspi_adin, hdmatx, hdma_adin_spi_tx);

    hdma_adin_spi_rx.Instance                 = ADIN_SPI_DMA_RX_INSTANCE;
    hdma_adin_spi_rx.Init.Channel             = ADIN_SPI_DMA_RX_CHANNEL;
    hdma_adin_spi_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adin_spi_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adin_spi_rx.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adin_spi_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    hdma_adin_spi_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    hdma_adin_spi_rx.Init.Mode                = DMA_NORMAL;
    hdma_adin_spi_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    hdma_adin_spi_rx.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&hdma_adin_spi_rx) != HAL_OK) {
        bsp_fatal("DMA_RX_Init");
    }
    __HAL_LINKDMA(&hspi_adin, hdmarx, hdma_adin_spi_rx);

    /* Both callbacks run FreeRTOS ...FromISR; keep priority >= max-syscall. */
    HAL_NVIC_SetPriority(ADIN_SPI_DMA_TX_IRQn, ADIN_SPI_DMA_IRQ_PRIO, 0);
    HAL_NVIC_EnableIRQ(ADIN_SPI_DMA_TX_IRQn);
    HAL_NVIC_SetPriority(ADIN_SPI_DMA_RX_IRQn, ADIN_SPI_DMA_IRQ_PRIO, 0);
    HAL_NVIC_EnableIRQ(ADIN_SPI_DMA_RX_IRQn);
    HAL_NVIC_SetPriority(ADIN_SPI_IRQn, ADIN_SPI_DMA_IRQ_PRIO, 0);
    HAL_NVIC_EnableIRQ(ADIN_SPI_IRQn);
}
#endif /* ADIN_SPI_USE_DMA */

/* ------------------------------------------------------------------------- */
/* Public                                                                     */
/* ------------------------------------------------------------------------- */

void bsp_init(void)
{
    system_clock_config();
    gpio_config();
    spi_config();
#if (ADIN_SPI_USE_DMA)
    spi_dma_config();
#endif
}

void bsp_get_mac_address(uint8_t mac[6])
{
    /* Locally administered, unicast: first octet bit1=1, bit0=0. */
    uint32_t uid0 = *(uint32_t *)0x1FFF7A10U;
    uint32_t uid1 = *(uint32_t *)0x1FFF7A14U;

    mac[0] = BOARD_MAC_0;                 /* 0x02: locally administered      */
    mac[1] = BOARD_MAC_1;
    mac[2] = (uint8_t)(uid0);
    mac[3] = (uint8_t)(uid0 >> 8);
    mac[4] = (uint8_t)(uid1);
    mac[5] = (uint8_t)(uid1 >> 8);
    (void)BOARD_MAC_2; (void)BOARD_MAC_3; (void)BOARD_MAC_4; (void)BOARD_MAC_5;
}

void bsp_led_link(bool up)
{
    HAL_GPIO_WritePin(LED_PORT, LED_LINK_PIN, up ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void bsp_led_activity(void)
{
    HAL_GPIO_TogglePin(LED_PORT, LED_ACT_PIN);
}

void bsp_fatal(const char *msg)
{
    (void)msg;
    __disable_irq();
    HAL_GPIO_WritePin(LED_PORT, LED_LINK_PIN | LED_ACT_PIN, GPIO_PIN_SET);
    for (;;) {
        /* Halt. Attach a debugger and inspect `msg`. */
    }
}
