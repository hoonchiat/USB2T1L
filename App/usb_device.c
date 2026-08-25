/**
 * @file    usb_device.c
 * @brief   Top-level USB device (CDC-ECM) bring-up.
 */
#include "usb_device.h"

#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_ecm.h"
#include "bsp.h"

USBD_HandleTypeDef hUsbDeviceFS;

void usb_device_init(void)
{
    /* Create the class's RTOS objects before the USB ISR can run. */
    USBD_ECM_OsInit();

    /* Advertise the same MAC to the host as we program into the ADIN filter. */
    uint8_t mac[6];
    bsp_get_mac_address(mac);
    USBD_ECM_SetMacAddress(mac);

    if (USBD_Init(&hUsbDeviceFS, &ECM_Desc, DEVICE_FS) != USBD_OK) {
        bsp_fatal("USBD_Init");
    }
    if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_ECM) != USBD_OK) {
        bsp_fatal("USBD_RegisterClass");
    }
    if (USBD_Start(&hUsbDeviceFS) != USBD_OK) {
        bsp_fatal("USBD_Start");
    }
}
