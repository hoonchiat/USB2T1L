/**
 * @file    adin2111.h
 * @brief   Portable driver for the ADIN2111 (10BASE-T1L MAC-PHY) over SPI.
 *
 * The driver core (adin2111.c) is platform independent: all hardware access
 * goes through the callbacks in @ref adin2111_hal_t. The STM32 implementation
 * of those callbacks lives in adin2111_port_stm32.c.
 *
 * Typical use from the network bridge:
 *
 *     adin2111_t dev;
 *     adin2111_init(&dev, &hal);              // reset + configure + verify id
 *     adin2111_set_host_mac(&dev, mac);       // frames for us -> host FIFO
 *     ...
 *     // in the ISR-woken RX task:
 *     while (adin2111_rx_ready(&dev)) {
 *         adin2111_read_frame(&dev, buf, sizeof buf, &len, &port);
 *         // forward buf/len to USB
 *     }
 *     // in the USB-fed TX task:
 *     adin2111_write_frame(&dev, ADIN2111_PORT_1, buf, len);
 */
#ifndef ADIN2111_H
#define ADIN2111_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Return codes */
typedef enum {
    ADIN2111_OK          = 0,
    ADIN2111_ERR_PARAM   = -1,
    ADIN2111_ERR_SPI     = -2,
    ADIN2111_ERR_ID      = -3,   /* unexpected PHY id                        */
    ADIN2111_ERR_CRC     = -4,   /* SPI CRC mismatch                         */
    ADIN2111_ERR_NODATA  = -5,   /* no RX frame available                    */
    ADIN2111_ERR_TOOBIG  = -6,   /* frame larger than caller buffer          */
    ADIN2111_ERR_NOSPACE = -7,   /* not enough TX FIFO space                 */
    ADIN2111_ERR_TIMEOUT = -8,
} adin2111_status_t;

/* Switch ports. The ADIN2111 has two 10BASE-T1L ports; the ADIN1110 one. */
typedef enum {
    ADIN2111_PORT_1 = 0,
    ADIN2111_PORT_2 = 1,
} adin2111_port_t;

/**
 * Platform hardware access. Provided by the caller (see adin2111_port_stm32.c).
 * All SPI transfers are full duplex and happen with CS already asserted by the
 * driver via @ref cs_assert / @ref cs_deassert.
 */
typedef struct {
    /** Full-duplex SPI byte transfer. @p tx or @p rx may be NULL (send zeros
     *  / discard). Returns 0 on success, negative on error. */
    int  (*spi_xfer)(void *ctx, const uint8_t *tx, uint8_t *rx, uint16_t len);
    void (*cs_assert)(void *ctx);     /**< drive chip-select active (low)     */
    void (*cs_deassert)(void *ctx);   /**< release chip-select (high)         */
    void (*reset_assert)(void *ctx);  /**< drive hardware RST active (low)    */
    void (*reset_deassert)(void *ctx);/**< release hardware RST (high)        */
    void (*delay_ms)(uint32_t ms);    /**< blocking millisecond delay         */
    void  *ctx;                       /**< opaque, passed back to callbacks   */
} adin2111_hal_t;

/** Driver instance. Treat as opaque; fields are for the driver's use. */
typedef struct {
    adin2111_hal_t hal;
    bool     crc_enabled;   /* SPI generic-protocol CRC-8 on each transaction */
    uint32_t phy_id;        /* value read from PHYID register                 */
    uint8_t  num_ports;     /* 1 (ADIN1110) or 2 (ADIN2111)                   */
} adin2111_t;

/**
 * Reset the device, verify its identity and apply the default bridge
 * configuration: HW FCS append/check, forward unknown frames to host, both
 * ports enabled, RX_RDY / SPI_ERR interrupts unmasked.
 *
 * @param crc_enabled  Must match how the part is strapped for SPI CRC.
 */
adin2111_status_t adin2111_init(adin2111_t *dev,
                                const adin2111_hal_t *hal,
                                bool crc_enabled);

/** Raw 32-bit register access (big-endian on the wire, host order here). */
adin2111_status_t adin2111_read_reg(adin2111_t *dev, uint16_t reg, uint32_t *val);
adin2111_status_t adin2111_write_reg(adin2111_t *dev, uint16_t reg, uint32_t val);

/**
 * Program the device unicast (host) MAC address into the filter table so that
 * frames destined for us are forwarded to the SPI host FIFO. Broadcast and
 * multicast forwarding are enabled by adin2111_init().
 */
adin2111_status_t adin2111_set_host_mac(adin2111_t *dev, const uint8_t mac[6]);

/** @return true if at least one RX frame is waiting on any port. */
bool adin2111_rx_ready(adin2111_t *dev);

/** Read the raw STATUS1 register (link, RX_RDY, errors). */
adin2111_status_t adin2111_read_status(adin2111_t *dev, uint32_t *status1);

/** @return true if the given port reports link up. */
bool adin2111_link_up(adin2111_t *dev, adin2111_port_t port);

/**
 * Read one Ethernet frame (without FCS) from the RX FIFO.
 *
 * @param buf      destination for the Ethernet frame (dst/src/type/payload)
 * @param buf_len  size of @p buf
 * @param out_len  [out] number of bytes written to @p buf
 * @param out_port [out] ingress port the frame arrived on (may be NULL)
 * @return ADIN2111_OK, ADIN2111_ERR_NODATA, ADIN2111_ERR_TOOBIG or SPI/CRC err
 */
adin2111_status_t adin2111_read_frame(adin2111_t *dev,
                                      uint8_t *buf, uint16_t buf_len,
                                      uint16_t *out_len,
                                      adin2111_port_t *out_port);

/**
 * Write one Ethernet frame (without FCS; HW appends it) to a port's TX FIFO.
 * Short frames are padded to the 60-byte Ethernet minimum.
 */
adin2111_status_t adin2111_write_frame(adin2111_t *dev, adin2111_port_t port,
                                       const uint8_t *frame, uint16_t len);

/**
 * Acknowledge/clear the interrupt sources by reading STATUS0/STATUS1.
 * Call this from the RX task after being woken by the INT line so the ADIN2111
 * can de-assert its (level, active-low) interrupt output.
 * @param status1 [out] STATUS1 snapshot (may be NULL).
 */
adin2111_status_t adin2111_irq_ack(adin2111_t *dev, uint32_t *status1);

#ifdef __cplusplus
}
#endif

#endif /* ADIN2111_H */
