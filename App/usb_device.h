/**
 * @file    usb_device.h
 * @brief   Top-level USB device (CDC-ECM) bring-up.
 */
#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#ifdef __cplusplus
extern "C" {
#endif

/** USB device index passed to USBD_Init (full-speed core = 0). */
#ifndef DEVICE_FS
#define DEVICE_FS   0
#endif

/** Initialise the USB device core, register the CDC-ECM class (with the MAC
 *  from bsp_get_mac_address()) and start the peripheral. */
void usb_device_init(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_DEVICE_H */
