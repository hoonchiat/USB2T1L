/**
 * @file    prodinfo.c
 * @brief   Read/validate the production record from reserved flash.
 */
#include "prodinfo.h"

uint32_t prodinfo_crc32(const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool prodinfo_valid(void)
{
    const prodinfo_t *p = PRODINFO;
    if (p->magic != PRODINFO_MAGIC || p->version != PRODINFO_VERSION ||
        p->size != PRODINFO_SIZE) {
        return false;
    }
    return prodinfo_crc32(p, PRODINFO_SIZE - 4u) == p->crc32;
}

const prodinfo_t *prodinfo_get(void)
{
    return prodinfo_valid() ? PRODINFO : NULL;
}
