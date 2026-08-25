/**
 * @file    app_config.h
 * @brief   Board / application configuration for the STM32F407 <-> ADIN2111
 *          <-> USB(CDC-ECM) Ethernet bridge.
 *
 * This is the single place to adapt the firmware to your board. It collects
 * every pin, peripheral instance and tunable constant used by the project so
 * that porting to a different STM32F407 board means editing only this file.
 *
 * Data path:
 *
 *      10BASE-T1L            SPI (generic protocol)        USB 2.0 FS
 *   +-------------+   INT   +-----------------+   Bulk   +-------------+
 *   |  ADIN2111   |<------->|   STM32F407     |<-------->|  Linux host |
 *   | 2x SPE PHY  |  SPI1   | FreeRTOS bridge |  CDC-ECM | (cdc_ether) |
 *   +-------------+         +-----------------+          +-------------+
 *
 * Frames received from the ADIN2111 are forwarded to the USB host and appear
 * on Linux as a standard Ethernet interface (enx.. / usb0). Frames sent by the
 * host are written back into the ADIN2111 transmit FIFO.
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ------------------------------------------------------------------------- */
/* Clock configuration                                                       */
/* ------------------------------------------------------------------------- */
/* External crystal on the board (Hz). 8 MHz is the value on the ST
 * Discovery and most "black" F407 boards. Change to match your HSE. */
#define BOARD_HSE_HZ                 8000000U

/* Target SYSCLK = 168 MHz, USB 48 MHz clock derived from PLL Q.
 * PLL: SYSCLK = HSE / M * N / P ; USB = HSE / M * N / Q            */
#define BOARD_PLL_M                  8U
#define BOARD_PLL_N                  336U
#define BOARD_PLL_P                  2U     /* -> 168 MHz SYSCLK             */
#define BOARD_PLL_Q                  7U     /* -> 48  MHz for USB OTG FS     */

/* ------------------------------------------------------------------------- */
/* ADIN2111 SPI interface                                                    */
/* ------------------------------------------------------------------------- */
#define ADIN_SPI                     SPI1
#define ADIN_SPI_CLK_ENABLE()        __HAL_RCC_SPI1_CLK_ENABLE()
#define ADIN_SPI_CLK_DISABLE()       __HAL_RCC_SPI1_CLK_DISABLE()
#define ADIN_SPI_AF                  GPIO_AF5_SPI1
/* APB2 = 84 MHz. Prescaler 4 -> 21 MHz SCK (ADIN2111 max is 25 MHz).       */
#define ADIN_SPI_PRESCALER           SPI_BAUDRATEPRESCALER_4

/* SCK / MISO / MOSI (alternate function pins) */
#define ADIN_SCK_PORT                GPIOA
#define ADIN_SCK_PIN                 GPIO_PIN_5
#define ADIN_MISO_PORT               GPIOA
#define ADIN_MISO_PIN                GPIO_PIN_6
#define ADIN_MOSI_PORT               GPIOA
#define ADIN_MOSI_PIN                GPIO_PIN_7

/* Chip select is driven as a plain GPIO (software NSS) so we can hold it low
 * for the duration of a full frame FIFO burst. */
#define ADIN_CS_PORT                 GPIOA
#define ADIN_CS_PIN                  GPIO_PIN_4

/* ADIN2111 open-drain, active-low interrupt output -> STM32 EXTI input. */
#define ADIN_INT_PORT                GPIOC
#define ADIN_INT_PIN                 GPIO_PIN_4
#define ADIN_INT_EXTI_IRQn           EXTI4_IRQn
#define ADIN_INT_EXTI_IRQHandler     EXTI4_IRQHandler

/* Active-low hardware reset of the ADIN2111. */
#define ADIN_RST_PORT                GPIOC
#define ADIN_RST_PIN                 GPIO_PIN_5

/* Enable the 1-byte CRC appended to every SPI control/data transaction.
 * Costs a little throughput but catches bit errors on the SPI bus. The
 * generic protocol CRC is a CRC-8 (poly 0x07, MSB-first). Set to 0 to
 * disable (must match how the ADIN2111 is strapped/OA_CFG0). */
#define ADIN_SPI_USE_CRC             1

/* ------------------------------------------------------------------------- */
/* Status LEDs (optional). Set ADIN/USB LED ports to NULL to disable.        */
/* Defaults target the STM32F4-Discovery user LEDs on GPIOD.                 */
/* ------------------------------------------------------------------------- */
#define LED_PORT                     GPIOD
#define LED_LINK_PIN                 GPIO_PIN_12   /* green: PHY link up      */
#define LED_ACT_PIN                  GPIO_PIN_14    /* red:   frame activity   */

/* ------------------------------------------------------------------------- */
/* Network / buffering                                                       */
/* ------------------------------------------------------------------------- */
/* Maximum Ethernet frame we handle (no VLAN jumbo). 1514 payload + slack.  */
#define NET_MTU                      1500U
#define NET_MAX_FRAME_LEN            1536U   /* rounded up, 4-byte aligned    */

/* Number of frame buffers in each direction pool. Trade RAM for burst
 * tolerance. 8 * 1536 = 12 KiB per pool. */
#define NET_RX_POOL_COUNT            8U      /* ADIN -> USB                   */
#define NET_TX_POOL_COUNT            8U      /* USB  -> ADIN                  */

/* MAC address advertised to the USB host for the ADIN-side interface.
 * Locally administered (bit1 of first octet set, bit0 clear). Override at
 * build time with -DBOARD_MAC_x=... or read from the STM32 unique ID (see
 * bsp_get_mac_address()). */
#ifndef BOARD_MAC_0
#define BOARD_MAC_0                  0x02U
#define BOARD_MAC_1                  0x11U
#define BOARD_MAC_2                  0x22U
#define BOARD_MAC_3                  0x33U
#define BOARD_MAC_4                  0x44U
#define BOARD_MAC_5                  0x55U
#endif

/* ------------------------------------------------------------------------- */
/* FreeRTOS task priorities and stack sizes (words)                          */
/* ------------------------------------------------------------------------- */
#define TASK_PRIO_NET_RX             (configMAX_PRIORITIES - 2)  /* ADIN->USB */
#define TASK_PRIO_NET_TX             (configMAX_PRIORITIES - 2)  /* USB->ADIN */
#define TASK_PRIO_LINK               (tskIDLE_PRIORITY + 1)      /* mgmt      */

#define TASK_STACK_NET_RX            512U
#define TASK_STACK_NET_TX            512U
#define TASK_STACK_LINK              384U

#endif /* APP_CONFIG_H */
