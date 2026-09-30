/*  NVS: non volatile storage in flash
 *
 * Copyright (c) 2018 Laczen
 * Copyright (c) 2026 Lingao Meng
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone extraction, modified 2026-09-30. Based on Zephyr v4.4.2,
 * subsys/kvss/nvs/nvs.c, blob fd6dd480d5712c08f5275830fb21e5b05f25df38.
 * Changes: no Zephyr/OS/heap dependencies; callback I/O; zat-style API;
 * explicit LE encoding; stricter errors/bounds; linear orphan-data scan.
 * See NOTICE for provenance and README.md for the flash/port contract.
 */
#include "znvs.h"
#include <stdbool.h>
#include <string.h>

#define ADDR_SECT_MASK UINT32_C(0xffff0000)
#define ADDR_SECT_SHIFT 16
#define ADDR_OFFS_MASK UINT32_C(0x0000ffff)
#define NVS_LOOKUP_CACHE_NO_ADDR UINT32_MAX
#define NVS_DATA_CRC_SIZE (ZNVS_DATA_CRC ? 4U : 0U)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct nvs_ate {
    uint16_t id;
    uint16_t offset;
    uint16_t len;
    uint8_t part;
    uint8_t crc8;
};
/* All on-flash access is explicit LE; never cast a flash pointer to this type. */
typedef char nvs_ate_size_check[(sizeof(struct nvs_ate) == 8) ? 1 : -1];

struct nvs_block_move_ctx {
    uint8_t buffer[ZNVS_IO_SIZE];
    size_t buffer_pos;
};
struct nvs_flash_buf {
    const uint8_t *ptr;
    size_t len;
};
struct nvs_flash_wrt_stream {
    struct nvs_flash_buf head, data, tail;
};
struct nvs_gc_write_entry {
    uint16_t id;
    const void *data;
    uint16_t len;
    bool is_written;
};

static uint16_t nvs_get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void nvs_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static void nvs_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
#if ZNVS_DATA_CRC
static uint32_t nvs_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
#endif
static void nvs_ate_encode(const struct nvs_ate *ate, uint8_t raw[8])
{
    nvs_put16(raw, ate->id);
    nvs_put16(raw + 2, ate->offset);
    nvs_put16(raw + 4, ate->len);
    raw[6] = ate->part;
    raw[7] = ate->crc8;
}

/* Nibble tables live in read-only storage, not per-instance RAM. */
static uint8_t nvs_crc8(const uint8_t *p, size_t n)
{
    static const uint8_t table[16] = {
        0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15,
        0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d
    };
    uint8_t crc = 0xff;
    while (n--) {
        crc ^= *p++;
        crc = (uint8_t)((crc << 4) ^ table[crc >> 4]);
        crc = (uint8_t)((crc << 4) ^ table[crc >> 4]);
    }
    return crc;
}
static uint32_t nvs_crc32(const void *data, size_t n)
{
    static const uint32_t table[16] = {
        UINT32_C(0x00000000), UINT32_C(0x1db71064),
        UINT32_C(0x3b6e20c8), UINT32_C(0x26d930ac),
        UINT32_C(0x76dc4190), UINT32_C(0x6b6b51f4),
        UINT32_C(0x4db26158), UINT32_C(0x5005713c),
        UINT32_C(0xedb88320), UINT32_C(0xf00f9344),
        UINT32_C(0xd6d6a3e8), UINT32_C(0xcb61b38c),
        UINT32_C(0x9b64c2b0), UINT32_C(0x86d3d2d4),
        UINT32_C(0xa00ae278), UINT32_C(0xbdbdf21c)
    };
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = UINT32_MAX;
    while (n--) {
        crc ^= *p++;
        crc = (crc >> 4) ^ table[crc & 15U];
        crc = (crc >> 4) ^ table[crc & 15U];
    }
    return ~crc;
}

static int nvs_io_result(znvs_t *fs, int rc)
{
    if (rc != 0) {
        fs->ready = 0;
        return ZNVS_EIO;
    }
    return ZNVS_OK;
}
static int nvs_io_bounds(znvs_t *fs, uint32_t off, size_t len)
{
    if (off > fs->cfg->size || len > fs->cfg->size - off) {
        fs->ready = 0;
        return ZNVS_EIO;
    }
    return ZNVS_OK;
}
static int nvs_io_read(znvs_t *fs, uint32_t off, void *data, size_t len)
{
    if (nvs_io_bounds(fs, off, len) != 0 || (!data && len)) {
        fs->ready = 0;
        return ZNVS_EIO;
    }
    if (!len)
        return ZNVS_OK;
    return nvs_io_result(fs, fs->cfg->read(fs->arg, off, data, len));
}
static int nvs_io_write(znvs_t *fs, uint32_t off, const void *data, size_t len)
{
    uint32_t mask = (uint32_t)fs->cfg->write_size - 1U;
    if (nvs_io_bounds(fs, off, len) != 0 || (!data && len) ||
        (off & mask) || (len & mask)) {
        fs->ready = 0;
        return ZNVS_EIO;
    }
    if (!len)
        return ZNVS_OK;
    return nvs_io_result(fs, fs->cfg->write(fs->arg, off, data, len));
}
static int nvs_io_erase(znvs_t *fs, uint32_t off, size_t len)
{
    uint32_t mask = fs->cfg->erase_size - 1U;
    if (nvs_io_bounds(fs, off, len) != 0 || (off & mask) || (len & mask)) {
        fs->ready = 0;
        return ZNVS_EIO;
    }
    return nvs_io_result(fs, fs->cfg->erase(fs->arg, off, len));
}

static int nvs_prev_ate(znvs_t *fs, uint32_t *addr, struct nvs_ate *ate);
static int nvs_ate_valid(znvs_t *fs, uint16_t entry_addr,
             const struct nvs_ate *entry);

#if ZNVS_CACHE_SIZE > 0

static inline size_t nvs_lookup_cache_pos(uint16_t id)
{
    uint16_t hash;

    /* 16-bit integer hash function found by https://github.com/skeeto/hash-prospector. */
    hash = id;
    hash ^= hash >> 8;
    hash *= 0x88b5U;
    hash ^= hash >> 7;
    hash *= 0xdb2dU;
    hash ^= hash >> 9;

    return hash % ZNVS_CACHE_SIZE;
}

static int nvs_lookup_cache_rebuild(znvs_t *fs)
{
    int rc;
    uint32_t addr, ate_addr;
    uint32_t *cache_entry;
    struct nvs_ate ate;

    memset(fs->lookup_cache, 0xff, sizeof(fs->lookup_cache));
    addr = fs->ate_wra;

    while (true) {
        /* Make a copy of 'addr' as it will be advanced by nvs_pref_ate() */
        ate_addr = addr;
        rc = nvs_prev_ate(fs, &addr, &ate);

        if (rc) {
            return rc;
        }

        cache_entry = &fs->lookup_cache[nvs_lookup_cache_pos(ate.id)];

        if (ate.id != 0xFFFF && *cache_entry == NVS_LOOKUP_CACHE_NO_ADDR &&
            nvs_ate_valid(fs, ate_addr, &ate)) {
            *cache_entry = ate_addr;
        }

        if (addr == fs->ate_wra) {
            break;
        }
    }

    return 0;
}

