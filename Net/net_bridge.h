/**
 * @file    net_bridge.h
 * @brief   Glue between the ADIN2111 (SPI) side and the USB CDC-ECM side.
 *
 * Two data directions, each with its own buffer pool:
 *
 *   Uplink   ADIN2111 --SPI--> [rx_pool] --net_rx_task--> usbd_ecm_if_send()
 *            The USB class transmits the frame to the host and, when the IN
 *            transfer completes, returns the buffer with net_bridge_uplink_free().
 *
 *   Downlink host --USB OUT--> [tx_pool] --net_bridge_downlink_submit()-->
 *            to_adin_q --net_tx_task--> ADIN2111 (SPI write), then the buffer
 *            is returned to tx_pool.
 *
 * The EXTI ISR for the ADIN INT line calls net_bridge_adin_int_isr() to wake
 * the uplink task.
 */
#ifndef NET_BRIDGE_H
#define NET_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>

#include "frame_pool.h"
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Create pools, queues, the ADIN device and the bridge tasks. Call before
 *  starting the scheduler (after HAL / BSP init). Returns false on failure. */
bool net_bridge_init(void);

/** Wake the uplink task from the ADIN INT EXTI ISR. */
void net_bridge_adin_int_isr(void);

/* ---- Downlink (USB -> ADIN) buffer ownership, used by the USB class ------ */

/** Borrow a tx_pool buffer to arm a USB OUT transfer into. NULL if the pool
 *  is exhausted (host is faster than the SPI link). ISR-safe variant too. */
net_frame_t *net_bridge_downlink_alloc(void);
net_frame_t *net_bridge_downlink_alloc_isr(BaseType_t *woken);

/** Submit a filled host frame (set frame->len first) for transmission to the
 *  ADIN2111. Ownership passes to the bridge. Call from USB IRQ (DataOut). */
void net_bridge_downlink_submit_isr(net_frame_t *frame, BaseType_t *woken);

/** Give a borrowed downlink buffer back without submitting it (e.g. on error).*/
void net_bridge_downlink_discard_isr(net_frame_t *frame, BaseType_t *woken);

#ifdef __cplusplus
}
#endif

#endif /* NET_BRIDGE_H */
