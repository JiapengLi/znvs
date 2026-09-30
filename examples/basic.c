/* Complete host demo; replace only the three callbacks on an MCU.
 * The RAM-backed flash below belongs to the DEMO, never to the library.
 * SPDX-License-Identifier: Apache-2.0 */
#include "znvs.h"
#include <stdio.h>
#include <string.h>

#define FLASH_BYTES 8192U
#define ERASE_BYTES 4096U
#define WRITE_BYTES 4U
static unsigned char flash[FLASH_BYTES];

static int read_flash(void *arg, uint32_t off, void *buf, size_t len)
{
    if (off > FLASH_BYTES || len > FLASH_BYTES - off)
        return ZNVS_EIO;
    memcpy(buf, (unsigned char *)arg + off, len);
    return ZNVS_OK;
}
static int write_flash(void *arg, uint32_t off, const void *buf, size_t len)
{
    unsigned char *dst = arg;
    const unsigned char *src = buf;
    size_t i;
    if (off > FLASH_BYTES || len > FLASH_BYTES - off ||
        off % WRITE_BYTES || len % WRITE_BYTES)
        return ZNVS_EIO;
    for (i = 0; i < len; ++i)
        if ((dst[off + i] & src[i]) != src[i])
            return ZNVS_EIO;
    for (i = 0; i < len; ++i)
        dst[off + i] &= src[i];
    return ZNVS_OK;
}
static int erase_flash(void *arg, uint32_t off, size_t len)
{
    if (off > FLASH_BYTES || len > FLASH_BYTES - off ||
        off % ERASE_BYTES || len % ERASE_BYTES)
        return ZNVS_EIO;
    memset((unsigned char *)arg + off, 0xff, len);
    return ZNVS_OK;
}
int main(void)
{
    static const znvs_cfg_t cfg = {
        read_flash, write_flash, erase_flash,
        FLASH_BYTES, ERASE_BYTES, ERASE_BYTES, WRITE_BYTES
    };
    static const char value[] = "hello znvs";
    znvs_t fs;
    char restored[32];
    size_t length;
    int rc;

    /* Start this DEMO with erased flash. Real hardware keeps its contents. */
    memset(flash, 0xff, sizeof(flash));
    rc = znvs_init(&fs, &cfg, flash);
    if (rc != ZNVS_OK) {
        fprintf(stderr, "mount failed: %d (not formatting automatically)\n", rc);
        return 1;
    }
    rc = znvs_write(&fs, 1, value, sizeof(value));
    if (rc != ZNVS_OK)
        return 2;
    /* Equal data is a successful no-op; the stored payload CRC is also checked. */
    if (znvs_write(&fs, 1, value, sizeof(value)) != ZNVS_OK)
        return 3;
    /* Lose all in-RAM state: reconstruct it from flash, as after reboot. */
    if (znvs_init(&fs, &cfg, flash) != ZNVS_OK)
        return 4;
    rc = znvs_read(&fs, 1, restored, sizeof(restored), &length);
    if (rc != ZNVS_OK || length != sizeof(value) ||
        memcmp(restored, value, sizeof(value)) != 0)
        return 5;
    printf("read: %s (%zu bytes including NUL)\n", restored, length);
    printf("instance: %zu bytes, config: %zu bytes\n", sizeof(fs), sizeof(cfg));
    return znvs_delete(&fs, 1) == ZNVS_OK ? 0 : 6;
}