static void nvs_lookup_cache_invalidate(znvs_t *fs, uint32_t sector)
{
    uint32_t *cache_entry = fs->lookup_cache;
    uint32_t *const cache_end = &fs->lookup_cache[ZNVS_CACHE_SIZE];

    for (; cache_entry < cache_end; ++cache_entry) {
        if ((*cache_entry >> ADDR_SECT_SHIFT) == sector) {
            *cache_entry = NVS_LOOKUP_CACHE_NO_ADDR;
        }
    }
}

#endif /* ZNVS_CACHE_SIZE */

/* basic routines */
/* nvs_al_size returns size aligned to fs->write_block_size */
static inline size_t nvs_al_size(znvs_t *fs, size_t len)
{
    size_t write_block_size = fs->cfg->write_size;

    if (write_block_size <= 1U) {
        return len;
    }
    return (len + (write_block_size - 1U)) & ~(write_block_size - 1U);
}
/* end basic routines */

/* flash routines */

/* Write the data described by @p strm to flash at the given NVS address,
 * respecting the flash write block alignment requirements. The write
 * may include a header, primary data, and a tail. All buffers are
 * written as a single contiguous stream.
 */
static int nvs_flash_al_wrt_streams(znvs_t *fs, uint32_t addr,
                          const struct nvs_flash_wrt_stream *strm)
{
    size_t wbs = fs->cfg->write_size;
    uint8_t buf[ZNVS_IO_SIZE];
    size_t stream_idx = 0U;
    size_t full_bytes = 0U;
    size_t buf_fill = 0U;
    size_t copy = 0U;
    uint32_t offset;
    int rc;

    /* Nothing to write */
    if ((strm->head.len + strm->data.len + strm->tail.len) == 0U) {
        return 0;
    }

    /* Convert NVS address to flash offset */
    offset = 0U;
    offset += fs->cfg->sector_size * (addr >> ADDR_SECT_SHIFT);
    offset += addr & ADDR_OFFS_MASK;

    /* Logical write stream: head -> data -> tail */
    struct nvs_flash_buf streams[] = {
        strm->head,
        strm->data,
        strm->tail,
    };

    while (stream_idx < ARRAY_SIZE(streams)) {
        if (streams[stream_idx].len == 0U) {
            stream_idx++;
            continue;
        }

        /* Direct write of aligned full blocks */
        if (buf_fill == 0) {
            /* number of full blocks = len & ~(wbs - 1) */
            full_bytes = streams[stream_idx].len & ~(wbs - 1);

            if (full_bytes > 0U) {
                rc = nvs_io_write(fs, offset,
                         streams[stream_idx].ptr,
                         full_bytes);
                if (rc) {
                    return rc;
                }

                streams[stream_idx].ptr += full_bytes;
                streams[stream_idx].len -= full_bytes;
                offset += full_bytes;
                continue;
            }
        }

        /* Copy to buffer to assemble a full block */
        copy = MIN(wbs - buf_fill, streams[stream_idx].len);
        if (copy > 0U) {
            (void)memcpy(buf + buf_fill, streams[stream_idx].ptr, copy);

            streams[stream_idx].ptr += copy;
            streams[stream_idx].len -= copy;
            buf_fill += copy;
        }

        /* If buffer full, write to flash */
        if (buf_fill == wbs) {
            rc = nvs_io_write(fs, offset, buf, wbs);
            if (rc) {
                return rc;
            }

            offset += wbs;
            buf_fill = 0U;
        }
    }

    if (buf_fill > 0U) {
        (void)memset(buf + buf_fill, UINT8_C(0xff), wbs - buf_fill);

        rc = nvs_io_write(fs, offset, buf, wbs);
        if (rc) {
            return rc;
        }
    }

    return 0;
}

static int nvs_flash_al_wrt(znvs_t *fs, uint32_t addr, const void *data,
                size_t len)
{
    struct nvs_flash_wrt_stream strm = {
        .data = {
            .ptr = data,
            .len = len,
        },
    };

    return nvs_flash_al_wrt_streams(fs, addr, &strm);
}

/* basic flash read from nvs address */
static int nvs_flash_rd(znvs_t *fs, uint32_t addr, void *data,
             size_t len)
{
    int rc;
    uint32_t offset;

    offset = 0U;
    offset += fs->cfg->sector_size * (addr >> ADDR_SECT_SHIFT);
    offset += addr & ADDR_OFFS_MASK;

    rc = nvs_io_read(fs, offset, data, len);
    return rc;
}

/* allocation entry write */
static int nvs_flash_ate_wrt(znvs_t *fs, const struct nvs_ate *entry)
{
    int rc;
    uint8_t raw[8];

    nvs_ate_encode(entry, raw);
    rc = nvs_flash_al_wrt(fs, fs->ate_wra, raw, sizeof(raw));
#if ZNVS_CACHE_SIZE > 0
    /* 0xFFFF is a special-purpose identifier. Exclude it from the cache */
    if (entry->id != 0xFFFF) {
        fs->lookup_cache[nvs_lookup_cache_pos(entry->id)] = fs->ate_wra;
    }
#endif
    fs->ate_wra -= nvs_al_size(fs, sizeof(struct nvs_ate));

    return rc;
}

/* data write */
static int nvs_flash_data_al_wrt(znvs_t *fs,
                 struct nvs_flash_wrt_stream *strm,
                 bool compute_crc)
{
    uint8_t data_crc[4];
    int rc;

    /* Only add the CRC if required (ignore deletion requests, i.e. when len is 0) */
    if (ZNVS_DATA_CRC && compute_crc && (strm->data.len > 0)) {
        nvs_put32(data_crc, nvs_crc32(strm->data.ptr, strm->data.len));

        strm->tail.ptr = data_crc;
        strm->tail.len = sizeof(data_crc);
    }

    rc = nvs_flash_al_wrt_streams(fs, fs->data_wra, strm);

    fs->data_wra += nvs_al_size(fs, strm->head.len + strm->data.len + strm->tail.len);

    return rc;
}

static int nvs_flash_data_wrt(znvs_t *fs, const void *data, size_t len,
                  bool compute_crc)
{
    struct nvs_flash_wrt_stream strm = {
        .data = {
            .ptr = data,
            .len = len,
        },
    };

    return nvs_flash_data_al_wrt(fs, &strm, compute_crc);
}

