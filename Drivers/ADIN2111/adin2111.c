/**
 * @file    adin2111.c
 * @brief   Platform-independent ADIN2111 / ADIN1110 driver core.
 *
 * Implements the Analog Devices "generic" SPI control protocol:
 *
 *   Register write : [CD|WR|addr_hi][addr_lo] (+hdr_crc) [d3 d2 d1 d0] (+crc)
 *   Register read  : [CD|addr_hi][addr_lo] (+hdr_crc) [TA] [d3 d2 d1 d0] (+crc)
 *   TX FIFO write  : [CD|WR|addr_hi][addr_lo] (+hdr_crc) [port_hdr][frame..pad]
 *   RX FIFO read   : [CD|addr_hi][addr_lo] (+hdr_crc) [TA] [port_hdr][frame..]
 *
 * All 32-bit registers are big-endian on the wire. FIFO payload lengths are
 * rounded up to a multiple of 4 bytes. When CRC is enabled a CRC-8 (poly 0x07,
 * MSB-first, init 0) is appended after the 2-byte command header (and, for
 * register transfers, after the 4 data bytes).
 */
#include "adin2111.h"
#include "adin2111_regs.h"

#include <string.h>

/* Largest SPI burst we build on the stack: control header + a full frame. */
#define ADIN_SPI_MAXBURST   (8u + 1536u)

/* ------------------------------------------------------------------------- */
/* Helpers                                                                   */
/* ------------------------------------------------------------------------- */

