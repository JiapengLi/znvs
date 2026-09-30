/* Logical-value comparison with upstream; foreign media must remain unchanged.
 * SPDX-License-Identifier: Apache-2.0 */
#include "znvs.h"
#include "ref_shim.h"
#include <stdio.h>
#include <stdlib.h>
#define FLASH_SIZE 4096U
#define REQUIRE(x)                                                         \
    do {                                                                   \
        if (!(x)) {                                                        \
            fprintf(stderr, "reference FAIL line %d: %s\n", __LINE__, #x); \
            exit(1);                                                       \
        }                                                                  \
    } while (0)
static uint8_t ours[FLASH_SIZE], upstream[FLASH_SIZE], cross1[FLASH_SIZE], cross2[FLASH_SIZE];
static uint16_t wbs;
static int rd(void *a, uint32_t off, void *p, size_t n)
{
    REQUIRE(off <= FLASH_SIZE && n <= FLASH_SIZE - off);
    memcpy(p, (uint8_t *)a + off, n);
    return 0;
}
static int wr(void *a, uint32_t off, const void *p, size_t n)
{
    uint8_t *d = a;
    const uint8_t *s = p;
    size_t i;
    REQUIRE(off <= FLASH_SIZE && n <= FLASH_SIZE - off && off % wbs == 0 && n % wbs == 0);
    for (i = 0; i < n; ++i) {
        REQUIRE((d[off + i] & s[i]) == s[i]);
        d[off + i] &= s[i];
    }
    return 0;
}
static int er(void *a, uint32_t off, size_t n)
{
    REQUIRE(off <= FLASH_SIZE && n <= FLASH_SIZE - off && off % 512 == 0 && n % 512 == 0);
    memset((uint8_t *)a + off, 0xff, n);
    return 0;
}
static void compare_reads(znvs_t *a, struct nvs_fs *b)
{
    uint16_t id;
    for (id = 0; id < 12; ++id) {
        uint8_t va[80], vb[80];
        size_t len;
        int ra = znvs_read(a, id, va, sizeof(va), &len);
        ssize_t rb = nvs_read(b, id, vb, sizeof(vb));
        if (ra == ZNVS_ENOENT) {
            REQUIRE(rb == -ENOENT);
        } else {
            REQUIRE(ra == 0 && rb >= 0 && len == (size_t)rb);
            REQUIRE(memcmp(va, vb, len) == 0);
        }
    }
}
static uint32_t next(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return *s;
}
int main(void)
{
    unsigned comparisons = 0, cross_mounts = 0;
    for (wbs = 1; wbs <= 32; wbs *= 2) {
        const znvs_cfg_t cfg = {rd, wr, er, FLASH_SIZE, 512, wbs};
        struct device dev = {upstream, rd, wr, er, {wbs, 0xff}, 512};
        struct nvs_fs ref = {0};
        znvs_t fs;
        uint32_t seed = 0x895241baU;
        unsigned step;
        memset(ours, 0xff, sizeof(ours));
        memset(upstream, 0xff, sizeof(upstream));
        ref.flash_device = &dev;
        ref.sector_size = 1024;
        ref.sector_count = 4;
        REQUIRE(nvs_mount(&ref) == 0 && znvs_init(&fs, &cfg, ours) == 0);
        for (step = 0; step < 1200; ++step) {
            uint8_t value[64];
            uint16_t id = (uint16_t)(next(&seed) % 12);
            size_t len = next(&seed) % 64, i;
            int a;
            ssize_t b;
            for (i = 0; i < len; ++i) {
                value[i] = (uint8_t)next(&seed);
            }
            a = znvs_write(&fs, id, value, len);
            b = nvs_write(&ref, id, value, len);
            REQUIRE((a == 0 && b >= 0) || (a == ZNVS_ENOSPC && b == -ENOSPC));
            compare_reads(&fs, &ref);
            ++comparisons;
            if (step % 37 == 0) {
                znvs_t cross_fs;
                memcpy(cross1, ours, sizeof(ours));
                memcpy(cross2, upstream, sizeof(upstream));
                REQUIRE(znvs_init(&cross_fs, &cfg, cross1) == 0);
                compare_reads(&cross_fs, &ref);
                REQUIRE(znvs_init(&cross_fs, &cfg, cross2) == ZNVS_ECORRUPT);
                REQUIRE(memcmp(cross2, upstream, sizeof(upstream)) == 0);
                cross_mounts += 2;
            }
        }
    }
    printf("PASS upstream v4.4.2 host-adapted CRC32: %u logical-value comparisons, %u remount/foreign-format rejection checks\n", comparisons, cross_mounts);
    return 0;
}