/* flash ate read */
static int nvs_flash_ate_rd(znvs_t *fs, uint32_t addr,
                 struct nvs_ate *entry)
{
    uint8_t raw[8];
    int rc = nvs_flash_rd(fs, addr, raw, sizeof(raw));
    if (rc == 0) {
        entry->id = nvs_get16(raw);
        entry->offset = nvs_get16(raw + 2);
        entry->len = nvs_get16(raw + 4);
        entry->part = raw[6];
        entry->crc8 = raw[7];
    }
    return rc;
}

/* end of basic flash routines */

/* advanced flash routines */

/* nvs_flash_block_cmp compares the data in flash at addr to data
 * in blocks of size ZNVS_IO_SIZE aligned to fs->write_block_size
 * returns 0 if equal, 1 if not equal, errcode if error
 */
static int nvs_flash_block_cmp(znvs_t *fs, uint32_t addr, const void *data,
                size_t len)
{
    const uint8_t *data8 = (const uint8_t *)data;
    int rc;
    size_t bytes_to_cmp, block_size;
    uint8_t buf[ZNVS_IO_SIZE];
#if ZNVS_DATA_CRC
    size_t original_len = len;
#endif

    block_size =
        ZNVS_IO_SIZE & ~(fs->cfg->write_size - 1U);

    while (len) {
        bytes_to_cmp = MIN(block_size, len);
        rc = nvs_flash_rd(fs, addr, buf, bytes_to_cmp);
        if (rc) {
            return rc;
        }
        rc = memcmp(data8, buf, bytes_to_cmp);
        if (rc) {
            return 1;
        }
        len -= bytes_to_cmp;
        addr += bytes_to_cmp;
        data8 += bytes_to_cmp;
    }
#if ZNVS_DATA_CRC
    /* Identical payload with a damaged stored CRC must be rewritten, not skipped. */
    rc = nvs_flash_rd(fs, addr, buf, 4);
    if (rc)
        return rc;
    if (nvs_get32(buf) != nvs_crc32(data, original_len))
        return 1;
#endif
    return 0;
}

/* nvs_flash_cmp_const compares the data in flash at addr to a constant
 * value. returns 0 if all data in flash is equal to value, 1 if not equal,
 * errcode if error
 */
static int nvs_flash_cmp_const(znvs_t *fs, uint32_t addr, uint8_t value,
                size_t len)
{
    int rc;
    size_t bytes_to_cmp, block_size;
    uint8_t buf[ZNVS_IO_SIZE];

    block_size =
        ZNVS_IO_SIZE & ~(fs->cfg->write_size - 1U);

    while (len) {
        bytes_to_cmp = MIN(block_size, len);
        rc = nvs_flash_rd(fs, addr, buf, bytes_to_cmp);
        if (rc) {
            return rc;
        }

        for (size_t i = 0; i < bytes_to_cmp; i++) {
            if (buf[i] != value) {
                return 1;
            }
        }

        len -= bytes_to_cmp;
        addr += bytes_to_cmp;
    }
    return 0;
}

/* flash block move (GC-only helper)
 *
 * Move data starting at @addr to the current data write location during GC.
 *
 * This function writes data in write_block_size-aligned chunks only.
 * If the total length is not aligned to write_block_size, the tail bytes
 * (less than one write block) are read into @buf but NOT written immediately.
 *
 * The number of buffered but unwritten bytes is returned via @ctx->buffer_pos.
 * The caller is responsible for flushing these remaining bytes later
 * (typically before writing the corresponding ATE).
 *
 * Notes:
 * - This function is intended to be used ONLY during GC.
 * - @ctx->buffer_pos is guaranteed to be < write_block_size.
 * - No padding or alignment is added to the data.
 */
static int nvs_flash_block_move(znvs_t *fs, uint32_t addr, struct nvs_block_move_ctx *ctx,
                struct nvs_ate *gc_ate)
{
    size_t wbs = fs->cfg->write_size;
    size_t bytes_to_copy, block_size, tail_len;
    size_t len = gc_ate->len;
    int rc;

    /* Include any buffered tail bytes in the length */
    len += ctx->buffer_pos;

    /*
     * Split the total length into:
     * - a multiple of write_block_size (block_size)
     * - a remaining tail smaller than write_block_size (tail_len)
     */
    tail_len = len & (wbs - 1U);
    block_size = ZNVS_IO_SIZE & ~(wbs - 1U);
    len -= tail_len;

    /* Update ATE offset to the new data location.
     * ctx.buffer_pos accounts for any buffered but unwritten
     * data carried over from previous moves.
     */
    gc_ate->offset = (uint16_t)((fs->data_wra + ctx->buffer_pos)
                    & ADDR_OFFS_MASK);

    /* Copy and write only write_block_size-aligned data.
     * Any previously buffered bytes (ctx->buffer_pos) are prepended
     * to the newly read data to form a full aligned write.
     */
    while (len) {
        bytes_to_copy = MIN(block_size, len) - ctx->buffer_pos;

        rc = nvs_flash_rd(fs, addr, ctx->buffer + ctx->buffer_pos, bytes_to_copy);
        if (rc) {
            return rc;
        }

        /* Just rewrite the whole record, no need to recompute the CRC as the data
         * did not change
         */
        rc = nvs_flash_data_wrt(fs, ctx->buffer, bytes_to_copy + ctx->buffer_pos, false);
        if (rc) {
            return rc;
        }

        len  -= bytes_to_copy + ctx->buffer_pos;
        addr += bytes_to_copy;
        ctx->buffer_pos = 0U;
    }

    /* Read the remaining unaligned tail into buffer.
     * This data is not written now and will be combined with the next data block.
     */
    if (tail_len) {
        rc = nvs_flash_rd(fs, addr, ctx->buffer + ctx->buffer_pos,
                  tail_len - ctx->buffer_pos);
        if (rc) {
            return rc;
        }

        ctx->buffer_pos = tail_len;
    }

    return 0;
}

/* erase a sector and verify erase was OK.
 * return 0 if OK, errorcode on error.
 */
static int nvs_flash_erase_sector(znvs_t *fs, uint32_t addr)
{
    int rc;
    uint32_t offset;

    addr &= ADDR_SECT_MASK;

    offset = 0U;
    offset += fs->cfg->sector_size * (addr >> ADDR_SECT_SHIFT);

#if ZNVS_CACHE_SIZE > 0
    nvs_lookup_cache_invalidate(fs, addr >> ADDR_SECT_SHIFT);
#endif
    rc = nvs_io_erase(fs, offset, fs->cfg->sector_size);

    if (rc) {
        return rc;
    }

    if (nvs_flash_cmp_const(fs, addr, UINT8_C(0xff),
            fs->cfg->sector_size)) {
        rc = ZNVS_EIO;
    }

    return rc;
}

static inline uint16_t nvs_data_len_with_crc(size_t len)
{
    return (uint16_t)(len ? len + NVS_DATA_CRC_SIZE : 0U);
}

