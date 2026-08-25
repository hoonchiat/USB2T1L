/**
 * @file    usbd_desc.h
 * @brief   USB device + string descriptors for the ECM bridge.
 */
#ifndef USBD_DESC_H
#define USBD_DESC_H

#include "usbd_def.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NOTE: 0x0483 is STMicroelectronics' Vendor ID. The PID below is arbitrary
 * and MUST be replaced with a VID/PID pair you are licensed to use before
 * shipping a product. It is fine for bench bring-up / development. */
#define USBD_VID                     0x0483
#define USBD_PID_FS                  0xA111
#define USBD_LANGID_STRING           0x0409   /* English (US) */
#define USBD_MANUFACTURER_STRING     "STMicro + Analog Devices"
#define USBD_PRODUCT_STRING_FS       "STM32F407 ADIN2111 USB Ethernet"
#define USBD_CONFIGURATION_STRING_FS "ECM Config"
#define USBD_INTERFACE_STRING_FS     "ECM Interface"

extern USBD_DescriptorsTypeDef ECM_Desc;

#ifdef __cplusplus
}
#endif

#endif /* USBD_DESC_H */
