/* ZNVS: a compact two-bank NOR journal.
 * Copyright (c) 2018 Laczen
 * Copyright (c) 2026 Lingao Meng
 * SPDX-License-Identifier: Apache-2.0
 * Source attribution: see NOTICE.
 */
#include "znvs.h"
#include <string.h>

/* Private build selection; never affects public types or the media format. */
#ifndef ZNVS_PROFILE
#define ZNVS_PROFILE 0
#endif
#if ZNVS_PROFILE < 0 || ZNVS_PROFILE > 2
#error "ZNVS_PROFILE: 0=full, 1=boot, 2=readonly"
#endif
enum {
    ZNVS_HEADER_BYTES = 32,
    ZNVS_HEADER_COPY_BYTES = 16,
    ZNVS_META_BYTES = 8,
    ZNVS_IO_BYTES = 32,
    ZNVS_GC_FILTER_BYTES = 16,
    ZNVS_GC_TAGS = 8
};

enum {
    ZNVS_EMPTY,
    ZNVS_TORN,
    ZNVS_COMMITTED
};

enum {
    ZNVS_READY = 1,
    ZNVS_SEALED = 2
};

struct znvs_gc_seen {
    uint16_t ids[ZNVS_GC_TAGS];
    uint8_t bits[ZNVS_GC_FILTER_BYTES];
};

struct znvs_entry {
    uint32_t addr;
    uint16_t id;
    uint16_t len;
};

static uint32_t znvs_get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void znvs_put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

#if ZNVS_PROFILE != 2
static void znvs_encode(uint8_t *raw, uint32_t a, uint32_t b)
{
    znvs_put32(raw, a);
    znvs_put32(raw + 4, b);
    znvs_put32(raw + 8, ~a);
    znvs_put32(raw + 12, ~b);
}
#endif

/* A subset of the intended zero bits cannot encode a different valid pair. */
static int znvs_encoded(const uint8_t *raw)
{
    return (znvs_get32(raw) ^ znvs_get32(raw + 8)) == UINT32_MAX && (znvs_get32(raw + 4) ^ znvs_get32(raw + 12)) == UINT32_MAX;
}

static int znvs_erased(const uint8_t *raw, size_t len)
{
    while (len--) {
        if (*raw++ != 0xff) {
            return 0;
        }
    }
    return 1;
}