/* crc update on allocation entry */
static void nvs_ate_crc8_update(struct nvs_ate *entry)
{
    uint8_t raw[8];
    entry->crc8 = 0;
    nvs_ate_encode(entry, raw);
    entry->crc8 = nvs_crc8(raw, 7);
}

/* crc check on allocation entry
 * returns 0 if OK, 1 on crc fail
 */
static int nvs_ate_crc8_check(const struct nvs_ate *entry)
{
    uint8_t crc8, raw[8];
    nvs_ate_encode(entry, raw);
    crc8 = nvs_crc8(raw, 7);
    if (crc8 == entry->crc8) {
        return 0;
    }
    return 1;
}

/* nvs_ate_cmp_const compares an ATE to a constant value. returns 0 if
 * the whole ATE is equal to value, 1 if not equal.
 */
static int nvs_ate_cmp_const(const struct nvs_ate *entry, uint8_t value)
{
    uint16_t pair = (uint16_t)((uint16_t)value | ((uint16_t)value << 8));
    return entry->id != pair || entry->offset != pair || entry->len != pair ||
           entry->part != value || entry->crc8 != value;
}

/* nvs_ate_valid validates an ate:
 *     return 1 if crc8, offset and length are valid,
 *            0 otherwise
 */
static int nvs_ate_valid(znvs_t *fs, uint16_t entry_addr,
             const struct nvs_ate *entry)
{
    uint32_t position;

    (void)fs;
    position = (uint32_t)entry->offset + entry->len;

    if ((nvs_ate_crc8_check(entry)) ||
        (position > (entry_addr & ADDR_OFFS_MASK))) {
        return 0;
    }

    return 1;
}

/* nvs_close_ate_valid validates an sector close ate: a valid sector close ate:
 * - valid ate
 * - len = 0 and id = 0xFFFF
 * - offset points to location at ate multiple from sector size
 * return 1 if valid, 0 otherwise
 */
static int nvs_close_ate_valid(znvs_t *fs, const struct nvs_ate *entry)
{
    size_t ate_size;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    if ((!nvs_ate_valid(fs, (fs->cfg->sector_size - ate_size), entry)) ||
        (entry->len != 0U) || (entry->id != 0xFFFF)) {
        return 0;
    }

    if (entry->offset > fs->cfg->sector_size - 2U * ate_size ||
        (fs->cfg->sector_size - entry->offset) % ate_size) {
        return 0;
    }

    return 1;
}

/* store an entry in flash */
static int nvs_flash_wrt_entry(znvs_t *fs, uint16_t id, const void *data,
                size_t len)
{
    int rc;
    struct nvs_ate entry;

    entry.id = id;
    entry.offset = (uint16_t)(fs->data_wra & ADDR_OFFS_MASK);
    entry.len = nvs_data_len_with_crc(len);
    entry.part = 0xff;

    rc = nvs_flash_data_wrt(fs, data, len, true);
    if (rc) {
        return rc;
    }

    nvs_ate_crc8_update(&entry);

    rc = nvs_flash_ate_wrt(fs, &entry);

    return rc;
}
/* end of flash routines */

/* If the closing ate is invalid, its offset cannot be trusted and
 * the last valid ate of the sector should instead try to be recovered by going
 * through all ate's.
 *
 * addr should point to the faulty closing ate and will be updated to the last
 * valid ate. If no valid ate is found it will be left untouched.
 */
static int nvs_recover_last_ate(znvs_t *fs, uint32_t *addr)
{
    uint32_t data_end_addr, ate_end_addr;
    struct nvs_ate end_ate;
    size_t ate_size;
    int rc;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    *addr -= ate_size;
    ate_end_addr = *addr;
    data_end_addr = *addr & ADDR_SECT_MASK;
    while ((ate_end_addr >= data_end_addr) && (ate_end_addr & ADDR_OFFS_MASK) != 0U) {
        rc = nvs_flash_ate_rd(fs, ate_end_addr, &end_ate);
        if (rc) {
            return rc;
        }
        if (nvs_ate_valid(fs, ate_end_addr, &end_ate)) {
            /* found a valid ate, update data_end_addr and *addr */
            data_end_addr &= ADDR_SECT_MASK;
            data_end_addr += end_ate.offset + end_ate.len;
            *addr = ate_end_addr;
        }
        ate_end_addr -= ate_size;
    }

    return 0;
}

/* walking through allocation entry list, from newest to oldest entries
 * read ate from addr, modify addr to the previous ate
 */
static int nvs_prev_ate(znvs_t *fs, uint32_t *addr, struct nvs_ate *ate)
{
    int rc;
    struct nvs_ate close_ate;
    size_t ate_size;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    rc = nvs_flash_ate_rd(fs, *addr, ate);
    if (rc) {
        return rc;
    }

    *addr += ate_size;
    if (((*addr) & ADDR_OFFS_MASK) != (fs->cfg->sector_size - ate_size)) {
        return 0;
    }

    /* last ate in sector, do jump to previous sector */
    if (((*addr) >> ADDR_SECT_SHIFT) == 0U) {
        *addr += ((uint32_t)(fs->sector_count - 1U) << ADDR_SECT_SHIFT);
    } else {
        *addr -= (UINT32_C(1) << ADDR_SECT_SHIFT);
    }

    rc = nvs_flash_ate_rd(fs, *addr, &close_ate);
    if (rc) {
        return rc;
    }

    rc = nvs_ate_cmp_const(&close_ate, UINT8_C(0xff));
    /* at the end of filesystem */
    if (!rc) {
        *addr = fs->ate_wra;
        return 0;
    }

    /* Update the address if the close ate is valid.
     */
    if (nvs_close_ate_valid(fs, &close_ate)) {
        (*addr) &= ADDR_SECT_MASK;
        (*addr) += close_ate.offset;
        return 0;
    }

    /* The close_ate was invalid, `lets find out the last valid ate
     * and point the address to this found ate.
     *
     * remark: if there was absolutely no valid data in the sector *addr
     * is kept at sector_end - 2*ate_size, the next read will contain
     * invalid data and continue with a sector jump
     */
    return nvs_recover_last_ate(fs, addr);
}

static void nvs_sector_advance(znvs_t *fs, uint32_t *addr)
{
    *addr += (UINT32_C(1) << ADDR_SECT_SHIFT);
    if ((*addr >> ADDR_SECT_SHIFT) == fs->sector_count) {
        *addr -= ((uint32_t)fs->sector_count << ADDR_SECT_SHIFT);
    }
}

/* allocation entry close (this closes the current sector) by writing offset
 * of last ate to the sector end.
 */
