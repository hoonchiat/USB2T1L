/**
 * @file    usbd_conf.h
 * @brief   Configuration for the ST USB Device Library (OTG FS, device mode).
 */
#ifndef USBD_CONF_H
#define USBD_CONF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32f4xx.h"
#include "stm32f4xx_hal.h"

/* --- Common config -------------------------------------------------------- */
#define USBD_MAX_NUM_INTERFACES        2U
#define USBD_MAX_NUM_CONFIGURATION     1U
#define USBD_MAX_STR_DESC_SIZ          512U
#define USBD_DEBUG_LEVEL               0U
#define USBD_LPM_ENABLED               0U
#define USBD_SELF_POWERED              0U   /* bus-powered (matches cfg desc) */
#define USBD_SUPPORT_USER_STRING_DESC  1U   /* needed for the MAC-address str  */

/* --- Memory management: the ECM class keeps its state statically, so the
 * core never actually allocates class data; provide a tiny static pool. ---- */
#define USBD_malloc                    (uint32_t *)USBD_static_malloc
#define USBD_free                      USBD_static_free
#define USBD_memset                    memset
#define USBD_memcpy                    memcpy
#define USBD_Delay                     HAL_Delay

void *USBD_static_malloc(uint32_t size);
void  USBD_static_free(void *p);

/* --- Trace/log (disabled) ------------------------------------------------- */
#if (USBD_DEBUG_LEVEL > 0U)
#define USBD_UsrLog(...)   do { printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBD_UsrLog(...)   do {} while (0)
#endif
#if (USBD_DEBUG_LEVEL > 1U)
#define USBD_ErrLog(...)   do { printf("ERROR: "); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBD_ErrLog(...)   do {} while (0)
#endif
#if (USBD_DEBUG_LEVEL > 2U)
#define USBD_DbgLog(...)   do { printf("DEBUG : "); printf(__VA_ARGS__); printf("\n"); } while (0)
#else
#define USBD_DbgLog(...)   do {} while (0)
#endif

#endif /* USBD_CONF_H */
