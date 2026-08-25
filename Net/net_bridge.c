/**
 * @file    net_bridge.c
 * @brief   ADIN2111 <-> USB CDC-ECM frame bridge with FreeRTOS tasks.
 *
 * The ADIN2111 driver core keeps scratch buffers and shares one SPI bus, so
 * every access is serialised with s_adin_lock. USB (OTG) accesses are on a
 * separate peripheral and need no shared lock with SPI.
 */
#include "net_bridge.h"

#include "adin2111.h"
#include "adin2111_port_stm32.h"
#include "usbd_ecm_if.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

static adin2111_t          s_adin;

static frame_pool_t        s_rx_pool;   /* ADIN -> USB (uplink)    */
static frame_pool_t        s_tx_pool;   /* USB  -> ADIN (downlink) */
static net_frame_t         s_rx_storage[NET_RX_POOL_COUNT];
static net_frame_t         s_tx_storage[NET_TX_POOL_COUNT];

static QueueHandle_t       s_to_adin_q;    /* net_frame_t* to send to ADIN   */
static SemaphoreHandle_t   s_adin_int_sem; /* given by EXTI ISR              */
static SemaphoreHandle_t   s_adin_lock;    /* serialises ADIN/SPI access     */

static volatile bool       s_link_up;

#define ADIN_LOCK()    (void)xSemaphoreTake(s_adin_lock, portMAX_DELAY)
#define ADIN_UNLOCK()  (void)xSemaphoreGive(s_adin_lock)

/* ------------------------------------------------------------------------- */
/* Uplink: ADIN -> USB                                                       */
/* ------------------------------------------------------------------------- */

static void net_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* Block until the ADIN asserts INT (RX_RDY / error). The timeout is a
         * safety poll in case a (level-triggered) edge is ever missed. */
        (void)xSemaphoreTake(s_adin_int_sem, pdMS_TO_TICKS(50));

        ADIN_LOCK();
        (void)adin2111_irq_ack(&s_adin, NULL);
        ADIN_UNLOCK();

        /* Drain every frame currently in the ADIN RX FIFO(s). */
        for (;;) {
            ADIN_LOCK();
            if (!adin2111_rx_ready(&s_adin)) {
                ADIN_UNLOCK();
                break;
            }
            net_frame_t *f = frame_pool_alloc(&s_rx_pool, 0);
            if (f == NULL) {
                /* No buffer free: leave the frame in the ADIN FIFO and retry
                 * on the next wake once the USB side drains a buffer. */
                ADIN_UNLOCK();
                break;
            }
            uint16_t len = 0;
            adin2111_port_t port = ADIN2111_PORT_1;
            adin2111_status_t rc =
                adin2111_read_frame(&s_adin, f->data, sizeof(f->data),
                                    &len, &port);
            ADIN_UNLOCK();

            if (rc != ADIN2111_OK || len == 0u) {
                frame_pool_free(&s_rx_pool, f);
                if (rc == ADIN2111_ERR_NODATA) {
                    break;
                }
                continue;
            }
            f->len  = len;
            f->port = (uint8_t)port;

            bsp_led_activity();

            /* Blocking transmit to the host; the class borrows f until done. */
            (void)usbd_ecm_if_send(f);
            frame_pool_free(&s_rx_pool, f);
        }
    }
}

void net_bridge_adin_int_isr(void)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_adin_int_sem, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ------------------------------------------------------------------------- */
/* Downlink: USB -> ADIN                                                     */
/* ------------------------------------------------------------------------- */

net_frame_t *net_bridge_downlink_alloc(void)
{
    return frame_pool_alloc(&s_tx_pool, 0);
}

net_frame_t *net_bridge_downlink_alloc_isr(BaseType_t *woken)
{
    return frame_pool_alloc_isr(&s_tx_pool, woken);
}

void net_bridge_downlink_submit_isr(net_frame_t *frame, BaseType_t *woken)
{
    if (xQueueSendFromISR(s_to_adin_q, &frame, woken) != pdTRUE) {
        /* Queue full: recycle the buffer, dropping the frame. */
        frame_pool_free_isr(&s_tx_pool, frame, woken);
    }
}