static uint8_t adin_crc8(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static inline uint16_t adin_round4(uint16_t len)
{
    return (uint16_t)((len + 3u) & ~3u);
}

/* Build the 2-byte command header (+optional header CRC) into buf.
 * Returns the number of header bytes written. */
static uint16_t adin_build_header(const adin2111_t *dev, uint16_t reg,
                                  bool write, uint8_t *buf)
{
    buf[0] = (uint8_t)(ADIN_SPI_CD | (write ? ADIN_SPI_RW_WRITE : 0u) |
                       ADIN_SPI_ADDR_HI(reg));
    buf[1] = (uint8_t)ADIN_SPI_ADDR_LO(reg);
    uint16_t n = 2;
    if (dev->crc_enabled) {
        buf[2] = adin_crc8(buf, 2);
        n = 3;
    }
    return n;
}

/* ------------------------------------------------------------------------- */
/* Register access                                                           */
/* ------------------------------------------------------------------------- */

adin2111_status_t adin2111_write_reg(adin2111_t *dev, uint16_t reg, uint32_t val)
{
    uint8_t tx[8];
    uint16_t n = adin_build_header(dev, reg, true, tx);

    /* 32-bit big-endian data */
    tx[n + 0] = (uint8_t)(val >> 24);
    tx[n + 1] = (uint8_t)(val >> 16);
    tx[n + 2] = (uint8_t)(val >> 8);
    tx[n + 3] = (uint8_t)(val);
    uint16_t len = (uint16_t)(n + 4);
    if (dev->crc_enabled) {
        tx[len] = adin_crc8(&tx[n], 4);
        len++;
    }

    dev->hal.cs_assert(dev->hal.ctx);
    int rc = dev->hal.spi_xfer(dev->hal.ctx, tx, NULL, len);
    dev->hal.cs_deassert(dev->hal.ctx);
    return (rc == 0) ? ADIN2111_OK : ADIN2111_ERR_SPI;
}

adin2111_status_t adin2111_read_reg(adin2111_t *dev, uint16_t reg, uint32_t *val)
{
    if (val == NULL) {
        return ADIN2111_ERR_PARAM;
    }

    uint8_t tx[10] = {0};
    uint8_t rx[10] = {0};
    uint16_t n = adin_build_header(dev, reg, false, tx);
    tx[n] = 0x00;            /* turnaround byte */
    n++;
    uint16_t data_off = n;
    uint16_t len = (uint16_t)(n + 4 + (dev->crc_enabled ? 1u : 0u));

    dev->hal.cs_assert(dev->hal.ctx);
    int rc = dev->hal.spi_xfer(dev->hal.ctx, tx, rx, len);
    dev->hal.cs_deassert(dev->hal.ctx);
    if (rc != 0) {
        return ADIN2111_ERR_SPI;
    }

    if (dev->crc_enabled) {
        uint8_t crc = adin_crc8(&rx[data_off], 4);
        if (crc != rx[data_off + 4]) {
            return ADIN2111_ERR_CRC;
        }
    }

    *val = ((uint32_t)rx[data_off + 0] << 24) |
           ((uint32_t)rx[data_off + 1] << 16) |
           ((uint32_t)rx[data_off + 2] << 8) |
           ((uint32_t)rx[data_off + 3]);
    return ADIN2111_OK;
}

/* ------------------------------------------------------------------------- */
/* FIFO (frame) access                                                       */
/* ------------------------------------------------------------------------- */

/* Write a frame body (port header + payload, already length-rounded) to a
 * TX/RX data register in a single CS-low burst. */
static adin2111_status_t adin_fifo_write(adin2111_t *dev, uint16_t reg,
                                         const uint8_t *body, uint16_t body_len)
{
    static uint8_t tx[ADIN_SPI_MAXBURST];
    uint16_t n = adin_build_header(dev, reg, true, tx);
    if ((uint32_t)n + body_len > sizeof(tx)) {
        return ADIN2111_ERR_TOOBIG;
    }
    memcpy(&tx[n], body, body_len);

    dev->hal.cs_assert(dev->hal.ctx);
    int rc = dev->hal.spi_xfer(dev->hal.ctx, tx, NULL, (uint16_t)(n + body_len));
    dev->hal.cs_deassert(dev->hal.ctx);
    return (rc == 0) ? ADIN2111_OK : ADIN2111_ERR_SPI;
}

/* Read a frame body of body_len bytes from an RX data register. */
static adin2111_status_t adin_fifo_read(adin2111_t *dev, uint16_t reg,
                                        uint8_t *body, uint16_t body_len)
{
    static uint8_t tx[ADIN_SPI_MAXBURST];
    static uint8_t rx[ADIN_SPI_MAXBURST];
    uint16_t n = adin_build_header(dev, reg, false, tx);
    tx[n] = 0x00;   /* turnaround */
    n++;
    if ((uint32_t)n + body_len > sizeof(tx)) {
        return ADIN2111_ERR_TOOBIG;
    }
    memset(&tx[n], 0, body_len);

    dev->hal.cs_assert(dev->hal.ctx);
    int rc = dev->hal.spi_xfer(dev->hal.ctx, tx, rx, (uint16_t)(n + body_len));
    dev->hal.cs_deassert(dev->hal.ctx);
    if (rc != 0) {
        return ADIN2111_ERR_SPI;
    }
    memcpy(body, &rx[n], body_len);
    return ADIN2111_OK;
}

/* ------------------------------------------------------------------------- */
/* High-level operations                                                     */
/* ------------------------------------------------------------------------- */

adin2111_status_t adin2111_read_status(adin2111_t *dev, uint32_t *status1)
{
    return adin2111_read_reg(dev, ADIN_REG_STATUS1, status1);
}

bool adin2111_rx_ready(adin2111_t *dev)
{
    uint32_t s1 = 0;
    if (adin2111_read_reg(dev, ADIN_REG_STATUS1, &s1) != ADIN2111_OK) {
        return false;
    }
    return (s1 & (ADIN_STATUS1_RX_RDY | ADIN2111_STATUS1_P2_RX_RDY)) != 0u;
}

bool adin2111_link_up(adin2111_t *dev, adin2111_port_t port)
{
    uint32_t s1 = 0;
    if (adin2111_read_reg(dev, ADIN_REG_STATUS1, &s1) != ADIN2111_OK) {
        return false;
    }
    uint32_t mask = (port == ADIN2111_PORT_2) ? ADIN2111_STATUS1_P2_LINK_STATE
                                              : ADIN_STATUS1_LINK_STATE;
    return (s1 & mask) != 0u;
}

adin2111_status_t adin2111_irq_ack(adin2111_t *dev, uint32_t *status1)
{
    uint32_t s0 = 0, s1 = 0;
    adin2111_status_t rc;

    rc = adin2111_read_reg(dev, ADIN_REG_STATUS0, &s0);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    rc = adin2111_read_reg(dev, ADIN_REG_STATUS1, &s1);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    /* STATUS registers are write-1-to-clear for the latched error bits. */
    (void)adin2111_write_reg(dev, ADIN_REG_STATUS0, s0);
    (void)adin2111_write_reg(dev, ADIN_REG_STATUS1,
                             s1 & ~(ADIN_STATUS1_RX_RDY |
                                    ADIN2111_STATUS1_P2_RX_RDY));
    if (status1 != NULL) {
        *status1 = s1;
    }
    return ADIN2111_OK;
}

adin2111_status_t adin2111_read_frame(adin2111_t *dev,
                                      uint8_t *buf, uint16_t buf_len,
                                      uint16_t *out_len,
                                      adin2111_port_t *out_port)
{
    if (buf == NULL || out_len == NULL) {
        return ADIN2111_ERR_PARAM;
    }

    /* Pick the port that has a frame ready, port 1 first. */
    uint32_t s1 = 0;
    adin2111_status_t rc = adin2111_read_reg(dev, ADIN_REG_STATUS1, &s1);
    if (rc != ADIN2111_OK) {
        return rc;
    }

    uint16_t fsize_reg, fifo_reg;
    adin2111_port_t port;
    if (s1 & ADIN_STATUS1_RX_RDY) {
        fsize_reg = ADIN_REG_RX_FSIZE;
        fifo_reg  = ADIN_REG_RX;
        port      = ADIN2111_PORT_1;
    } else if ((dev->num_ports > 1) && (s1 & ADIN2111_STATUS1_P2_RX_RDY)) {
        fsize_reg = ADIN2111_REG_RX_P2_FSIZE;
        fifo_reg  = ADIN2111_REG_RX_P2;
        port      = ADIN2111_PORT_2;
    } else {
        return ADIN2111_ERR_NODATA;
    }

    uint32_t fsize = 0;
    rc = adin2111_read_reg(dev, fsize_reg, &fsize);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    /* fsize includes the 2-byte internal port header and the 4-byte FCS. */
    if (fsize < (ADIN_FRAME_HEADER_LEN + ADIN_FCS_LEN) ||
        fsize > (ADIN_FRAME_HEADER_LEN + ADIN_FCS_LEN + ADIN_MAX_ETH_FRAME)) {
        /* Corrupt length: flush the offending FIFO and report no data. */
        (void)adin2111_write_reg(dev, ADIN_REG_FIFO_CLR, ADIN_FIFO_CLR_RX);
        return ADIN2111_ERR_NODATA;
    }

    uint16_t body_len = adin_round4((uint16_t)fsize);
    uint16_t eth_len  = (uint16_t)(fsize - ADIN_FRAME_HEADER_LEN - ADIN_FCS_LEN);
    if (eth_len > buf_len) {
        (void)adin2111_write_reg(dev, ADIN_REG_FIFO_CLR, ADIN_FIFO_CLR_RX);
        return ADIN2111_ERR_TOOBIG;
    }

    /* Read [port_hdr(2)][eth frame][FCS(4)][pad] into a scratch buffer. */
    static uint8_t body[ADIN_SPI_MAXBURST];
    rc = adin_fifo_read(dev, fifo_reg, body, body_len);
    if (rc != ADIN2111_OK) {
        return rc;
    }

    /* body[0..1] is the ADIN internal frame header (skipped, like the ADI /
     * Linux drivers do); the reliable ingress port is the FIFO we drained. */
    if (out_port != NULL) {
        *out_port = port;
    }
    memcpy(buf, &body[ADIN_FRAME_HEADER_LEN], eth_len);
    *out_len = eth_len;
    return ADIN2111_OK;
}

adin2111_status_t adin2111_write_frame(adin2111_t *dev, adin2111_port_t port,
                                       const uint8_t *frame, uint16_t len)
{
    if (frame == NULL || len == 0u || len > ADIN_MAX_ETH_FRAME) {
        return ADIN2111_ERR_PARAM;
    }

    /* Pad short frames to the 60-byte Ethernet minimum (HW appends 4B FCS). */
    uint16_t eth_len = (len < 60u) ? 60u : len;

    /* body = [port header(2)][ethernet frame][zero pad] rounded to 4 bytes */
    uint16_t body_len = adin_round4((uint16_t)(eth_len + ADIN_FRAME_HEADER_LEN));

    /* TX_FSIZE counts the port header + ethernet frame (HW adds the FCS). */
    uint32_t fsize = (uint32_t)(eth_len + ADIN_FRAME_HEADER_LEN);

    /* Ensure there is room in the TX FIFO (TX_SPACE is in half-words). */
    uint32_t space = 0;
    adin2111_status_t rc = adin2111_read_reg(dev, ADIN_REG_TX_SPACE, &space);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    if ((space * 2u) < ((uint32_t)body_len + ADIN_FCS_LEN)) {
        return ADIN2111_ERR_NOSPACE;
    }

    rc = adin2111_write_reg(dev, ADIN_REG_TX_FSIZE, fsize);
    if (rc != ADIN2111_OK) {
        return rc;
    }

    static uint8_t body[ADIN_SPI_MAXBURST];
    memset(body, 0, body_len);
    body[0] = (uint8_t)(((uint16_t)port >> 8) & 0xFF);   /* port hdr, big-end */
    body[1] = (uint8_t)((uint16_t)port & 0xFF);
    memcpy(&body[ADIN_FRAME_HEADER_LEN], frame, len);
    /* bytes [len .. eth_len) already zero from memset (min-frame padding)   */

    return adin_fifo_write(dev, ADIN_REG_TX, body, body_len);
}

adin2111_status_t adin2111_set_host_mac(adin2111_t *dev, const uint8_t mac[6])
{
    if (mac == NULL) {
        return ADIN2111_ERR_PARAM;
    }
    uint32_t upr = ((uint32_t)mac[0] << 8) | mac[1];
    uint32_t lwr = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                   ((uint32_t)mac[4] << 8) | mac[5];

    /* Forward frames matching our address to the host on both ports. */
    uint32_t upr_ctl = upr | ADIN_MAC_ADDR_TO_HOST | ADIN_MAC_ADDR_APPLY2PORT1;
    if (dev->num_ports > 1) {
        upr_ctl |= ADIN2111_MAC_ADDR_APPLY2PORT2;
    }

    adin2111_status_t rc;
    rc = adin2111_write_reg(dev,
             ADIN_REG_MAC_ADDR_FILT_UPR(ADIN_MAC_SLOT_HOST), upr_ctl);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    rc = adin2111_write_reg(dev,
             ADIN_REG_MAC_ADDR_FILT_LWR(ADIN_MAC_SLOT_HOST), lwr);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    /* Exact-match mask (all address bits significant). */
    (void)adin2111_write_reg(dev, ADIN_REG_MAC_ADDR_MASK_UPR, 0x0000FFFFu);
    (void)adin2111_write_reg(dev, ADIN_REG_MAC_ADDR_MASK_LWR, 0xFFFFFFFFu);
    return ADIN2111_OK;
}

/* Program the broadcast forwarding entry (dst = ff:ff:ff:ff:ff:ff -> host). */
static adin2111_status_t adin_set_broadcast_filter(adin2111_t *dev)
{
    uint32_t upr = 0x0000FFFFu | ADIN_MAC_ADDR_TO_HOST | ADIN_MAC_ADDR_APPLY2PORT1;
    if (dev->num_ports > 1) {
        upr |= ADIN2111_MAC_ADDR_APPLY2PORT2;
    }
    adin2111_status_t rc;
    rc = adin2111_write_reg(dev,
             ADIN_REG_MAC_ADDR_FILT_UPR(ADIN_MAC_SLOT_BROADCAST), upr);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    return adin2111_write_reg(dev,
             ADIN_REG_MAC_ADDR_FILT_LWR(ADIN_MAC_SLOT_BROADCAST), 0xFFFFFFFFu);
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* ------------------------------------------------------------------------- */

adin2111_status_t adin2111_init(adin2111_t *dev,
                                const adin2111_hal_t *hal,
                                bool crc_enabled)
{
    if (dev == NULL || hal == NULL || hal->spi_xfer == NULL) {
        return ADIN2111_ERR_PARAM;
    }
    memset(dev, 0, sizeof(*dev));
    dev->hal = *hal;
    dev->crc_enabled = crc_enabled;
    dev->num_ports = 2;   /* assume ADIN2111; corrected after id read below  */

    /* Hardware reset pulse (RST is active low). */
    if (dev->hal.reset_assert) {
        dev->hal.reset_assert(dev->hal.ctx);
        dev->hal.delay_ms(5);
        dev->hal.reset_deassert(dev->hal.ctx);
    }
    dev->hal.delay_ms(50);   /* datasheet: allow the device to boot          */

    /* Software reset for good measure, then wait for RESETC. */
    (void)adin2111_write_reg(dev, ADIN_REG_RESET, ADIN_RESET_SWRESET);
    dev->hal.delay_ms(10);

    uint32_t s0 = 0;
    for (int i = 0; i < 100; i++) {
        if (adin2111_read_reg(dev, ADIN_REG_STATUS0, &s0) == ADIN2111_OK &&
            (s0 & ADIN_STATUS0_RESETC)) {
            break;
        }
        dev->hal.delay_ms(1);
    }

    /* Identify the device. */
    adin2111_status_t rc = adin2111_read_reg(dev, ADIN_REG_PHYID, &dev->phy_id);
    if (rc != ADIN2111_OK) {
        return rc;
    }
    if (dev->phy_id == ADIN1110_PHYID_VAL) {
        dev->num_ports = 1;
    } else if (dev->phy_id == ADIN2111_PHYID_VAL) {
        dev->num_ports = 2;
    } else {
        return ADIN2111_ERR_ID;
    }

    /* Clear both FIFOs. */
    (void)adin2111_write_reg(dev, ADIN_REG_FIFO_CLR,
                             ADIN_FIFO_CLR_RX | ADIN_FIFO_CLR_TX);

    /* CONFIG2: append/check FCS in hardware, forward unknown-DA frames to the
     * host on both ports so the bridge behaves like a promiscuous NIC. */
    uint32_t cfg2 = ADIN_CONFIG2_CRC_APPEND | ADIN_CONFIG2_FWD_UNK2HOST;
    if (dev->num_ports > 1) {
        cfg2 |= ADIN2111_CONFIG2_P2_FWD_UNK2HOST;
    }
    rc = adin2111_write_reg(dev, ADIN_REG_CONFIG2, cfg2);
    if (rc != ADIN2111_OK) {
        return rc;
    }

    /* Broadcast forwarding filter. Unicast (host) filter is set later via
     * adin2111_set_host_mac() once the MAC address is known. */
    (void)adin_set_broadcast_filter(dev);

    /* Unmask RX_RDY (both ports) and SPI error interrupts; mask stays 1=off
     * for everything else. */
    uint32_t imask1 = 0xFFFFFFFFu;
    imask1 &= ~(uint32_t)(ADIN_IMASK1_RX_RDY | ADIN_IMASK1_SPI_ERR);
    if (dev->num_ports > 1) {
        imask1 &= ~(uint32_t)ADIN2111_IMASK1_P2_RX_RDY;
    }
    rc = adin2111_write_reg(dev, ADIN_REG_IMASK1, imask1);
    if (rc != ADIN2111_OK) {
        return rc;
    }

    /* Signal configuration complete: MAC starts forwarding once SYNC is set. */
    uint32_t cfg0 = 0;
    (void)adin2111_read_reg(dev, ADIN_REG_CONFIG0, &cfg0);
    cfg0 |= ADIN_CONFIG0_SYNC;
    rc = adin2111_write_reg(dev, ADIN_REG_CONFIG0, cfg0);
    return rc;
}