static int nvs_sector_close(znvs_t *fs)
{
    struct nvs_ate close_ate;
    size_t ate_size;
    int rc;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    close_ate.id = 0xFFFF;
    close_ate.len = 0U;
    close_ate.offset = (uint16_t)((fs->ate_wra + ate_size) & ADDR_OFFS_MASK);
    close_ate.part = 0xff;

    fs->ate_wra &= ADDR_SECT_MASK;
    fs->ate_wra += (fs->cfg->sector_size - ate_size);

    nvs_ate_crc8_update(&close_ate);

    rc = nvs_flash_ate_wrt(fs, &close_ate);
    if (rc) {
        return rc;
    }

    nvs_sector_advance(fs, &fs->ate_wra);

    fs->data_wra = fs->ate_wra & ADDR_SECT_MASK;

    return 0;
}

static int nvs_add_gc_done_ate(znvs_t *fs)
{
    struct nvs_ate gc_done_ate;

    gc_done_ate.id = 0xffff;
    gc_done_ate.len = 0U;
    gc_done_ate.part = 0xff;
    gc_done_ate.offset = (uint16_t)(fs->data_wra & ADDR_OFFS_MASK);
    nvs_ate_crc8_update(&gc_done_ate);

    return nvs_flash_ate_wrt(fs, &gc_done_ate);
}

/* Attempt to write a new entry during garbage collection
 * and flush any remaining tail.
 */
static int nvs_gc_flush_and_try_write(znvs_t *fs,
                      struct nvs_block_move_ctx *bm_ctx,
                      struct nvs_gc_write_entry *entry)
{
    struct nvs_flash_wrt_stream strm = {
        .head = {
            .ptr = bm_ctx->buffer,
            .len = bm_ctx->buffer_pos,
        },
    };
    size_t required_space = 0U;
    struct nvs_ate wrt_ate;
    size_t ate_size;
    int rc;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    if (entry) {
        required_space = ate_size +
            nvs_al_size(fs, bm_ctx->buffer_pos + nvs_data_len_with_crc(entry->len));
    }

    if (!entry || (fs->ate_wra < (fs->data_wra + required_space))) {
        /* Not enough space for entry, only flush buffer if needed */
        if (bm_ctx->buffer_pos > 0U) {
            rc = nvs_flash_data_al_wrt(fs, &strm, false);
            if (rc) {
                return rc;
            }
        }

        return 0;
    }

    strm.data.ptr = entry->data;
    strm.data.len = entry->len;

    /* Update ATE offset to the new data location.
     * bm_ctx->buffer_pos accounts for any buffered but unwritten
     * data carried over from previous moves.
     */
    wrt_ate.offset = (uint16_t)((fs->data_wra + bm_ctx->buffer_pos)
                    & ADDR_OFFS_MASK);

    rc = nvs_flash_data_al_wrt(fs, &strm, true);
    if (rc) {
        return rc;
    }

    wrt_ate.id = entry->id;
    wrt_ate.len = nvs_data_len_with_crc(entry->len);
    wrt_ate.part = 0xff;

    nvs_ate_crc8_update(&wrt_ate);

    rc = nvs_flash_ate_wrt(fs, &wrt_ate);
    if (rc) {
        return rc;
    }

    entry->is_written = true;

    return 0;
}

/* garbage collection: the address ate_wra has been updated to the new sector
 * that has just been started. The data to gc is in the sector after this new
 * sector.
 */
static int nvs_gc(znvs_t *fs, struct nvs_gc_write_entry *entry)
{
    int rc;
    struct nvs_ate close_ate, gc_ate, wlk_ate;
    uint32_t sec_addr, gc_addr, gc_prev_addr, wlk_addr, wlk_prev_addr,
          data_addr, stop_addr;
    struct nvs_block_move_ctx ctx = {
        .buffer_pos = 0U,
    };
    size_t ate_size;

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));

    sec_addr = (fs->ate_wra & ADDR_SECT_MASK);
    nvs_sector_advance(fs, &sec_addr);
    gc_addr = sec_addr + fs->cfg->sector_size - ate_size;

    /* if the sector is not closed don't do gc */
    rc = nvs_flash_ate_rd(fs, gc_addr, &close_ate);
    if (rc) {
        /* flash error */
        return rc;
    }

    rc = nvs_ate_cmp_const(&close_ate, UINT8_C(0xff));
    if (!rc) {
        goto gc_done;
    }

    stop_addr = gc_addr - ate_size;

    if (nvs_close_ate_valid(fs, &close_ate)) {
        gc_addr &= ADDR_SECT_MASK;
        gc_addr += close_ate.offset;
    } else {
        rc = nvs_recover_last_ate(fs, &gc_addr);
        if (rc) {
            return rc;
        }
    }

    do {
        gc_prev_addr = gc_addr;
        rc = nvs_prev_ate(fs, &gc_addr, &gc_ate);
        if (rc) {
            return rc;
        }

        if (!nvs_ate_valid(fs, gc_prev_addr, &gc_ate)) {
            continue;
        }

#if ZNVS_CACHE_SIZE > 0
        wlk_addr = fs->lookup_cache[nvs_lookup_cache_pos(gc_ate.id)];

        if (wlk_addr == NVS_LOOKUP_CACHE_NO_ADDR) {
            wlk_addr = fs->ate_wra;
        }
#else
        wlk_addr = fs->ate_wra;
#endif
        do {
            wlk_prev_addr = wlk_addr;
            rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
            if (rc) {
                return rc;
            }
            /* if ate with same id is reached we might need to copy.
             * only consider valid wlk_ate's. Something wrong might
             * have been written that has the same ate but is
             * invalid, don't consider these as a match.
             */
            if ((wlk_ate.id == gc_ate.id) &&
                (nvs_ate_valid(fs, wlk_prev_addr, &wlk_ate))) {
                break;
            }
        } while (wlk_addr != fs->ate_wra);

        /* If the walk cursor reaches the same entry as the GC cursor,
         * the data must be moved unless this entry represents a deleted item
         * (len == 0).
         *
         * During GC, data is compacted and rewritten to the current data
         * write location. Data may be packed back-to-back without alignment
         * padding; write alignment is handled internally by buffering.
         */
        if ((wlk_prev_addr == gc_prev_addr) && gc_ate.len) {
            /* If we have a matching entry already in the sector being GC'd:
             * - Check that the entry ID matches the current GC ATE
             * - Check that the existing entry's data + CRC fits within the GC
             *   ATE length
             *
             * Entry matches the GC target and fits in the allocation.
             * Do not write it now; it will be written later during the final GC
             * flush.
             */
            if (entry && (entry->id == gc_ate.id) &&
                (nvs_data_len_with_crc(entry->len) <= gc_ate.len)) {

                continue;
            }

            /* copy needed */

            data_addr = (gc_prev_addr & ADDR_SECT_MASK);
            data_addr += gc_ate.offset;

            /* Move the data to the new location.
             * Data is written in write_block_size-aligned chunks only.
             * Any remaining unaligned bytes are buffered in ctx.buffer and
             * reported back via ctx.buffer_pos.
             */
            rc = nvs_flash_block_move(fs, data_addr, &ctx, &gc_ate);
            if (rc) {
                return rc;
            }

            nvs_ate_crc8_update(&gc_ate);

            rc = nvs_flash_ate_wrt(fs, &gc_ate);
            if (rc) {
                return rc;
            }
        }
    } while (gc_prev_addr != stop_addr);

    rc = nvs_gc_flush_and_try_write(fs, &ctx, entry);
    if (rc) {
        return rc;
    }