static uint32_t znvs_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    unsigned bit;
    while (len--) {
        crc ^= *p++;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t znvs_entry_crc(const struct znvs_entry *entry)
{
    uint8_t raw[4];
    znvs_put32(raw, entry->id | (uint32_t)entry->len << 16);
    return znvs_crc32(UINT32_MAX, raw, sizeof(raw));
}

static uint32_t znvs_align_size(const znvs_t *fs, uint32_t len)
{
    uint32_t mask = fs->write_size - 1U;
    return (len + mask) & ~mask;
}

static uint32_t znvs_meta_size(const znvs_t *fs)
{
    return fs->slot_size - fs->write_size;
}

static uint32_t znvs_slot_size(const znvs_t *fs)
{
    return fs->slot_size;
}

static uint32_t znvs_data_size(const znvs_t *fs, uint32_t len)
{
    return len ? znvs_align_size(fs, len) + fs->crc_size : 0;
}

static int znvs_fail(znvs_t *fs, int error)
{
    fs->ready = 0;
    return error;
}

static int znvs_read_flash(znvs_t *fs, uint32_t addr, void *data, size_t len)
{
    if (fs->cfg.read(fs->arg, addr, data, len)) {
        return znvs_fail(fs, ZNVS_EIO);
    }
    return 0;
}

/* Read only the active bank. Validate addresses before using decoded entries. */
static int znvs_load_entry(znvs_t *fs, uint32_t addr, struct znvs_entry *entry)
{
    uint8_t raw[ZNVS_IO_BYTES + 1];
    uint32_t size = znvs_meta_size(fs);
    if (znvs_read_flash(fs, addr, raw, size + 1)) {
        return ZNVS_EIO;
    }
    if (raw[size] == 0xff) {
        return znvs_erased(raw, size) ? ZNVS_EMPTY : ZNVS_TORN;
    }
    if (znvs_get32(raw + 4) != ~znvs_crc32(UINT32_MAX, raw, 4)) {
        return znvs_fail(fs, ZNVS_ECORRUPT);
    }
    entry->id = (uint16_t)znvs_get32(raw);
    entry->len = (uint16_t)(znvs_get32(raw) >> 16);
    if (entry->id == UINT16_MAX) {
        return znvs_fail(fs, ZNVS_ECORRUPT);
    }
    return ZNVS_COMMITTED;
}

/* Committed records form a contiguous prefix; offsets follow from lengths. */
static int znvs_previous_entry(znvs_t *fs, struct znvs_pos *pos, struct znvs_entry *entry)
{
    uint32_t size;
    int rc = znvs_load_entry(fs, pos->ate, entry);
    if (rc < 0) {
        return rc;
    }
    if (rc != ZNVS_COMMITTED) {
        return znvs_fail(fs, ZNVS_ECORRUPT);
    }
    size = znvs_data_size(fs, entry->len);
    if (size > pos->data - (fs->bank + ZNVS_HEADER_BYTES)) {
        return znvs_fail(fs, ZNVS_ECORRUPT);
    }
    pos->data -= size;
    pos->ate += znvs_slot_size(fs);
    entry->addr = pos->data;
    return 0;
}

static int znvs_find_entry(znvs_t *fs, uint16_t id, uint16_t history, struct znvs_entry *entry)
{
    struct znvs_pos pos = fs->pos;
    int rc;
    while (pos.ate < fs->bank + fs->bank_size) {
        rc = znvs_previous_entry(fs, &pos, entry);
        if (rc) {
            return rc;
        }
        if (entry->id == id) {
            if (!history) {
                return 0;
            }
            --history;
        }
    }
    return ZNVS_ENOENT;
}

static uint32_t znvs_format_magic(const znvs_t *fs)
{
    /* B is a multiple of W, W is a power of two and B < 2^31. The trailing
     * ones uniquely encode W; the remaining bits encode B without overlap. */
    return UINT32_C(0x31564e5a) ^ (fs->bank_size << 1) ^ (fs->write_size - 1U);
}

static int znvs_bank_header(znvs_t *fs, uint32_t bank, uint32_t *sequence)
{
    uint8_t raw[ZNVS_HEADER_BYTES];
    unsigned i;
    int valid = 0;
    if (znvs_read_flash(fs, bank, raw, sizeof(raw))) {
        return ZNVS_EIO;
    }
    /* Either copy can publish the snapshot. Once both copies are complete,
     * a single retained bit flip cannot remove both or select an older bank. */
    for (i = 0; i < ZNVS_HEADER_BYTES; i += ZNVS_HEADER_COPY_BYTES) {
        if (znvs_encoded(raw + i) && znvs_get32(raw + i) == znvs_format_magic(fs)) {
            uint32_t value = znvs_get32(raw + i + 4);
            if (valid && value != *sequence) {
                return znvs_fail(fs, ZNVS_ECORRUPT);
            }
            *sequence = value;
            valid = 1;
        }
    }
    return valid;
}

#if ZNVS_PROFILE != 2
static int znvs_check_erased(znvs_t *fs, uint32_t addr, uint32_t len)
{
    uint8_t raw[ZNVS_IO_BYTES];
    while (len) {
        uint32_t n = len < sizeof(raw) ? len : sizeof(raw);
        if (znvs_read_flash(fs, addr, raw, n)) {
            return ZNVS_EIO;
        }
        if (!znvs_erased(raw, n)) {
            return ZNVS_ECORRUPT;
        }
        addr += n;
        len -= n;
    }
    return 0;
}

static int znvs_write_flash(znvs_t *fs, uint32_t addr, const void *data, size_t len)
{
    if (fs->cfg.write(fs->arg, addr, data, len)) {
        return znvs_fail(fs, ZNVS_EIO);
    }
    return 0;
}

static int znvs_program(znvs_t *fs, uint32_t addr, const void *data, uint32_t len)
{
    uint8_t raw[ZNVS_IO_BYTES];
    uint32_t full = len & ~(uint32_t)(fs->write_size - 1);
    if (full && znvs_write_flash(fs, addr, data, full)) {
        return ZNVS_EIO;
    }
    if (len != full) {
        memset(raw, 0xff, fs->write_size);
        memcpy(raw, (const uint8_t *)data + full, len - full);
        return znvs_write_flash(fs, addr + full, raw, fs->write_size);
    }
    return 0;
}

static int znvs_erase_bank(znvs_t *fs, uint32_t bank)
{
    if (fs->cfg.erase(fs->arg, bank, fs->bank_size) || znvs_check_erased(fs, bank, fs->bank_size)) {
        return znvs_fail(fs, ZNVS_EIO);
    }
    return 0;
}

static int znvs_publish_bank(znvs_t *fs, uint32_t bank, uint32_t sequence)
{
    uint8_t raw[ZNVS_HEADER_BYTES];
    znvs_encode(raw, znvs_format_magic(fs), sequence);
    memcpy(raw + ZNVS_HEADER_COPY_BYTES, raw, ZNVS_HEADER_COPY_BYTES);
    /* Every supported write granule divides ZNVS_HEADER_BYTES. */
    return znvs_write_flash(fs, bank, raw, sizeof(raw));
}

/* Write metadata, payload and CRC before the separate commit block. */
static int znvs_append(znvs_t *fs, struct znvs_pos *pos, const struct znvs_entry *entry, const void *data)
{
    uint8_t raw[ZNVS_IO_BYTES];
    uint32_t addr = pos->ate - znvs_slot_size(fs);
    uint32_t data_addr = pos->data;
    uint32_t done = 0;
    uint32_t crc = znvs_entry_crc(entry);
    znvs_put32(raw, entry->id | (uint32_t)entry->len << 16);
    znvs_put32(raw + 4, ~crc);
    if (znvs_program(fs, addr, raw, ZNVS_META_BYTES)) {
        return ZNVS_EIO;
    }
    while (done < entry->len) {
        uint32_t n = entry->len - done;
        const void *chunk = data;
        if (!data) {
            if (n > ZNVS_IO_BYTES) {
                n = ZNVS_IO_BYTES;
            }
            if (znvs_read_flash(fs, entry->addr + done, raw, n)) {
                return ZNVS_EIO;
            }
            chunk = raw;
        }
        crc = znvs_crc32(crc, chunk, n);
        if (znvs_program(fs, data_addr + done, chunk, n)) {
            return ZNVS_EIO;
        }
        done += n;
    }
    if (entry->len) {
        if (!data) {
            if (znvs_read_flash(fs, entry->addr + znvs_align_size(fs, entry->len), raw, 4)) {
                return ZNVS_EIO;
            }
            if (znvs_get32(raw) != ~crc) {
                return znvs_fail(fs, ZNVS_ECORRUPT);
            }
        }
        znvs_put32(raw, ~crc);
        if (znvs_program(fs, data_addr + znvs_align_size(fs, entry->len), raw, 4)) {
            return ZNVS_EIO;
        }
    }
    raw[0] = 0;
    memset(raw + 1, 0xff, fs->write_size - 1);
    if (znvs_write_flash(fs, addr + znvs_meta_size(fs), raw, fs->write_size)) {
        return ZNVS_EIO;
    }
    pos->ate = addr;
    pos->data += znvs_data_size(fs, entry->len);
    return 0;
}

static int znvs_next_live(znvs_t *fs, struct znvs_pos *cursor, uint16_t exclude, struct znvs_entry *entry, struct znvs_gc_seen *seen)
{
    struct znvs_entry latest;
    int rc;
    while (cursor->ate < fs->bank + fs->bank_size) {
        rc = znvs_previous_entry(fs, cursor, entry);
        if (rc) {
            return rc;
        }
        if (entry->id == exclude) {
            continue;
        }
#if ZNVS_PROFILE == 0
        {
            uint32_t slot = (entry->id ^ (entry->id >> 8)) & (ZNVS_GC_TAGS - 1);
            uint8_t mask = (uint8_t)(1U << (entry->id & 7));
            int first;
            if (seen->ids[slot] == entry->id) {
                continue;
            }
            seen->ids[slot] = entry->id;
            slot = (entry->id >> 3) & (ZNVS_GC_FILTER_BYTES - 1);
            first = seen->bits[slot] & mask;
            seen->bits[slot] &= (uint8_t)~mask;
            if (!entry->len) {
                continue;
            }
            /* A set bit proves the ID is new. Tags skip known duplicates;
             * filter collisions and evicted tags use an exact lookup. */
            if (first) {
                return 1;
            }
        }
#else
        (void)seen;
        if (!entry->len) {
            continue;
        }
#endif
        rc = znvs_find_entry(fs, entry->id, 0, &latest);
        if (rc) {
            return rc;
        }
        /* Nonempty payload starts uniquely identify records, even after deletes. */
        if (entry->addr == latest.addr) {
            return 1;
        }
    }
    return 0;
}

static int znvs_collect(znvs_t *fs, const struct znvs_entry *pending, const void *data)
{
    struct znvs_pos pos;
    struct znvs_entry entry;
    uint32_t bank_size = fs->bank_size;
    uint32_t bank = fs->bank ^ bank_size;
    struct znvs_pos cursor;
    uint32_t used = ZNVS_HEADER_BYTES;
#if ZNVS_PROFILE == 0
    struct znvs_gc_seen storage;
    struct znvs_gc_seen *seen = &storage;
#else
    struct znvs_gc_seen *seen = NULL;
#endif
    int rc;
    unsigned pass;
    pos.data = bank + ZNVS_HEADER_BYTES;
    pos.ate = bank + bank_size;
    if (pending->len) {
        used += znvs_slot_size(fs) + znvs_data_size(fs, pending->len);
    }
    /* First prove that the snapshot fits, then erase and copy it. */
    for (pass = 0; pass < 2; ++pass) {
        cursor = fs->pos;
#if ZNVS_PROFILE == 0
        memset(seen, 0xff, sizeof(*seen));
#endif
        while ((rc = znvs_next_live(fs, &cursor, pending->id, &entry, seen)) > 0) {
            if (pass) {
                rc = znvs_append(fs, &pos, &entry, NULL);
                if (rc) {
                    return rc;
                }
            } else {
                used += znvs_slot_size(fs) + znvs_data_size(fs, entry.len);
                if (used > bank_size) {
                    return ZNVS_ENOSPC;
                }
            }
        }
        if (rc < 0) {
            return rc;
        }
        if (!pass && znvs_erase_bank(fs, bank)) {
            return ZNVS_EIO;
        }
    }
    if (!rc && pending->len) {
        rc = znvs_append(fs, &pos, pending, data);
    }
    if (!rc) {
        rc = znvs_publish_bank(fs, bank, fs->sequence + 1);
    }
    if (!rc) {
        fs->bank = bank;
        ++fs->sequence;
        fs->pos = pos;
        fs->ready = ZNVS_READY;
    }
    return rc;
}
#endif

static int znvs_ready(const znvs_t *fs)
{
    return !fs ? ZNVS_EINVAL : ((fs->ready & ZNVS_READY) ? 0 : ZNVS_ESTATE);
}

static int znvs_valid_config(const znvs_cfg_t *cfg)
{
    uint32_t w, bank;
    if (!cfg || !cfg->read || (ZNVS_PROFILE != 2 && (!cfg->write || !cfg->erase))) {
        return 0;
    }
    w = cfg->write_size;
    bank = cfg->size / 2;
    return w && !(w & (w - 1)) && w <= ZNVS_IO_BYTES && cfg->erase_size && !(cfg->erase_size & (cfg->erase_size - 1)) && !(cfg->size & 1) && !(bank & (w - 1)) && !(bank & (cfg->erase_size - 1));
}

int znvs_init(znvs_t *fs, const znvs_cfg_t *cfg, void *arg)
{
    if (!fs) {
        return ZNVS_EINVAL;
    }
    fs->ready = 0;
    if (!znvs_valid_config(cfg)) {
        fs->cfg.read = NULL;
        return ZNVS_EINVAL;
    }
    fs->cfg = *cfg;
    fs->bank_size = cfg->size / 2;
    fs->write_size = (uint8_t)cfg->write_size;
    fs->slot_size = (uint8_t)(znvs_align_size(fs, ZNVS_META_BYTES) + fs->write_size);
    fs->crc_size = (uint8_t)znvs_align_size(fs, 4);
    if (fs->bank_size < (uint32_t)ZNVS_HEADER_BYTES + fs->slot_size + fs->write_size + fs->crc_size) {
        fs->cfg.read = NULL;
        return ZNVS_EINVAL;
    }
    fs->arg = arg;
    return znvs_mount(fs);
}

int znvs_mount(znvs_t *fs)
{
    struct znvs_entry entry;
    uint32_t sequence[2];
    uint32_t bank_size;
    unsigned sealed = 0;
    int a, b, rc;
    if (!fs || !fs->cfg.read) {
        return fs ? ZNVS_ESTATE : ZNVS_EINVAL;
    }
    fs->ready = 0;
    bank_size = fs->bank_size;
    a = znvs_bank_header(fs, 0, &sequence[0]);
    if (a < 0) {
        return a;
    }
    b = znvs_bank_header(fs, bank_size, &sequence[1]);
    if (b < 0) {
        return b;
    }
    if (!a && !b) {
#if ZNVS_PROFILE != 0
        return ZNVS_ECORRUPT;
#else
        uint8_t raw[ZNVS_HEADER_BYTES], expected[ZNVS_HEADER_BYTES];
        unsigned i;
        /* Only blank media or a torn first header may be initialized. */
        if ((rc = znvs_check_erased(fs, ZNVS_HEADER_BYTES, fs->cfg.size - ZNVS_HEADER_BYTES)) != 0 || znvs_read_flash(fs, 0, raw, sizeof(raw))) {
            return rc ? rc : ZNVS_EIO;
        }
        znvs_encode(expected, znvs_format_magic(fs), 0);
        memcpy(expected + ZNVS_HEADER_COPY_BYTES, expected, ZNVS_HEADER_COPY_BYTES);
        for (i = 0; i < sizeof(raw); ++i) {
            if ((raw[i] & expected[i]) != expected[i]) {
                return ZNVS_ECORRUPT;
            }
        }
        if (!znvs_erased(raw, sizeof(raw)) && znvs_erase_bank(fs, 0)) {
            return ZNVS_EIO;
        }
        if (znvs_publish_bank(fs, 0, 0)) {
            return ZNVS_EIO;
        }
        a = 1;
        sequence[0] = 0;
#endif
    }
    if (a && b && sequence[1] - sequence[0] != 1 && sequence[0] - sequence[1] != 1) {
        return ZNVS_ECORRUPT;
    }
    b = b && (!a || sequence[1] - sequence[0] == 1);
    fs->bank = b ? bank_size : 0;
    fs->sequence = sequence[b];
    fs->pos.data = fs->bank + ZNVS_HEADER_BYTES;
    fs->pos.ate = fs->bank + bank_size;
    while (fs->pos.ate - fs->pos.data >= znvs_slot_size(fs)) {
        uint32_t record_size;
        rc = znvs_load_entry(fs, fs->pos.ate - znvs_slot_size(fs), &entry);
        if (rc < 0) {
            return rc;
        }
        if (rc == ZNVS_EMPTY) {
            break;
        }
        if (rc == ZNVS_TORN) {
            /* Never append after a partial record or interpret its length. */
            sealed = ZNVS_SEALED;
            break;
        }
        record_size = znvs_data_size(fs, entry.len);
        if (record_size > fs->pos.ate - znvs_slot_size(fs) - fs->pos.data) {
            return znvs_fail(fs, ZNVS_ECORRUPT);
        }
        fs->pos.ate -= znvs_slot_size(fs);
        fs->pos.data += record_size;
    }
    fs->ready = ZNVS_READY | sealed;
    return 0;
}

static size_t znvs_max_payload(const znvs_t *fs)
{
    uint32_t bytes;
    bytes = fs->bank_size - ZNVS_HEADER_BYTES - znvs_slot_size(fs) - fs->crc_size;
    return bytes > UINT16_MAX ? UINT16_MAX : bytes;
}

size_t znvs_max_size(const znvs_t *fs)
{
    return fs && fs->cfg.read ? znvs_max_payload(fs) : 0;
}

#if ZNVS_PROFILE == 0
size_t znvs_available(const znvs_t *fs)
{
    uint32_t bytes;
    if (znvs_ready(fs) || (fs->ready & ZNVS_SEALED)) {
        return 0;
    }
    bytes = fs->pos.ate - fs->pos.data;
    if (bytes <= znvs_slot_size(fs) + fs->crc_size) {
        return 0;
    }
    bytes -= znvs_slot_size(fs) + fs->crc_size;
    return bytes > UINT16_MAX ? UINT16_MAX : bytes;
}

#endif

#if ZNVS_PROFILE != 0
int znvs_read(znvs_t *fs, uint16_t id, void *data, size_t capacity, size_t *length)
{
    const uint16_t history = 0;
#else
int znvs_read_hist(znvs_t *fs, uint16_t id, uint16_t history, void *data, size_t capacity, size_t *length)
{
#endif
    struct znvs_entry entry;
    int rc;
    if (length) {
        *length = 0;
    }
    if ((rc = znvs_ready(fs)) != 0) {
        return rc;
    }
    if (id == UINT16_MAX || (!data && (capacity || !length))) {
        return ZNVS_EINVAL;
    }
    if ((rc = znvs_find_entry(fs, id, history, &entry)) != 0) {
        return rc;
    }
    if (!entry.len) {
        return ZNVS_ENOENT;
    }
    if (length) {
        *length = entry.len;
    }
    if (!data) {
        return 0;
    }
    if (capacity < entry.len) {
        return ZNVS_ENOSPC;
    }
    if (znvs_read_flash(fs, entry.addr, data, entry.len)) {
        return ZNVS_EIO;
    }
    {
        uint8_t raw[4];
        if (znvs_read_flash(fs, entry.addr + znvs_align_size(fs, entry.len), raw, sizeof(raw))) {
            return ZNVS_EIO;
        }
        if (znvs_get32(raw) != ~znvs_crc32(znvs_entry_crc(&entry), data, entry.len)) {
            return ZNVS_ECORRUPT;
        }
    }
    return 0;
}

#if ZNVS_PROFILE == 0
int znvs_read(znvs_t *fs, uint16_t id, void *data, size_t capacity, size_t *length)
{
    return znvs_read_hist(fs, id, 0, data, capacity, length);
}
#endif

#if ZNVS_PROFILE != 2
int znvs_write(znvs_t *fs, uint16_t id, const void *data, size_t len)
{
    struct znvs_entry entry;
#if ZNVS_PROFILE == 0
    uint8_t raw[ZNVS_IO_BYTES];
    uint32_t done = 0;
#endif
    int rc = znvs_ready(fs);
    if (rc) {
        return rc;
    }
    if (id == UINT16_MAX || (!data && len) || len > znvs_max_payload(fs)) {
        return ZNVS_EINVAL;
    }
    if (ZNVS_PROFILE == 0 || !len) {
        rc = znvs_find_entry(fs, id, 0, &entry);
        if (rc && rc != ZNVS_ENOENT) {
            return rc;
        }
    }
#if ZNVS_PROFILE == 0
    if ((!rc && entry.len == len) || (rc == ZNVS_ENOENT && !len)) {
        while (done < len) {
            uint32_t n = len - done;
            if (n > sizeof(raw)) {
                n = sizeof(raw);
            }
            if (znvs_read_flash(fs, entry.addr + done, raw, n)) {
                return ZNVS_EIO;
            }
            if (memcmp(raw, (const uint8_t *)data + done, n)) {
                break;
            }
            done += n;
        }
        if (done == len) {
            if (len) {
                if (znvs_read_flash(fs, entry.addr + znvs_align_size(fs, entry.len), raw, 4)) {
                    return ZNVS_EIO;
                }
                if (znvs_get32(raw) == ~znvs_crc32(znvs_entry_crc(&entry), data, len)) {
                    return 0;
                }
            } else {
                return 0;
            }
        }
    }
#else
    if (!len && (rc == ZNVS_ENOENT || !entry.len)) {
        return 0;
    }
#endif
    entry.id = id;
    entry.len = (uint16_t)len;
    if (!(fs->ready & ZNVS_SEALED) && znvs_slot_size(fs) + znvs_data_size(fs, entry.len) <= fs->pos.ate - fs->pos.data) {
        rc = znvs_append(fs, &fs->pos, &entry, data);
        return rc;
    }
    return znvs_collect(fs, &entry, data);
}

int znvs_delete(znvs_t *fs, uint16_t id)
{
    return znvs_write(fs, id, NULL, 0);
}

#if ZNVS_PROFILE == 0
int znvs_rotate(znvs_t *fs)
{
    const struct znvs_entry pending = {0, UINT16_MAX, 0};
    int rc = znvs_ready(fs);
    return rc ? rc : znvs_collect(fs, &pending, NULL);
}

int znvs_format(znvs_t *fs)
{
    int rc;
    if (!fs || !fs->cfg.read) {
        return fs ? ZNVS_ESTATE : ZNVS_EINVAL;
    }
    fs->ready = 0;
    rc = znvs_erase_bank(fs, 0);
    if (!rc) {
        rc = znvs_erase_bank(fs, fs->bank_size);
    }
    return rc ? rc : znvs_mount(fs);
}
#endif
#endif

#if ZNVS_PROFILE == 0
void *znvs_arg(const znvs_t *fs)
{
    return fs && fs->cfg.read ? fs->arg : NULL;
}
#endif
