/* TEST ONLY: a host adapter, not the Zephyr kernel/drivers.
 * Used to execute the pinned, unmodified upstream NVS algorithm independently.
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef ZNVS_REF_SHIM_H
#define ZNVS_REF_SHIM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#define __packed __attribute__((packed))
#define KB(n) ((size_t)(n) * 1024U)
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ZTESTABLE_STATIC static
#define BUILD_ASSERT(c,m) _Static_assert(c,m)
#define LOG_MODULE_REGISTER(...) typedef int nvs_reference_log_disabled
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define K_FOREVER 0
#ifdef CONFIG_NVS_DATA_CRC
#define IS_ENABLED(x) 1
#else
#define IS_ENABLED(x) 0
#endif
struct k_mutex { int unused; };
static inline void k_mutex_init(struct k_mutex *m) { (void)m; }
static inline void k_mutex_lock(struct k_mutex *m, int t) { (void)m; (void)t; }
static inline void k_mutex_unlock(struct k_mutex *m) { (void)m; }
struct flash_parameters { size_t write_block_size; uint8_t erase_value; };
struct flash_pages_info { size_t size; };
struct device {
    void *arg;
    int (*read)(void *, uint32_t, void *, size_t);
    int (*write)(void *, uint32_t, const void *, size_t);
    int (*erase)(void *, uint32_t, size_t);
    struct flash_parameters params;
    size_t erase_size;
};
static inline int flash_read(const struct device *d, off_t o, void *p, size_t n)
{ return d->read(d->arg, (uint32_t)o, p, n); }
static inline int flash_write(const struct device *d, off_t o, const void *p, size_t n)
{ return d->write(d->arg, (uint32_t)o, p, n); }
static inline int flash_flatten(const struct device *d, off_t o, size_t n)
{ return d->erase(d->arg, (uint32_t)o, n); }
static inline const struct flash_parameters *flash_get_parameters(const struct device *d)
{ return &d->params; }
static inline size_t flash_get_write_block_size(const struct device *d)
{ return d->params.write_block_size; }
static inline int flash_get_page_info_by_offs(const struct device *d, off_t o,
                                             struct flash_pages_info *i)
{ (void)o; i->size = d->erase_size; return 0; }
/* Deliberately independent, bitwise CRC implementations for comparison. */
static inline uint8_t crc8_ccitt(uint8_t crc, const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n--) {
        unsigned bit;
        crc ^= *p++;
        for (bit = 0; bit < 8; ++bit)
            crc = (uint8_t)((crc << 1) ^ ((crc & 0x80) ? 0x07 : 0));
    }
    return crc;
}
static inline uint32_t crc32_ieee(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (n--) {
        unsigned bit;
        crc ^= *p++;
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}
/* Same upstream field types; only mutex/device are test shims. */
struct nvs_fs {
    off_t offset;
    uint32_t ate_wra, data_wra, sector_size;
    uint16_t sector_count;
    bool ready;
    struct k_mutex nvs_lock;
    const struct device *flash_device;
    const struct flash_parameters *flash_parameters;
};
int nvs_mount(struct nvs_fs *fs);
ssize_t nvs_write(struct nvs_fs *fs, uint16_t id, const void *data, size_t len);
ssize_t nvs_read(struct nvs_fs *fs, uint16_t id, void *data, size_t len);
#endif
