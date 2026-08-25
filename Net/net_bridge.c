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
#include "adin2111_regs.h"   /* ADIN_MAC_SLOT_FDB_BASE for the learning table */
#include "adin2111_port_stm32.h"
#include "usbd_ecm_if.h"
#include "bsp.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* Module state                                                              */
/* ------------------------------------------------------------------------- */

static adin2111_t          s_adin;
static uint8_t             s_own_mac[6];

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
/* Switch-mode learning FDB (host-managed ADIN hardware forwarding table)    */
/*                                                                           */
/* The ADIN2111 does not auto-learn: unicast is forwarded port-to-port only  */
/* for destinations present in its filter table. We learn source addresses   */
/* from the frames the host does see (broadcast/multicast + frames for us)   */
/* and program a hardware forwarding entry so later unicast to that node is   */
/* switched without host involvement. Bounded by the 14 free filter slots.   */
/* s_fdb and the ADIN table are both protected by s_adin_lock.               */
/* ------------------------------------------------------------------------- */
#if (ADIN_DAISY_CHAIN_MODE)
_Static_assert(NET_FDB_MAX_ENTRIES <=
                   (ADIN_MAC_FILTER_SLOTS - ADIN_MAC_SLOT_FDB_BASE),
               "NET_FDB_MAX_ENTRIES exceeds the free ADIN filter slots");

typedef struct {
    uint8_t  mac[6];
    uint8_t  port;    /* live port where this MAC was last seen (ingress)     */
    bool     valid;
    uint32_t stamp;   /* tick of last refresh, for aging / LRU eviction       */
} fdb_entry_t;

static fdb_entry_t s_fdb[NET_FDB_MAX_ENTRIES];

static inline bool mac_is_group(const uint8_t *m)
{
    return (m[0] & 0x01u) != 0u;   /* broadcast/multicast group bit */
}

/* Learn src@ingress into the FDB and the ADIN table. Caller holds s_adin_lock. */
static void net_bridge_learn(const uint8_t *src, adin2111_port_t ingress)
{
    if (mac_is_group(src) || memcmp(src, s_own_mac, 6) == 0) {
        return;   /* never learn a group address or our own */
    }
    const uint32_t now  = (uint32_t)xTaskGetTickCount();
    const uint32_t agel = pdMS_TO_TICKS(NET_FDB_AGE_MS);

    /* Pass 1: refresh an existing entry (and reprogram if the node moved). */
    for (uint32_t i = 0; i < NET_FDB_MAX_ENTRIES; i++) {
        if (s_fdb[i].valid && memcmp(s_fdb[i].mac, src, 6) == 0) {
            s_fdb[i].stamp = now;
            if (s_fdb[i].port != (uint8_t)ingress) {
                s_fdb[i].port = (uint8_t)ingress;
                (void)adin2111_fdb_set(&s_adin,
                        (uint8_t)(ADIN_MAC_SLOT_FDB_BASE + i), src, ingress);
            }
            return;
        }
    }

    /* Pass 2: choose a slot — a free one, else an aged-out one, else LRU. */
    int slot = -1;
    int lru = 0;
    uint32_t lru_age = 0;
    for (uint32_t i = 0; i < NET_FDB_MAX_ENTRIES; i++) {
        if (!s_fdb[i].valid) {
            slot = (int)i;
            break;
        }
        uint32_t age = now - s_fdb[i].stamp;
        if (age > agel) {
            slot = (int)i;   /* stale: reclaim */
            break;
        }
        if (age >= lru_age) {
            lru_age = age;
            lru = (int)i;
        }
    }
    if (slot < 0) {
        slot = lru;   /* table full of fresh entries: evict least-recent */
    }

    memcpy(s_fdb[slot].mac, src, 6);
    s_fdb[slot].port  = (uint8_t)ingress;
    s_fdb[slot].valid = true;
    s_fdb[slot].stamp = now;
    (void)adin2111_fdb_set(&s_adin,
            (uint8_t)(ADIN_MAC_SLOT_FDB_BASE + slot), src, ingress);
}

