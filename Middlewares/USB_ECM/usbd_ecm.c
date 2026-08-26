/**
 * @file    usbd_ecm.c
 * @brief   USB CDC-ECM device class implementation for the ST USB Device
 *          Library. Bridges the bulk data endpoints to net_bridge's frame
 *          pools and reports link state over the interrupt endpoint.
 *
 * Uplink (device -> host) is driven synchronously by USBD_ECM_SendFrameBlocking
 * from the net_rx_task: one frame in flight, terminated by a ZLP when the frame
 * length is a multiple of the max packet size. Downlink (host -> device) uses
 * zero-copy reception directly into a net_bridge tx_pool buffer.
 */
#include "usbd_ecm.h"
#include "usbd_ctlreq.h"

#include "net_bridge.h"
#include "prodinfo.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <string.h>

/* Vendor control request: IN | Vendor | Device, bRequest = 0x50 returns the
 * 128-byte production record (see tools/prodinfo.py `read`). */
#define VENDOR_REQ_GET_PRODINFO              0x50U

/* ECM class-specific request codes (bRequest). */
#define ECM_SET_ETHERNET_MULTICAST_FILTERS   0x40U
#define ECM_SET_ETHERNET_PM_PATTERN_FILTER   0x41U
#define ECM_GET_ETHERNET_PM_PATTERN_FILTER   0x42U
#define ECM_SET_ETHERNET_PACKET_FILTER       0x43U
#define ECM_GET_ETHERNET_STATISTIC           0x44U

/* ECM notification codes (bNotificationType). */
#define ECM_NOTIFY_NETWORK_CONNECTION        0x00U
#define ECM_NOTIFY_CONNECTION_SPEED_CHANGE   0x2AU

#define ECM_LINK_SPEED_BPS                   10000000UL   /* 10BASE-T1L */

/* ------------------------------------------------------------------------- */
/* Class state                                                               */
/* ------------------------------------------------------------------------- */

static USBD_HandleTypeDef *s_pdev;
static volatile bool       s_ready;         /* data alt-setting 1 selected   */
static uint8_t             s_data_alt;      /* current IF1 alternate setting */

static SemaphoreHandle_t   s_in_done;       /* signalled by bulk-IN DataIn   */
static volatile bool       s_in_zlp;        /* owe a terminating ZLP         */

static net_frame_t        *s_out_frame;     /* buffer armed for bulk OUT     */

static volatile bool       s_cmd_busy;      /* interrupt EP transfer active  */
static volatile bool       s_speed_pending; /* owe a CONNECTION_SPEED_CHANGE */
static uint8_t             s_notify_buf[16];

static uint8_t             s_ctl_buf[64];    /* EP0 class-request data scratch */

static uint8_t             s_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};

/* ------------------------------------------------------------------------- */
/* Configuration descriptor (Full Speed)                                     */
/* ------------------------------------------------------------------------- */

#define USB_ECM_CONFIG_DESC_SIZE   80U