gc_done:

    /* Make it possible to detect that gc has finished by writing a
     * gc done ate to the sector. In the field we might have nvs systems
     * that do not have sufficient space to add this ate, so for these
     * situations avoid adding the gc done ate.
     */

    if (fs->ate_wra >= (fs->data_wra + ate_size)) {
        rc = nvs_add_gc_done_ate(fs);
        if (rc) {
            return rc;
        }
    }

    /* Erase the gc'ed sector */
    rc = nvs_flash_erase_sector(fs, sec_addr);

    return rc;
}

static int nvs_startup(znvs_t *fs)
{
    int rc;
    struct nvs_ate last_ate;
    size_t ate_size, empty_len;
    /* Initialize addr to 0 for the case fs->sector_count == 0. This
     * should never happen as this is verified in nvs_mount() but both
     * Coverity and GCC believe the contrary.
     */
    uint32_t addr = 0U;
    uint16_t i, closed_sectors = 0;
    uint8_t erase_value = UINT8_C(0xff);

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    /* step through the sectors to find a open sector following
     * a closed sector, this is where NVS can write.
     */
    for (i = 0; i < fs->sector_count; i++) {
        addr = ((uint32_t)i << ADDR_SECT_SHIFT) +
               (uint16_t)(fs->cfg->sector_size - ate_size);
        rc = nvs_flash_cmp_const(fs, addr, erase_value,
                     sizeof(struct nvs_ate));
        if (rc < 0) {
            goto end;
        }
        if (rc) {
            /* closed sector */
            closed_sectors++;
            nvs_sector_advance(fs, &addr);
            rc = nvs_flash_cmp_const(fs, addr, erase_value,
                         sizeof(struct nvs_ate));
        if (rc < 0) {
            goto end;
        }
            if (!rc) {
                /* open sector */
                break;
            }
        }
    }
    /* all sectors are closed, this is not a nvs fs or irreparably corrupted */
    if (closed_sectors == fs->sector_count) {
        rc = ZNVS_ECORRUPT;
        goto end;

    }

    if (i == fs->sector_count) {
        /* none of the sectors where closed, in most cases we can set
         * the address to the first sector, except when there are only
         * two sectors. Then we can only set it to the first sector if
         * the last sector contains no ate's. So we check this first
         */
        rc = nvs_flash_cmp_const(fs, addr - ate_size, erase_value,
                sizeof(struct nvs_ate));
        if (rc < 0) {
            goto end;
        }
        if (!rc) {
            /* empty ate */
            nvs_sector_advance(fs, &addr);
        }
    }

    /* addr contains address of closing ate in the most recent sector,
     * search for the last valid ate using the recover_last_ate routine
     */

    rc = nvs_recover_last_ate(fs, &addr);
    if (rc) {
        goto end;
    }

    /* addr contains address of the last valid ate in the most recent sector
     * search for the first ate containing all cells erased, in the process
     * also update fs->data_wra.
     */
    fs->ate_wra = addr;
    fs->data_wra = addr & ADDR_SECT_MASK;

    while (fs->ate_wra >= fs->data_wra) {
        rc = nvs_flash_ate_rd(fs, fs->ate_wra, &last_ate);
        if (rc) {
            goto end;
        }

        rc = nvs_ate_cmp_const(&last_ate, erase_value);

        if (!rc) {
            /* found ff empty location */
            break;
        }

        if (nvs_ate_valid(fs, fs->ate_wra, &last_ate)) {
            /* complete write of ate was performed */
            fs->data_wra = addr & ADDR_SECT_MASK;
            /* Align the data write address to the current
             * write block size so that it is possible to write to
             * the sector even if the block size has changed after
             * a software upgrade (unless the physical ATE size
             * will change)."
             */
            fs->data_wra += nvs_al_size(fs, last_ate.offset + last_ate.len);

            /* ate on the last position within the sector is
             * reserved for deletion an entry
             */
            if (fs->ate_wra == fs->data_wra && last_ate.len) {
                /* not a delete ate */
                rc = ZNVS_ECORRUPT;
                goto end;
            }
        }

        fs->ate_wra -= ate_size;
    }

    /* if the sector after the write sector is not empty gc was interrupted
     * we might need to restart gc if it has not yet finished. Otherwise
     * just erase the sector.
     * When gc needs to be restarted, first erase the sector otherwise the
     * data might not fit into the sector.
     */
    addr = fs->ate_wra & ADDR_SECT_MASK;
    nvs_sector_advance(fs, &addr);
    rc = nvs_flash_cmp_const(fs, addr, erase_value, fs->cfg->sector_size);
    if (rc < 0) {
        goto end;
    }
    if (rc) {
        /* the sector after fs->ate_wrt is not empty, look for a marker
         * (gc_done_ate) that indicates that gc was finished.
         */
        bool gc_done_marker = false;
        struct nvs_ate gc_done_ate;

        addr = fs->ate_wra + ate_size;
        while ((addr & ADDR_OFFS_MASK) < (fs->cfg->sector_size - ate_size)) {
            rc = nvs_flash_ate_rd(fs, addr, &gc_done_ate);
            if (rc) {
                goto end;
            }
            if (nvs_ate_valid(fs, addr, &gc_done_ate) &&
                (gc_done_ate.id == 0xffff) &&
                (gc_done_ate.len == 0U)) {
                gc_done_marker = true;
                break;
            }
            addr += ate_size;
        }

        if (gc_done_marker) {
            /* erase the next sector */

            addr = fs->ate_wra & ADDR_SECT_MASK;
            nvs_sector_advance(fs, &addr);
            rc = nvs_flash_erase_sector(fs, addr);
            goto end;
        }

        rc = nvs_flash_erase_sector(fs, fs->ate_wra);
        if (rc) {
            goto end;
        }
        fs->ate_wra &= ADDR_SECT_MASK;
        fs->ate_wra += (fs->cfg->sector_size - 2 * ate_size);
        fs->data_wra = (fs->ate_wra & ADDR_SECT_MASK);
#if ZNVS_CACHE_SIZE > 0
        /**
         * At this point, the lookup cache wasn't built but the gc function need to use it.
         * So, temporarily, we set the lookup cache to the end of the fs.
         * The cache will be rebuilt afterwards
         **/
        for (i = 0; i < ZNVS_CACHE_SIZE; i++) {
            fs->lookup_cache[i] = fs->ate_wra;
        }
#endif
        rc = nvs_gc(fs, NULL);
        goto end;
    }

    /* Standalone change: inspect each orphan-data byte at most once. */
    if (fs->ate_wra > fs->data_wra) {
        uint8_t buf[ZNVS_IO_SIZE];
        uint32_t end_addr = fs->ate_wra;
        while (end_addr > fs->data_wra) {
            empty_len = MIN((size_t)(end_addr - fs->data_wra), sizeof(buf));
            addr = end_addr - (uint32_t)empty_len;
            rc = nvs_flash_rd(fs, addr, buf, empty_len);
            if (rc) {
                goto end;
            }
            while (empty_len > 0 && buf[empty_len - 1] == erase_value) {
                empty_len--;
            }
            if (empty_len > 0) {
                fs->data_wra = (addr & ADDR_SECT_MASK) +
                    (uint32_t)nvs_al_size(fs, (addr & ADDR_OFFS_MASK) + empty_len);
                break;
            }
            end_addr = addr;
        }
    }
    rc = 0;

    /* If the ate_wra is pointing to the first ate write location in a
     * sector and data_wra is not 0, erase the sector as it contains no
     * valid data (this also avoids closing a sector without any data).
     */
    if ((((fs->ate_wra & ADDR_OFFS_MASK) + 2 * ate_size) == fs->cfg->sector_size) &&
        (fs->data_wra != (fs->ate_wra & ADDR_SECT_MASK))) {
        rc = nvs_flash_erase_sector(fs, fs->ate_wra);
        if (rc) {
            goto end;
        }
        fs->data_wra = fs->ate_wra & ADDR_SECT_MASK;
    }

end:

#if ZNVS_CACHE_SIZE > 0
    if (!rc) {
        rc = nvs_lookup_cache_rebuild(fs);
    }
#endif
    /* If the sector is empty add a gc done ate to avoid having insufficient
     * space when doing gc.
     */
    if ((!rc) && ((fs->ate_wra & ADDR_OFFS_MASK) ==
              (fs->cfg->sector_size - 2 * ate_size))) {

        rc = nvs_add_gc_done_ate(fs);
    }
    return rc;
}