void net_bridge_downlink_discard_isr(net_frame_t *frame, BaseType_t *woken)
{
    frame_pool_free_isr(&s_tx_pool, frame, woken);
}

static void net_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_frame_t *f = NULL;
        if (xQueueReceive(s_to_adin_q, &f, portMAX_DELAY) != pdTRUE ||
            f == NULL) {
            continue;
        }
        bsp_led_activity();

        /* Host frames egress on port 1. Retry briefly if the TX FIFO is full. */
        adin2111_status_t rc = ADIN2111_ERR_NOSPACE;
        for (int attempt = 0; attempt < 4; attempt++) {
            ADIN_LOCK();
            rc = adin2111_write_frame(&s_adin, ADIN2111_PORT_1, f->data, f->len);
            ADIN_UNLOCK();
            if (rc != ADIN2111_ERR_NOSPACE) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        frame_pool_free(&s_tx_pool, f);
    }
}

/* ------------------------------------------------------------------------- */
/* Link management                                                           */
/* ------------------------------------------------------------------------- */

static void link_task(void *arg)
{
    (void)arg;
    bool last_sent = false;
    bool have_sent = false;

    for (;;) {
        ADIN_LOCK();
        bool up = adin2111_link_up(&s_adin, ADIN2111_PORT_1);
        if (s_adin.num_ports > 1) {
            up = up || adin2111_link_up(&s_adin, ADIN2111_PORT_2);
        }
        ADIN_UNLOCK();

        if (up != s_link_up) {
            s_link_up = up;
            bsp_led_link(up);
        }

        /* (Re)notify the host whenever it is ready or the link state changes.
         * Runs in task context, so USBD_ECM_SetLinkState's critical section is
         * valid here. */
        bool ready = usbd_ecm_if_ready();
        if (!ready) {
            have_sent = false;
        } else if (!have_sent || up != last_sent) {
            usbd_ecm_if_set_link_state(up);
            last_sent = up;
            have_sent = true;
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* ------------------------------------------------------------------------- */
/* Bring-up                                                                   */
/* ------------------------------------------------------------------------- */

bool net_bridge_init(void)
{
    if (!frame_pool_init(&s_rx_pool, s_rx_storage, NET_RX_POOL_COUNT)) {
        return false;
    }
    if (!frame_pool_init(&s_tx_pool, s_tx_storage, NET_TX_POOL_COUNT)) {
        return false;
    }

    s_to_adin_q    = xQueueCreate(NET_TX_POOL_COUNT, sizeof(net_frame_t *));
    s_adin_int_sem = xSemaphoreCreateBinary();
    s_adin_lock    = xSemaphoreCreateMutex();
    if (s_to_adin_q == NULL || s_adin_int_sem == NULL || s_adin_lock == NULL) {
        return false;
    }

    /* Bring up the ADIN2111 over SPI (no concurrency yet: tasks not created). */
    adin2111_hal_t hal;
    adin2111_port_stm32_get_hal(&hal);
    if (adin2111_init(&s_adin, &hal, (ADIN_SPI_USE_CRC != 0)) != ADIN2111_OK) {
        return false;
    }

    uint8_t mac[6];
    bsp_get_mac_address(mac);
    (void)adin2111_set_host_mac(&s_adin, mac);

    /* Register + start the USB ECM class (creates the OS objects it needs). */
    usbd_ecm_if_register();

    BaseType_t ok = pdPASS;
    ok &= xTaskCreate(net_rx_task, "net_rx", TASK_STACK_NET_RX, NULL,
                      TASK_PRIO_NET_RX, NULL);
    ok &= xTaskCreate(net_tx_task, "net_tx", TASK_STACK_NET_TX, NULL,
                      TASK_PRIO_NET_TX, NULL);
    ok &= xTaskCreate(link_task, "link", TASK_STACK_LINK, NULL,
                      TASK_PRIO_LINK, NULL);
    return ok == pdPASS;
}