/* clang-format off */
__ALIGN_BEGIN static uint8_t USBD_ECM_CfgDesc[USB_ECM_CONFIG_DESC_SIZE] __ALIGN_END = {
    /* --- Configuration descriptor --- */
    0x09,                       /* bLength                                    */
    USB_DESC_TYPE_CONFIGURATION,/* bDescriptorType                            */
    USB_ECM_CONFIG_DESC_SIZE,   /* wTotalLength (LSB)                         */
    0x00,                       /* wTotalLength (MSB)                         */
    0x02,                       /* bNumInterfaces                             */
    0x01,                       /* bConfigurationValue                        */
    0x00,                       /* iConfiguration                             */
    0x80,                       /* bmAttributes: bus powered                  */
    0x32,                       /* bMaxPower: 100 mA                          */

    /* --- Interface 0: Communications Class (ECM control) --- */
    0x09,                       /* bLength                                    */
    USB_DESC_TYPE_INTERFACE,    /* bDescriptorType                            */
    CDC_ECM_CMD_ITF_NBR,        /* bInterfaceNumber = 0                       */
    0x00,                       /* bAlternateSetting                          */
    0x01,                       /* bNumEndpoints = 1 (interrupt)              */
    0x02,                       /* bInterfaceClass = Communications           */
    0x06,                       /* bInterfaceSubClass = ECM                   */
    0x00,                       /* bInterfaceProtocol                         */
    0x00,                       /* iInterface                                 */

    /* CDC Header Functional Descriptor */
    0x05, 0x24, 0x00, 0x10, 0x01,

    /* CDC Union Functional Descriptor: master IF0, slave IF1 */
    0x05, 0x24, 0x06, CDC_ECM_CMD_ITF_NBR, CDC_ECM_DATA_ITF_NBR,

    /* CDC Ethernet Networking Functional Descriptor */
    0x0D,                       /* bLength                                    */
    0x24,                       /* bDescriptorType = CS_INTERFACE             */
    0x0F,                       /* bDescriptorSubtype = Ethernet Networking   */
    CDC_ECM_MAC_STRING_INDEX,   /* iMACAddress -> host MAC string             */
    0x00, 0x00, 0x00, 0x00,     /* bmEthernetStatistics                       */
    LOBYTE(CDC_ECM_ETH_MAX_SEGMENT_SIZE),
    HIBYTE(CDC_ECM_ETH_MAX_SEGMENT_SIZE), /* wMaxSegmentSize = 1514           */
    0x00, 0x00,                 /* wNumberMCFilters                           */
    0x00,                       /* bNumberPowerFilters                        */

    /* Endpoint: Interrupt IN (notifications) */
    0x07,                       /* bLength                                    */
    USB_DESC_TYPE_ENDPOINT,     /* bDescriptorType                            */
    CDC_ECM_CMD_EP,             /* bEndpointAddress = 0x82                    */
    0x03,                       /* bmAttributes = Interrupt                   */
    LOBYTE(CDC_ECM_CMD_PACKET_SIZE), HIBYTE(CDC_ECM_CMD_PACKET_SIZE),
    0x10,                       /* bInterval = 16 (FS ms)                     */

    /* --- Interface 1, alt 0: Data Class (idle, no endpoints) --- */
    0x09,
    USB_DESC_TYPE_INTERFACE,
    CDC_ECM_DATA_ITF_NBR,       /* bInterfaceNumber = 1                       */
    0x00,                       /* bAlternateSetting = 0                      */
    0x00,                       /* bNumEndpoints = 0                          */
    0x0A,                       /* bInterfaceClass = Data                     */
    0x00, 0x00, 0x00,

    /* --- Interface 1, alt 1: Data Class (active, 2 bulk endpoints) --- */
    0x09,
    USB_DESC_TYPE_INTERFACE,
    CDC_ECM_DATA_ITF_NBR,       /* bInterfaceNumber = 1                       */
    0x01,                       /* bAlternateSetting = 1                      */
    0x02,                       /* bNumEndpoints = 2                          */
    0x0A,                       /* bInterfaceClass = Data                     */
    0x00, 0x00, 0x00,

    /* Endpoint: Bulk IN */
    0x07, USB_DESC_TYPE_ENDPOINT, CDC_ECM_IN_EP, 0x02,
    LOBYTE(CDC_ECM_DATA_MAX_PACKET_SIZE), HIBYTE(CDC_ECM_DATA_MAX_PACKET_SIZE),
    0x00,

    /* Endpoint: Bulk OUT */
    0x07, USB_DESC_TYPE_ENDPOINT, CDC_ECM_OUT_EP, 0x02,
    LOBYTE(CDC_ECM_DATA_MAX_PACKET_SIZE), HIBYTE(CDC_ECM_DATA_MAX_PACKET_SIZE),
    0x00,
};

/* USB device qualifier (FS device). */
__ALIGN_BEGIN static uint8_t USBD_ECM_DeviceQualifierDesc[USB_LEN_DEV_QUALIFIER_DESC] __ALIGN_END = {
    USB_LEN_DEV_QUALIFIER_DESC,
    USB_DESC_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02,
    0x00, 0x00, 0x00,
    0x40,
    0x01,
    0x00,
};
/* clang-format on */

