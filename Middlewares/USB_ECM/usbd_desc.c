/**
 * @file    usbd_desc.c
 * @brief   Device + standard string descriptors for the CDC-ECM bridge.
 *
 * The device is declared as a Communications-class device (bDeviceClass 0x02);
 * the ECM interface layout is described by the class configuration descriptor
 * in usbd_ecm.c. The per-device MAC-address string (index 4) is served by the
 * class GetUsrStrDescriptor callback, not here.
 */
#include "usbd_desc.h"
#include "usbd_ctlreq.h"

#define USBD_MAX_STR_DESC_SIZ        64U
#define DEVICE_ID_REG_BASE           0x1FFF7A10U   /* STM32F4 96-bit UID      */

/* clang-format off */
__ALIGN_BEGIN static uint8_t USBD_DeviceDesc[USB_LEN_DEV_DESC] __ALIGN_END = {
    0x12,                       /* bLength                                    */
    USB_DESC_TYPE_DEVICE,       /* bDescriptorType                            */
    0x00, 0x02,                 /* bcdUSB = 2.00                              */
    0x02,                       /* bDeviceClass = Communications              */
    0x00,                       /* bDeviceSubClass                            */
    0x00,                       /* bDeviceProtocol                            */
    USB_MAX_EP0_SIZE,           /* bMaxPacketSize0                            */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID_FS), HIBYTE(USBD_PID_FS),
    0x00, 0x02,                 /* bcdDevice = 2.00                           */
    USBD_IDX_MFC_STR,           /* iManufacturer                             */
    USBD_IDX_PRODUCT_STR,       /* iProduct                                  */
    USBD_IDX_SERIAL_STR,        /* iSerialNumber                             */
    USBD_MAX_NUM_CONFIGURATION, /* bNumConfigurations                        */
};

__ALIGN_BEGIN static uint8_t USBD_LangIDDesc[USB_LEN_LANGID_STR_DESC] __ALIGN_END = {
    USB_LEN_LANGID_STR_DESC,
    USB_DESC_TYPE_STRING,
    LOBYTE(USBD_LANGID_STRING), HIBYTE(USBD_LANGID_STRING),
};

__ALIGN_BEGIN static uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ] __ALIGN_END;

/* Serial number string descriptor, filled from the STM32 unique ID. */
__ALIGN_BEGIN static uint8_t USBD_StringSerial[26] __ALIGN_END = {
    26, USB_DESC_TYPE_STRING,
};
/* clang-format on */

static void int_to_unicode(uint32_t value, uint8_t *pbuf, uint8_t len)
{
    for (uint8_t i = 0; i < len; i++) {
        uint8_t nib = (uint8_t)(value >> 28);
        pbuf[2 * i] = (nib < 0xA) ? (uint8_t)(nib + '0')
                                  : (uint8_t)(nib + 'A' - 0xA);
        pbuf[2 * i + 1] = 0;
        value <<= 4;
    }
}

static void get_serial_num(void)
{
    uint32_t d0 = *(uint32_t *)(DEVICE_ID_REG_BASE);
    uint32_t d1 = *(uint32_t *)(DEVICE_ID_REG_BASE + 4U);
    uint32_t d2 = *(uint32_t *)(DEVICE_ID_REG_BASE + 8U);
    d0 += d2;
    if (d0 != 0U) {
        int_to_unicode(d0, &USBD_StringSerial[2], 8U);
        int_to_unicode(d1, &USBD_StringSerial[18], 4U);
    }
}

static uint8_t *ECM_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(USBD_DeviceDesc);
    return USBD_DeviceDesc;
}

static uint8_t *ECM_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = sizeof(USBD_LangIDDesc);
    return USBD_LangIDDesc;
}

static uint8_t *ECM_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed,
                                              uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_MANUFACTURER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *ECM_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_PRODUCT_STRING_FS, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *ECM_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = USBD_StringSerial[0];
    get_serial_num();
    return USBD_StringSerial;
}

static uint8_t *ECM_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_CONFIGURATION_STRING_FS, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *ECM_InterfaceStrDescriptor(USBD_SpeedTypeDef speed,
                                           uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_INTERFACE_STRING_FS, USBD_StrDesc, length);
    return USBD_StrDesc;
}

USBD_DescriptorsTypeDef ECM_Desc = {
    ECM_DeviceDescriptor,
    ECM_LangIDStrDescriptor,
    ECM_ManufacturerStrDescriptor,
    ECM_ProductStrDescriptor,
    ECM_SerialStrDescriptor,
    ECM_ConfigStrDescriptor,
    ECM_InterfaceStrDescriptor,
};