/* Look up a destination MAC -> live port, or -1 if unknown. Caller holds lock. */
static int net_bridge_fdb_lookup(const uint8_t *dst)
{
    for (uint32_t i = 0; i < NET_FDB_MAX_ENTRIES; i++) {
        if (s_fdb[i].valid && memcmp(s_fdb[i].mac, dst, 6) == 0) {
            return (int)s_fdb[i].port;
        }
    }
    return -1;
}
#endif /* ADIN_DAISY_CHAIN_MODE */

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
#if (ADIN_DAISY_CHAIN_MODE)
            /* Learn the source of every frame we see (still under the lock, as
             * this programs the ADIN table). len >= 12 => dst+src present. */
            if (rc == ADIN2111_OK && len >= 12u &&
                s_adin.mode == ADIN2111_MODE_SWITCH) {
                net_bridge_learn(&f->data[6], port);
            }
#endif
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

/* Write one frame out a specific port, retrying briefly if the TX FIFO is full. */
static void tx_frame_port(net_frame_t *f, adin2111_port_t port)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        ADIN_LOCK();
        adin2111_status_t rc =
            adin2111_write_frame(&s_adin, port, f->data, f->len);
        ADIN_UNLOCK();
        if (rc != ADIN2111_ERR_NOSPACE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
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

#if (ADIN_DAISY_CHAIN_MODE)
        if (s_adin.mode == ADIN2111_MODE_SWITCH) {
            /* Choose the egress port for host-injected traffic:
             *  - broadcast/multicast, or unknown unicast -> flood both ports
             *    (so it reaches either direction of the chain);
             *  - learned unicast -> the single port toward that node. */
            int p = -1;
            if (!mac_is_group(f->data)) {
                ADIN_LOCK();
                p = net_bridge_fdb_lookup(f->data);   /* dst MAC = f->data[0..5] */
                ADIN_UNLOCK();
            }
            if (p < 0) {
                tx_frame_port(f, ADIN2111_PORT_1);
                tx_frame_port(f, ADIN2111_PORT_2);
            } else {
                tx_frame_port(f, (adin2111_port_t)p);
            }
            frame_pool_free(&s_tx_pool, f);
            continue;
        }
#endif
        /* Endpoint mode: single-port egress. */
        tx_frame_port(f, ADIN2111_PORT_1);
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

#if (ADIN_DAISY_CHAIN_MODE)
        /* Age out stale learned entries so their hardware forwarding slot is
         * released and traffic isn't sent toward a node that has gone away. */
        if (s_adin.mode == ADIN2111_MODE_SWITCH) {
            uint32_t now  = (uint32_t)xTaskGetTickCount();
            uint32_t agel = pdMS_TO_TICKS(NET_FDB_AGE_MS);
            ADIN_LOCK();
            for (uint32_t i = 0; i < NET_FDB_MAX_ENTRIES; i++) {
                if (s_fdb[i].valid && (now - s_fdb[i].stamp) > agel) {
                    s_fdb[i].valid = false;
                    (void)adin2111_fdb_clear(&s_adin,
                            (uint8_t)(ADIN_MAC_SLOT_FDB_BASE + i));
                }
            }
            ADIN_UNLOCK();
        }
#endif

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
    adin2111_mode_t mode = (ADIN_DAISY_CHAIN_MODE != 0) ? ADIN2111_MODE_SWITCH
                                                        : ADIN2111_MODE_ENDPOINT;
    if (adin2111_init(&s_adin, &hal, (ADIN_SPI_USE_CRC != 0), mode)
            != ADIN2111_OK) {
        return false;
    }

    bsp_get_mac_address(s_own_mac);
    (void)adin2111_set_host_mac(&s_adin, s_own_mac);

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