/* ------------------------------------------------------------------------- */
/* Endpoint helpers                                                          */
/* ------------------------------------------------------------------------- */

/* Called from the USB ISR (SET_INTERFACE handling), hence the ISR-safe pool
 * allocation. */
static void ecm_open_data_eps(USBD_HandleTypeDef *pdev)
{
    (void)USBD_LL_OpenEP(pdev, CDC_ECM_IN_EP, USBD_EP_TYPE_BULK,
                         CDC_ECM_DATA_MAX_PACKET_SIZE);
    pdev->ep_in[CDC_ECM_IN_EP & 0x0FU].is_used = 1U;
    (void)USBD_LL_OpenEP(pdev, CDC_ECM_OUT_EP, USBD_EP_TYPE_BULK,
                         CDC_ECM_DATA_MAX_PACKET_SIZE);
    pdev->ep_out[CDC_ECM_OUT_EP & 0x0FU].is_used = 1U;

    /* Arm the first host-to-device reception into a bridge tx buffer. */
    BaseType_t woken = pdFALSE;
    s_out_frame = net_bridge_downlink_alloc_isr(&woken);
    if (s_out_frame != NULL) {
        (void)USBD_LL_PrepareReceive(pdev, CDC_ECM_OUT_EP, s_out_frame->data,
                                     NET_MAX_FRAME_LEN);
    }
    s_ready = true;
    portYIELD_FROM_ISR(woken);
}

