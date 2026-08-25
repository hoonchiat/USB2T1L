/**
 * @file    frame_pool.h
 * @brief   Fixed-size Ethernet frame buffer pool, safe from tasks and ISRs.
 *
 * A pool owns a static array of @ref net_frame_t and a FreeRTOS queue that
 * holds pointers to the currently free frames. alloc pops a free buffer,
 * free pushes it back. Passing frames between producer and consumer is done
 * with a separate "ready" queue of net_frame_t* (see net_bridge).
 */
#ifndef FRAME_POOL_H
#define FRAME_POOL_H

#include <stdint.h>
#include <stdbool.h>

#include "app_config.h"
#include "FreeRTOS.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One Ethernet frame (no FCS). @p data is 4-byte aligned for SPI bursts. */
typedef struct {
    uint16_t len;                          /**< valid bytes in @p data       */
    uint8_t  port;                         /**< ADIN ingress/egress port     */
    uint8_t  _pad;
    uint8_t  data[NET_MAX_FRAME_LEN] __attribute__((aligned(4)));
} net_frame_t;

typedef struct {
    QueueHandle_t  free_q;   /**< queue of net_frame_t* that are free        */
    net_frame_t   *storage;  /**< caller-provided backing array             */
    uint32_t       count;
} frame_pool_t;

/**
 * Initialise a pool over a caller-provided array of @p count frames.
 * @return true on success (queue created and filled).
 */
bool frame_pool_init(frame_pool_t *pool, net_frame_t *storage, uint32_t count);

/** Allocate a frame, blocking up to @p ticks. Returns NULL on timeout. */
net_frame_t *frame_pool_alloc(frame_pool_t *pool, TickType_t ticks);

/** Allocate a frame from ISR context. Returns NULL if none free. */
net_frame_t *frame_pool_alloc_isr(frame_pool_t *pool,
                                  BaseType_t *higher_prio_woken);

/** Return a frame to the pool (task context). */
void frame_pool_free(frame_pool_t *pool, net_frame_t *frame);

/** Return a frame to the pool (ISR context). */
void frame_pool_free_isr(frame_pool_t *pool, net_frame_t *frame,
                         BaseType_t *higher_prio_woken);

#ifdef __cplusplus
}
#endif

#endif /* FRAME_POOL_H */
