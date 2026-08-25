/**
 * @file    frame_pool.c
 * @brief   Implementation of the fixed-size frame buffer pool.
 */
#include "frame_pool.h"

bool frame_pool_init(frame_pool_t *pool, net_frame_t *storage, uint32_t count)
{
    if (pool == NULL || storage == NULL || count == 0u) {
        return false;
    }
    pool->storage = storage;
    pool->count   = count;
    pool->free_q  = xQueueCreate(count, sizeof(net_frame_t *));
    if (pool->free_q == NULL) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        net_frame_t *p = &storage[i];
        p->len  = 0;
        p->port = 0;
        (void)xQueueSend(pool->free_q, &p, 0);
    }
    return true;
}

net_frame_t *frame_pool_alloc(frame_pool_t *pool, TickType_t ticks)
{
    net_frame_t *p = NULL;
    if (xQueueReceive(pool->free_q, &p, ticks) != pdTRUE) {
        return NULL;
    }
    return p;
}

net_frame_t *frame_pool_alloc_isr(frame_pool_t *pool,
                                  BaseType_t *higher_prio_woken)
{
    net_frame_t *p = NULL;
    if (xQueueReceiveFromISR(pool->free_q, &p, higher_prio_woken) != pdTRUE) {
        return NULL;
    }
    return p;
}

void frame_pool_free(frame_pool_t *pool, net_frame_t *frame)
{
    if (frame == NULL) {
        return;
    }
    (void)xQueueSend(pool->free_q, &frame, 0);
}

void frame_pool_free_isr(frame_pool_t *pool, net_frame_t *frame,
                         BaseType_t *higher_prio_woken)
{
    if (frame == NULL) {
        return;
    }
    (void)xQueueSendFromISR(pool->free_q, &frame, higher_prio_woken);
}
