/**
 * @file    prodinfo.h
 * @brief   Production / provisioning record stored in a reserved STM32 flash
 *          sector (MAC address, serial number, batch, firmware version, ...).
 *
 * The record lives in the last flash sector (sector 11, 128 KB @ 0x080E0000)
 * so the application (which the linker caps at 896 KB) never overwrites it and
 * a firmware update (sector-erase of the low sectors) leaves it intact. It is
 * written out-of-band by the production tool over SWD (see tools/prodinfo.py),
 * not by the firmware, so no in-application flash programming is required.
 *
 * Integrity is a CRC-32 (IEEE 802.3 / zlib) over the first 124 bytes, so the
 * host tool (`zlib.crc32`) and the firmware agree bit-for-bit.
 */
#ifndef PRODINFO_H
#define PRODINFO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Reserved sector: STM32F407 sector 11 (128 KB). Relocate here + in the linker
 * script (FLASH length and the PRODINFO region) if you need a different one. */
#define PRODINFO_FLASH_ADDR   0x080E0000U
#define PRODINFO_MAGIC        0x444F5250U   /* bytes 'P','R','O','D'          */
#define PRODINFO_VERSION      1U
#define PRODINFO_SIZE         128U

/* Fixed 128-byte layout. Strings are ASCII, NUL-padded, and not required to be
 * NUL-terminated when they fill the field. Reserved bytes allow forward-
 * compatible additions without moving existing fields. */
typedef struct __attribute__((packed)) {
    uint32_t magic;          /* PRODINFO_MAGIC                                */
    uint8_t  version;        /* layout version (PRODINFO_VERSION)            */
    uint8_t  flags;          /* reserved, 0                                  */
    uint16_t size;           /* sizeof(prodinfo_t) == PRODINFO_SIZE          */
    uint8_t  mac[6];         /* device MAC address                          */
    uint8_t  rsv0[2];
    char     serial[24];     /* serial number                              */
    char     batch[16];      /* production batch number                    */
    char     fw_version[16]; /* firmware version programmed at production   */
    char     hw_rev[8];      /* hardware revision                          */
    char     prod_date[12];  /* production date, e.g. "2026-08-25"          */
    uint8_t  rsv1[32];       /* reserved for future fields                  */
    uint32_t crc32;          /* CRC-32/IEEE over the first 124 bytes        */
} prodinfo_t;

#ifndef PRODINFO_HOST_TEST
_Static_assert(sizeof(prodinfo_t) == PRODINFO_SIZE,
               "prodinfo_t must be exactly 128 bytes");
#endif

/** Pointer to the in-flash record (contents may be erased/invalid). */
#define PRODINFO ((const prodinfo_t *)PRODINFO_FLASH_ADDR)

#ifdef __cplusplus
extern "C" {
#endif

/** CRC-32 (IEEE 802.3, reflected, init 0xFFFFFFFF, xorout 0xFFFFFFFF) — the
 *  algorithm used by Python's zlib.crc32. */
uint32_t prodinfo_crc32(const void *data, uint32_t len);

/** @return true if the flash record has a valid magic, version and CRC. */
bool prodinfo_valid(void);

/** @return the validated flash record, or NULL if it is missing/corrupt. */
const prodinfo_t *prodinfo_get(void);

#ifdef __cplusplus
}
#endif

#endif /* PRODINFO_H */
