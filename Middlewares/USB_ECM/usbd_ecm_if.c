/**
 * @file    usbd_ecm_if.c
 * @brief   Bridge <-> CDC-ECM class adapter.
 */
#include "usbd_ecm_if.h"
#include "usbd_ecm.h"
#include "usb_device.h"

#define ECM_IF_TX_TIMEOUT_MS   100u

void usbd_ecm_if_register(void)
{
    /* Bring up the USB device core, register the ECM class and start it. */
    usb_device_init();
}

int usbd_ecm_if_send(net_frame_t *frame)
{
    if (frame == NULL) {
        return -1;
    }
    return USBD_ECM_SendFrameBlocking(frame->data, frame->len,
                                      ECM_IF_TX_TIMEOUT_MS);
}

void usbd_ecm_if_set_link_state(bool up)
{
    USBD_ECM_SetLinkState(up);
}

bool usbd_ecm_if_ready(void)
{
    return USBD_ECM_IsReady();
}