static void ecm_close_data_eps(USBD_HandleTypeDef *pdev)
{
    s_ready = false;
    (void)USBD_LL_CloseEP(pdev, CDC_ECM_IN_EP);
    pdev->ep_in[CDC_ECM_IN_EP & 0x0FU].is_used = 0U;
    (void)USBD_LL_CloseEP(pdev, CDC_ECM_OUT_EP);
    pdev->ep_out[CDC_ECM_OUT_EP & 0x0FU].is_used = 0U;

    if (s_out_frame != NULL) {
        BaseType_t woken = pdFALSE;
        net_bridge_downlink_discard_isr(s_out_frame, &woken);
        s_out_frame = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Class callbacks                                                           */
/* ------------------------------------------------------------------------- */

static uint8_t ECM_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    s_pdev = pdev;
    s_data_alt = 0;
    s_ready = false;
    s_in_zlp = false;
    s_cmd_busy = false;
    s_speed_pending = false;

    /* s_in_done is created by USBD_ECM_OsInit() from task context; ECM_Init
     * runs in the USB ISR where creating kernel objects is not allowed. */

    /* Interrupt IN endpoint is available in every configuration. */
    (void)USBD_LL_OpenEP(pdev, CDC_ECM_CMD_EP, USBD_EP_TYPE_INTR,
                         CDC_ECM_CMD_PACKET_SIZE);
    pdev->ep_in[CDC_ECM_CMD_EP & 0x0FU].is_used = 1U;

    return (uint8_t)USBD_OK;
}

static uint8_t ECM_DeInit(USBD_HandleTypeDef *pdev, uint8_t cfgidx)
{
    (void)cfgidx;
    ecm_close_data_eps(pdev);
    (void)USBD_LL_CloseEP(pdev, CDC_ECM_CMD_EP);
    pdev->ep_in[CDC_ECM_CMD_EP & 0x0FU].is_used = 0U;
    s_pdev = NULL;
    return (uint8_t)USBD_OK;
}

static void ECM_HandleClassReq(USBD_HandleTypeDef *pdev,
                               USBD_SetupReqTypedef *req)
{
    if (req->wLength == 0U) {
        /* No data stage: e.g. SET_ETHERNET_PACKET_FILTER (filter in wValue).
         * Nothing to store for a promiscuous bridge; just acknowledge. */
        return; /* core completes the status stage */
    }

    if ((req->bmRequest & 0x80U) != 0U) {
        /* device-to-host GET requests: we expose no statistics. */
        USBD_CtlError(pdev, req);
    } else {
        /* host-to-device SET with a data stage: receive and discard. */
        uint16_t len = req->wLength;
        if (len > sizeof(s_ctl_buf)) {
            len = sizeof(s_ctl_buf);
        }
        (void)USBD_CtlPrepareRx(pdev, s_ctl_buf, len);
    }
}

/* RAM copy of the production record for the EP0 IN control transfer (the core
 * streams from this buffer in the ISR, so it must stay valid — a static is fine). */
static uint8_t s_prodinfo_buf[PRODINFO_SIZE];

static uint8_t ECM_Setup(USBD_HandleTypeDef *pdev, USBD_SetupReqTypedef *req)
{
    switch (req->bmRequest & USB_REQ_TYPE_MASK) {
    case USB_REQ_TYPE_VENDOR:
        if ((req->bmRequest & 0x80U) != 0U &&
            req->bRequest == VENDOR_REQ_GET_PRODINFO) {
            uint16_t len = req->wLength;
            if (len > sizeof(s_prodinfo_buf)) {
                len = sizeof(s_prodinfo_buf);
            }
            memcpy(s_prodinfo_buf, (const void *)PRODINFO_FLASH_ADDR,
                   sizeof(s_prodinfo_buf));
            (void)USBD_CtlSendData(pdev, s_prodinfo_buf, len);
        } else {
            USBD_CtlError(pdev, req);
            return (uint8_t)USBD_FAIL;
        }
        break;

    case USB_REQ_TYPE_CLASS:
        ECM_HandleClassReq(pdev, req);
        break;

    case USB_REQ_TYPE_STANDARD:
        switch (req->bRequest) {
        case USB_REQ_GET_INTERFACE:
            if (pdev->dev_state == USBD_STATE_CONFIGURED) {
                uint8_t alt = (req->wIndex == CDC_ECM_DATA_ITF_NBR)
                                  ? s_data_alt : 0U;
                (void)USBD_CtlSendData(pdev, &alt, 1U);
            } else {
                USBD_CtlError(pdev, req);
                return (uint8_t)USBD_FAIL;
            }
            break;

        case USB_REQ_SET_INTERFACE:
            if (pdev->dev_state == USBD_STATE_CONFIGURED) {
                if (req->wIndex == CDC_ECM_DATA_ITF_NBR) {
                    uint8_t alt = (uint8_t)req->wValue;
                    if (alt != s_data_alt) {
                        s_data_alt = alt;
                        if (alt == 1U) {
                            /* Endpoints come up here; the link_task (task
                             * context) sends the ECM link notification once it
                             * sees the interface is ready. */
                            ecm_open_data_eps(pdev);
                        } else {
                            ecm_close_data_eps(pdev);
                        }
                    }
                }
            } else {
                USBD_CtlError(pdev, req);
                return (uint8_t)USBD_FAIL;
            }
            break;

        default:
            USBD_CtlError(pdev, req);
            return (uint8_t)USBD_FAIL;
        }
        break;

    default:
        USBD_CtlError(pdev, req);
        return (uint8_t)USBD_FAIL;
    }
    return (uint8_t)USBD_OK;
}

static uint8_t ECM_EP0_RxReady(USBD_HandleTypeDef *pdev)
{
    (void)pdev;
    /* Class SET request data landed in s_ctl_buf; nothing to apply. */
    return (uint8_t)USBD_OK;
}

static uint8_t ECM_DataIn(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    BaseType_t woken = pdFALSE;

    if (epnum == (CDC_ECM_IN_EP & 0x0FU)) {
        if (s_in_zlp) {
            s_in_zlp = false;
            (void)USBD_LL_Transmit(pdev, CDC_ECM_IN_EP, NULL, 0U);
        } else if (s_in_done != NULL) {
            (void)xSemaphoreGiveFromISR(s_in_done, &woken);
        }
    } else if (epnum == (CDC_ECM_CMD_EP & 0x0FU)) {
        if (s_speed_pending) {
            s_speed_pending = false;
            /* CONNECTION_SPEED_CHANGE: DL/UL bit rate = 10 Mbps. */
            s_notify_buf[0] = 0xA1;
            s_notify_buf[1] = ECM_NOTIFY_CONNECTION_SPEED_CHANGE;
            s_notify_buf[2] = 0x00; s_notify_buf[3] = 0x00; /* wValue */
            s_notify_buf[4] = CDC_ECM_CMD_ITF_NBR; s_notify_buf[5] = 0x00;
            s_notify_buf[6] = 0x08; s_notify_buf[7] = 0x00; /* wLength = 8 */
            uint32_t bps = ECM_LINK_SPEED_BPS;
            memcpy(&s_notify_buf[8], &bps, 4);   /* DL bitrate */
            memcpy(&s_notify_buf[12], &bps, 4);  /* UL bitrate */
            (void)USBD_LL_Transmit(pdev, CDC_ECM_CMD_EP, s_notify_buf, 16U);
        } else {
            s_cmd_busy = false;
        }
    }

    portYIELD_FROM_ISR(woken);
    return (uint8_t)USBD_OK;
}

static uint8_t ECM_DataOut(USBD_HandleTypeDef *pdev, uint8_t epnum)
{
    if (epnum != (CDC_ECM_OUT_EP & 0x0FU) || s_out_frame == NULL) {
        return (uint8_t)USBD_OK;
    }

    BaseType_t woken = pdFALSE;
    uint32_t len = USBD_LL_GetRxDataSize(pdev, CDC_ECM_OUT_EP);

    if (len == 0U || len > NET_MAX_FRAME_LEN) {
        /* Zero-length terminator or oversize: re-arm the same buffer. */
        (void)USBD_LL_PrepareReceive(pdev, CDC_ECM_OUT_EP, s_out_frame->data,
                                     NET_MAX_FRAME_LEN);
        return (uint8_t)USBD_OK;
    }

    /* Grab a fresh buffer first so a submit never orphans the armed one. */
    net_frame_t *next = net_bridge_downlink_alloc_isr(&woken);
    if (next == NULL) {
        /* Pool exhausted: drop this frame, keep using the same buffer. */
        (void)USBD_LL_PrepareReceive(pdev, CDC_ECM_OUT_EP, s_out_frame->data,
                                     NET_MAX_FRAME_LEN);
        portYIELD_FROM_ISR(woken);
        return (uint8_t)USBD_OK;
    }

    s_out_frame->len = (uint16_t)len;
    net_bridge_downlink_submit_isr(s_out_frame, &woken);
    s_out_frame = next;
    (void)USBD_LL_PrepareReceive(pdev, CDC_ECM_OUT_EP, s_out_frame->data,
                                 NET_MAX_FRAME_LEN);

    portYIELD_FROM_ISR(woken);
    return (uint8_t)USBD_OK;
}

static uint8_t *ECM_GetFSCfgDesc(uint16_t *length)
{
    *length = (uint16_t)sizeof(USBD_ECM_CfgDesc);
    return USBD_ECM_CfgDesc;
}

static uint8_t *ECM_GetHSCfgDesc(uint16_t *length)
{
    *length = (uint16_t)sizeof(USBD_ECM_CfgDesc);
    return USBD_ECM_CfgDesc;
}

static uint8_t *ECM_GetOtherSpeedCfgDesc(uint16_t *length)
{
    *length = (uint16_t)sizeof(USBD_ECM_CfgDesc);
    return USBD_ECM_CfgDesc;
}

static uint8_t *ECM_GetDeviceQualifierDesc(uint16_t *length)
{
    *length = (uint16_t)sizeof(USBD_ECM_DeviceQualifierDesc);
    return USBD_ECM_DeviceQualifierDesc;
}

#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
static uint8_t *ECM_GetUsrStrDesc(USBD_HandleTypeDef *pdev, uint8_t index,
                                  uint16_t *length)
{
    static uint8_t str_desc[26];   /* 2 + 12*2 unicode chars */
    static const char hex[] = "0123456789ABCDEF";

    if ((index & 0xFFU) == CDC_ECM_MAC_STRING_INDEX) {
        char ascii[13];
        for (int i = 0; i < 6; i++) {
            ascii[i * 2]     = hex[(s_mac[i] >> 4) & 0x0F];
            ascii[i * 2 + 1] = hex[s_mac[i] & 0x0F];
        }
        ascii[12] = '\0';
        USBD_GetString((uint8_t *)ascii, str_desc, length);
        return str_desc;
    }
    (void)pdev;
    *length = 0;
    return NULL;
}
#endif

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

void USBD_ECM_OsInit(void)
{
    if (s_in_done == NULL) {
        s_in_done = xSemaphoreCreateBinary();
    }
}

void USBD_ECM_SetMacAddress(const uint8_t mac[6])
{
    memcpy(s_mac, mac, 6);
}

bool USBD_ECM_IsReady(void)
{
    return s_ready;
}

int USBD_ECM_SendFrameBlocking(const uint8_t *data, uint16_t len,
                               uint32_t timeout_ms)
{
    if (!s_ready || s_pdev == NULL || s_in_done == NULL ||
        data == NULL || len == 0U) {
        return -1;
    }

    /* Clear any stale completion signal. */
    (void)xSemaphoreTake(s_in_done, 0);

    s_in_zlp = (len % CDC_ECM_DATA_MAX_PACKET_SIZE) == 0U;

    if (USBD_LL_Transmit(s_pdev, CDC_ECM_IN_EP, (uint8_t *)data, len)
            != (uint8_t)USBD_OK) {
        s_in_zlp = false;
        return -1;
    }

    if (xSemaphoreTake(s_in_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        (void)USBD_LL_FlushEP(s_pdev, CDC_ECM_IN_EP);
        s_in_zlp = false;
        return -1;
    }
    return 0;
}

void USBD_ECM_SetLinkState(bool up)
{
    if (s_pdev == NULL) {
        return;
    }

    taskENTER_CRITICAL();
    bool busy = s_cmd_busy;
    if (!busy) {
        s_cmd_busy = true;
        s_speed_pending = up;   /* follow up with speed change when connected */
    }
    taskEXIT_CRITICAL();

    if (busy) {
        /* A notification is already in flight; the DataIn handler will not
         * pick up this change. Link toggles are rare, so simply drop it; the
         * periodic link_task will resend the current state on the next edge. */
        return;
    }

    /* NETWORK_CONNECTION notification (8 bytes). */
    s_notify_buf[0] = 0xA1;
    s_notify_buf[1] = ECM_NOTIFY_NETWORK_CONNECTION;
    s_notify_buf[2] = up ? 0x01 : 0x00; s_notify_buf[3] = 0x00; /* wValue */
    s_notify_buf[4] = CDC_ECM_CMD_ITF_NBR; s_notify_buf[5] = 0x00; /* wIndex */
    s_notify_buf[6] = 0x00; s_notify_buf[7] = 0x00;               /* wLength */

    if (USBD_LL_Transmit(s_pdev, CDC_ECM_CMD_EP, s_notify_buf, 8U)
            != (uint8_t)USBD_OK) {
        s_cmd_busy = false;
        s_speed_pending = false;
    }
}

/* ------------------------------------------------------------------------- */
/* Class object                                                              */
/* ------------------------------------------------------------------------- */

USBD_ClassTypeDef USBD_ECM = {
    ECM_Init,
    ECM_DeInit,
    ECM_Setup,
    NULL,                       /* EP0_TxSent            */
    ECM_EP0_RxReady,
    ECM_DataIn,
    ECM_DataOut,
    NULL,                       /* SOF                   */
    NULL,                       /* IsoINIncomplete       */
    NULL,                       /* IsoOUTIncomplete      */
    ECM_GetHSCfgDesc,
    ECM_GetFSCfgDesc,
    ECM_GetOtherSpeedCfgDesc,
    ECM_GetDeviceQualifierDesc,
#if (USBD_SUPPORT_USER_STRING_DESC == 1U)
    ECM_GetUsrStrDesc,
#endif
};
