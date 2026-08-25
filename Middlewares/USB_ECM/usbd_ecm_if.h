/**
 * @file    usbd_ecm_if.h
 * @brief   Application interface to the CDC-ECM USB class, used by net_bridge.
 *
 * This layer decouples the generic ECM class implementation (usbd_ecm.c) from
 * the bridge frame pools. The bridge pushes uplink frames here; the class
 * pulls host downlink frames from the bridge.
 */
#ifndef USBD_ECM_IF_H
#define USBD_ECM_IF_H

#include <stdbool.h>
#include "frame_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the class with the USB device core and bind the bridge callbacks.
 *  Called from usb_device_init(). */
void usbd_ecm_if_register(void);

/**
 * Queue an uplink (ADIN -> host) frame for transmission over the ECM bulk IN
 * endpoint. On success (0) ownership passes to the class, which returns the
 * buffer via net_bridge_uplink_free_isr() when the transfer completes. On
 * failure (-1: not configured / queue full) the caller keeps ownership.
 */
int usbd_ecm_if_send(net_frame_t *frame);

/** Notify the host of Ethernet link up/down via the ECM interrupt endpoint. */
void usbd_ecm_if_set_link_state(bool up);

/** @return true once the host has selected the ECM data-interface alt-setting. */
bool usbd_ecm_if_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* USBD_ECM_IF_H */
