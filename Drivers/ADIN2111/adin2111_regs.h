/**
 * @file    adin2111_regs.h
 * @brief   Register map and bit definitions for the Analog Devices ADIN2111
 *          (and pin-compatible ADIN1110) 10BASE-T1L MAC-PHY.
 *
 * Values taken from the ADIN2111 datasheet / user guide and cross-checked
 * against the mainline Linux driver (drivers/net/ethernet/adi/adin1110.c)
 * and the ADI no-OS driver. Registers are 32-bit and transferred big-endian
 * over SPI using the "generic" (non OPEN Alliance) control protocol.
 */
#ifndef ADIN2111_REGS_H
#define ADIN2111_REGS_H

/* --- Generic SPI control-word bits (first header byte) ------------------- */
#define ADIN_SPI_CD                  (1U << 7)   /* control (0) vs data(?)   */
#define ADIN_SPI_RW_WRITE            (1U << 5)   /* set = write, clear = read*/
/* Register address is 13 bits: addr[12:8] in header byte0[4:0],
 * addr[7:0] in header byte1.                                               */
#define ADIN_SPI_ADDR_HI(reg)        (((reg) >> 8) & 0x1FU)
#define ADIN_SPI_ADDR_LO(reg)        ((reg) & 0xFFU)

/* --- Register addresses -------------------------------------------------- */
#define ADIN_REG_IDVER               0x00U   /* MAC-PHY id / version         */
#define ADIN_REG_PHYID               0x01U   /* device identification        */
#define ADIN_REG_CAPABILITY          0x02U
#define ADIN_REG_RESET               0x03U   /* software reset               */
#define ADIN_REG_CONFIG0             0x04U   /* (a.k.a CONFIG1 in some docs) */
#define ADIN_REG_CONFIG2             0x06U
#define ADIN_REG_STATUS0             0x08U
#define ADIN_REG_STATUS1             0x09U
#define ADIN_REG_BUFSTS              0x0BU
#define ADIN_REG_IMASK0              0x0CU
#define ADIN_REG_IMASK1              0x0DU
#define ADIN_REG_MDIOACC(x)          (0x20U + (x))
#define ADIN_REG_TX_FSIZE            0x30U   /* next TX frame size (bytes)   */
#define ADIN_REG_TX                  0x31U   /* TX FIFO data port            */
#define ADIN_REG_TX_SPACE            0x32U   /* free TX FIFO space (halfwd)  */
#define ADIN_REG_FIFO_CLR            0x36U
#define ADIN_REG_SOFT_RST            0x3CU   /* key-based soft reset         */
#define ADIN_REG_MAC_RST_STATUS      0x3BU
#define ADIN_REG_MAC_ADDR_FILT_UPR(x) (0x50U + 2U * (x))
#define ADIN_REG_MAC_ADDR_FILT_LWR(x) (0x51U + 2U * (x))
#define ADIN_REG_MAC_ADDR_MASK_UPR   0x70U
#define ADIN_REG_MAC_ADDR_MASK_LWR   0x71U
#define ADIN_REG_RX_FSIZE            0x90U   /* port 1 RX frame size (bytes) */
#define ADIN_REG_RX                  0x91U   /* port 1 RX FIFO data port     */
#define ADIN2111_REG_RX_P2_FSIZE     0xC0U   /* port 2 RX frame size         */
#define ADIN2111_REG_RX_P2           0xC1U   /* port 2 RX FIFO data port     */

/* --- RESET (0x03) -------------------------------------------------------- */
#define ADIN_RESET_SWRESET           (1U << 0)

/* --- Key-based reset (0x3C) --------------------------------------------- */
#define ADIN_SWRESET_KEY1            0x4F1CU
#define ADIN_SWRESET_KEY2            0xC1F4U
#define ADIN_SWRELEASE_KEY1          0x6F1AU
#define ADIN_SWRELEASE_KEY2          0xA1F6U

/* --- CONFIG0 (0x04) ------------------------------------------------------ */
#define ADIN_CONFIG0_SYNC            (1U << 15)  /* config done / MAC in sync */
#define ADIN_CONFIG0_TXCTE           (1U << 7)
#define ADIN_CONFIG0_RXCTE           (1U << 6)
#define ADIN_CONFIG0_TXFCSVE         (1U << 14)

/* --- CONFIG2 (0x06) ------------------------------------------------------ */
#define ADIN_CONFIG2_CRC_APPEND      (1U << 5)   /* HW appends/checks FCS     */
#define ADIN_CONFIG2_FWD_UNK2HOST    (1U << 2)   /* port1 unknown -> host     */
#define ADIN2111_CONFIG2_PORT_CUT_THRU (1U << 11)
#define ADIN2111_CONFIG2_P2_FWD_UNK2HOST (1U << 12)

/* --- STATUS0 (0x08) ------------------------------------------------------ */
#define ADIN_STATUS0_TXPE            (1U << 0)   /* TX protocol error         */
#define ADIN_STATUS0_RESETC          (1U << 6)   /* reset complete            */

/* --- STATUS1 (0x09) ------------------------------------------------------ */
#define ADIN_STATUS1_LINK_STATE      (1U << 0)   /* port1 PHY link up         */
#define ADIN_STATUS1_RX_RDY          (1U << 4)   /* port1 RX frame available  */
#define ADIN_STATUS1_SPI_ERR         (1U << 10)
#define ADIN2111_STATUS1_P2_RX_RDY   (1U << 17)  /* port2 RX frame available  */
#define ADIN2111_STATUS1_P2_LINK_STATE (1U << 5)

/* --- IMASK0 / IMASK1 (interrupt mask, 1 = masked/disabled) -------------- */
#define ADIN_IMASK1_TX_RDY           (1U << 3)
#define ADIN_IMASK1_RX_RDY           (1U << 4)
#define ADIN_IMASK1_SPI_ERR          (1U << 10)
#define ADIN2111_IMASK1_P2_RX_RDY    (1U << 17)

/* --- FIFO_CLR (0x36) ----------------------------------------------------- */
#define ADIN_FIFO_CLR_RX             (1U << 0)
#define ADIN_FIFO_CLR_TX             (1U << 1)

/* --- MAC address filter table entries ------------------------------------ */
#define ADIN_MAC_ADDR_APPLY2PORT1    (1U << 30)
#define ADIN2111_MAC_ADDR_APPLY2PORT2 (1U << 31)
#define ADIN_MAC_ADDR_TO_HOST        (1U << 16)
#define ADIN2111_MAC_ADDR_TO_OTHER_PORT (1U << 17)

/* MAC address slot assignments used by adin2111.c */
#define ADIN_MAC_SLOT_BROADCAST      0U
#define ADIN_MAC_SLOT_HOST           1U   /* our own unicast (host) address  */
#define ADIN_MAC_SLOT_MULTICAST      2U

/* --- Device identification ---------------------------------------------- */
#define ADIN1110_PHYID_VAL           0x0283BC91U
#define ADIN2111_PHYID_VAL           0x0283BCA1U

/* --- Frame framing constants -------------------------------------------- */
#define ADIN_FRAME_HEADER_LEN        2U   /* internal port header, per frame */
#define ADIN_FCS_LEN                 4U   /* Ethernet FCS                    */
/* Largest Ethernet frame (no FCS) the driver's scratch buffers accept.
 * 1518 = 6+6+2+1500 dst/src/type/payload, rounded up to a 4-byte multiple. */
#define ADIN_MAX_ETH_FRAME           1520U

#endif /* ADIN2111_REGS_H */