static int nvs_write(znvs_t *fs, uint16_t id, const void *data, size_t len)
{
    int rc;
    uint32_t gc_count;
    struct nvs_gc_write_entry wrt_entry;
    size_t ate_size, data_size;
    struct nvs_ate wlk_ate;
    uint32_t wlk_addr, rd_addr;
    uint16_t required_space = 0U; /* no space, appropriate for delete ate */
    bool prev_found = false;

    if (!fs->ready) {

        return ZNVS_ESTATE;
    }

    ate_size = nvs_al_size(fs, sizeof(struct nvs_ate));
    data_size = nvs_al_size(fs, nvs_data_len_with_crc(len));

    /* The maximum data size is sector size - 4 ate
     * where: 1 ate for data, 1 ate for sector close, 1 ate for gc done,
     * and 1 ate to always allow a delete.
     * Also take into account the data CRC that is appended at the end of the data field,
     * if any.
     */
    if ((data_size > (fs->cfg->sector_size - 4 * ate_size)) ||
        ((len > 0) && (data == NULL))) {
        return ZNVS_EINVAL;
    }

    /* find latest entry with same id */
#if ZNVS_CACHE_SIZE > 0
    wlk_addr = fs->lookup_cache[nvs_lookup_cache_pos(id)];

    if (wlk_addr == NVS_LOOKUP_CACHE_NO_ADDR) {
        goto no_cached_entry;
    }
#else
    wlk_addr = fs->ate_wra;
#endif

    while (1) {
        rd_addr = wlk_addr;
        rc = nvs_prev_ate(fs, &wlk_addr, &wlk_ate);
        if (rc) {
            return rc;
        }
        if ((wlk_ate.id == id) &&
            (nvs_ate_valid(fs, rd_addr, &wlk_ate))) {
            prev_found = true;
            break;
        }
        if (wlk_addr == fs->ate_wra) {
            break;
        }
    }

#if ZNVS_CACHE_SIZE > 0
no_cached_entry:
#endif

    if (prev_found) {
        /* previous entry found */
        rd_addr &= ADDR_SECT_MASK;
        rd_addr += wlk_ate.offset;

        if (len == 0) {
            /* do not try to compare with empty data */
            if (wlk_ate.len == 0U) {
                /* skip delete entry as it is already the
                 * last one
                 */
                return 0;
            }
        } else if (len + NVS_DATA_CRC_SIZE == wlk_ate.len) {
            /* do not try to compare if lengths are not equal */
            /* compare the data and if equal return 0 */
            /* note: data CRC is not taken into account here, as it has not yet been
             * appended to the data buffer
             */
            rc = nvs_flash_block_cmp(fs, rd_addr, data, len);
            if (rc <= 0) {
                return rc;
            }
        }
    } else {
        /* skip delete entry for non-existing entry */
        if (len == 0) {
            return 0;
        }
    }

    /* calculate required space if the entry contains data */
    if (data_size) {
        /* Leave space for delete ate */
        required_space = data_size + ate_size;
    }

    gc_count = 0;
    while (1) {
        if (gc_count == fs->sector_count) {
            /* gc'ed all sectors, no extra space will be created
             * by extra gc.
             */
            rc = ZNVS_ENOSPC;
            goto end;
        }

        /* ATEs grow backwards within a sector. In delete-only scenarios,
         * a sector may contain only delete ATEs and no data entries.
         * Prevent ATE writes at current start of sector to avoid crossing
         * into the previous sector.
         */
        if (fs->ate_wra >= (fs->data_wra + required_space) &&
            (fs->ate_wra & ADDR_OFFS_MASK) != 0) {

            rc = nvs_flash_wrt_entry(fs, id, data, len);
            if (rc) {
                goto end;
            }
            break;
        }

        rc = nvs_sector_close(fs);
        if (rc) {
            goto end;
        }

        /* Initialize pending write request for GC processing */
        if (gc_count == 0) {
            wrt_entry.id = id;
            wrt_entry.data = data;
            wrt_entry.len = len;
            wrt_entry.is_written = false;
        }

        rc = nvs_gc(fs, &wrt_entry);
        if (rc) {
            goto end;
        }
        gc_count++;

        /* Exit if the entry has been written during GC */
        if (wrt_entry.is_written) {
            break;
        }
    }
    rc = 0;
end:
    return rc;
}

