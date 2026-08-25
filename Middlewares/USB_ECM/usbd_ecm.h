/**
 * @file    usbd_ecm.h
 * @brief   USB CDC-ECM (Ethernet Control Model) device class for the ST USB
 *          Device Library. Presents the device to a Linux host as a standard
 *          USB Ethernet interface handled by the in-tree cdc_ether driver.
 *
 * Interface layout (one configuration):
 *   IF0  Communications / ECM  : 1x Interrupt IN  (notifications)
 *   IF1  Data                  : alt0 = no endpoints (idle)
 *                                alt1 = Bulk IN + Bulk OUT (frame data)
 */
#ifndef USBD_ECM_H
#define USBD_ECM_H

#include <stdint.h>
#include <stdbool.h>
#include "usbd_ioreq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Endpoint addresses / sizes (Full Speed). */
#define CDC_ECM_IN_EP                   0x81U   /* bulk IN  (device -> host)  */
#define CDC_ECM_OUT_EP                  0x01U   /* bulk OUT (host -> device)  */
#define CDC_ECM_CMD_EP                  0x82U   /* interrupt IN (notify)      */

#define CDC_ECM_DATA_MAX_PACKET_SIZE    64U     /* FS bulk max packet         */
#define CDC_ECM_CMD_PACKET_SIZE         16U     /* interrupt notify size      */
#define CDC_ECM_CMD_ITF_NBR             0x00U
#define CDC_ECM_DATA_ITF_NBR            0x01U

/* Ethernet segment size advertised in the ECM functional descriptor. */
#define CDC_ECM_ETH_MAX_SEGMENT_SIZE    1514U

/* String descriptor index carrying the 12-hex-digit host MAC address.
 * Must be >= 6: indices 0..5 are reserved by the ST core string switch
 * (LANGID/MFC/PRODUCT/SERIAL/CONFIG/INTERFACE); only higher indices fall
 * through to the class GetUsrStrDescriptor callback. */
#define CDC_ECM_MAC_STRING_INDEX        0x06U

/* The class object registered with USBD_RegisterClass(). */
extern USBD_ClassTypeDef USBD_ECM;

/**
 * Transmit one Ethernet frame (no FCS) to the host over the bulk IN endpoint,
 * blocking until the transfer (and any required zero-length terminator) has
 * completed or @p timeout_ms elapses.
 * @return 0 on success, negative on error / not ready / timeout.
 */
int  USBD_ECM_SendFrameBlocking(const uint8_t *data, uint16_t len,
                                uint32_t timeout_ms);

/** Update the ECM NETWORK_CONNECTION state reported to the host. */
void USBD_ECM_SetLinkState(bool up);

/** @return true once the host has selected data interface alt-setting 1. */
bool USBD_ECM_IsReady(void);

/** Create the RTOS objects the class needs. Call from task context (before
 *  the scheduler is running is fine) prior to USBD_Start, since the class Init
 *  callback runs in the USB ISR where object creation is not allowed. */
void USBD_ECM_OsInit(void);

/** Set the 6-byte MAC advertised to the host (call before USBD_Start). */
void USBD_ECM_SetMacAddress(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* USBD_ECM_H */