/* Public interface: status returns and optional output lengths, like zat. */
static bool nvs_power_of_two(uint32_t n)
{
    return n != 0U && (n & (n - 1U)) == 0U;
}
static bool nvs_config_valid(const znvs_cfg_t *cfg)
{
    uint32_t ate_size, count;
    if (!cfg || !cfg->read || !cfg->write || !cfg->erase ||
        !nvs_power_of_two(cfg->write_size) || cfg->write_size > ZNVS_IO_SIZE ||
        !nvs_power_of_two(cfg->sector_size) || cfg->sector_size > UINT32_C(65536) ||
        !nvs_power_of_two(cfg->erase_size) || cfg->sector_size % cfg->erase_size ||
        cfg->size % cfg->sector_size)
        return false;
    ate_size = cfg->write_size > 8U ? cfg->write_size : 8U;
    if (cfg->sector_size < 4U * ate_size + cfg->write_size)
        return false;
    count = cfg->size / cfg->sector_size;
    return count >= 2U && count <= UINT16_MAX;
}
static int nvs_check_ready(const znvs_t *fs)
{
    if (!fs)
        return ZNVS_EINVAL;
    return fs->cfg && fs->ready ? ZNVS_OK : ZNVS_ESTATE;
}
int znvs_init(znvs_t *fs, const znvs_cfg_t *cfg, void *arg)
{
    if (!fs)
        return ZNVS_EINVAL;
    memset(fs, 0, sizeof(*fs));
    if (!nvs_config_valid(cfg))
        return ZNVS_EINVAL;
    fs->cfg = cfg;
    fs->arg = arg;
    fs->sector_count = (uint16_t)(cfg->size / cfg->sector_size);
    return znvs_mount(fs);
}
int znvs_mount(znvs_t *fs)
{
    int rc;
    if (!fs)
        return ZNVS_EINVAL;
    fs->ready = 0;
    if (!nvs_config_valid(fs->cfg))
        return ZNVS_ESTATE;
    fs->sector_count = (uint16_t)(fs->cfg->size / fs->cfg->sector_size);
    rc = nvs_startup(fs);
    if (rc == 0)
        fs->ready = 1;
    return rc;
}
int znvs_format(znvs_t *fs)
{
    uint32_t i;
    int rc;
    if (!fs)
        return ZNVS_EINVAL;
    fs->ready = 0;
    if (!nvs_config_valid(fs->cfg))
        return ZNVS_ESTATE;
    for (i = 0; i < fs->sector_count; ++i) {
        rc = nvs_flash_erase_sector(fs, i << ADDR_SECT_SHIFT);
        if (rc)
            return rc;
    }
    return znvs_mount(fs);
}
size_t znvs_max_size(const znvs_t *fs)
{
    size_t ate_size;
    if (!fs || !fs->cfg)
        return 0;
    ate_size = fs->cfg->write_size > 8U ? fs->cfg->write_size : 8U;
    return fs->cfg->sector_size - 4U * ate_size - NVS_DATA_CRC_SIZE;
}
size_t znvs_available(const znvs_t *fs)
{
    size_t ate_size, bytes;
    if (nvs_check_ready(fs) != 0 || fs->ate_wra <= fs->data_wra ||
        (fs->ate_wra & ADDR_SECT_MASK) != (fs->data_wra & ADDR_SECT_MASK))
        return 0;
    ate_size = fs->cfg->write_size > 8U ? fs->cfg->write_size : 8U;
    bytes = fs->ate_wra - fs->data_wra;
    if (bytes <= ate_size)
        return 0;
    bytes = (bytes - ate_size) & ~((size_t)fs->cfg->write_size - 1U);
    if (bytes <= NVS_DATA_CRC_SIZE)
        return 0;
    bytes -= NVS_DATA_CRC_SIZE;
    return MIN(bytes, znvs_max_size(fs));
}
int znvs_write(znvs_t *fs, uint16_t id, const void *data, size_t len)
{
    int rc = nvs_check_ready(fs);
    if (rc)
        return rc;
    /* Check size before upstream uint16_t narrowing and alignment arithmetic. */
    if (id > ZNVS_ID_MAX || (!data && len) || len > znvs_max_size(fs))
        return ZNVS_EINVAL;
    rc = nvs_write(fs, id, data, len);
    if (rc == ZNVS_EIO || rc == ZNVS_ECORRUPT)
        fs->ready = 0;
    return rc;
}
int znvs_delete(znvs_t *fs, uint16_t id)
{
    return znvs_write(fs, id, NULL, 0);
}
int znvs_read_hist(znvs_t *fs, uint16_t id, uint16_t history,
                   void *data, size_t capacity, size_t *length)
{
    uint32_t addr, entry_addr;
    struct nvs_ate ate;
    size_t data_len;
    bool found = false;
    int rc;
    if (length)
        *length = 0;
    rc = nvs_check_ready(fs);
    if (rc)
        return rc;
    if (id > ZNVS_ID_MAX || (!data && (capacity || !length)))
        return ZNVS_EINVAL;
#if ZNVS_CACHE_SIZE > 0
    addr = fs->lookup_cache[nvs_lookup_cache_pos(id)];
    if (addr == NVS_LOOKUP_CACHE_NO_ADDR)
        return ZNVS_ENOENT;
#else
    addr = fs->ate_wra;
#endif
    do {
        entry_addr = addr;
        rc = nvs_prev_ate(fs, &addr, &ate);
        if (rc)
            return rc;
        if (ate.id == id && nvs_ate_valid(fs, (uint16_t)entry_addr, &ate)) {
            if (history == 0) {
                found = true;
                break;
            }
            --history;
        }
    } while (addr != fs->ate_wra);
    if (!found || ate.len == 0)
        return ZNVS_ENOENT;
    if (ate.len <= NVS_DATA_CRC_SIZE)
        return ZNVS_ECORRUPT;
    data_len = ate.len - NVS_DATA_CRC_SIZE;
    if (length)
        *length = data_len;
    if (!data)
        return ZNVS_OK;
    if (capacity < data_len)
        return ZNVS_ENOSPC;
    addr = (entry_addr & ADDR_SECT_MASK) + ate.offset;
    rc = nvs_flash_rd(fs, addr, data, data_len);
    if (rc)
        return rc;
#if ZNVS_DATA_CRC
    {
        uint8_t raw[4];
        rc = nvs_flash_rd(fs, addr + (uint32_t)data_len, raw, sizeof(raw));
        if (rc)
            return rc;
        if (nvs_get32(raw) != nvs_crc32(data, data_len))
            return ZNVS_ECORRUPT;
    }
#endif
    return ZNVS_OK;
}
int znvs_read(znvs_t *fs, uint16_t id, void *data, size_t capacity, size_t *length)
{
    return znvs_read_hist(fs, id, 0, data, capacity, length);
}
int znvs_rotate(znvs_t *fs)
{
    int rc = nvs_check_ready(fs);
    if (rc)
        return rc;
    rc = nvs_sector_close(fs);
    if (rc == 0)
        rc = nvs_gc(fs, NULL);
    if (rc)
        fs->ready = 0;
    return rc;
}
void *znvs_arg(const znvs_t *fs)
{
    return fs ? fs->arg : NULL;
}
