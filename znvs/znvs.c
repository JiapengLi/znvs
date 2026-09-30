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
    HEADER_BYTES = 32,
    HEADER_COPY_BYTES = 16,
    META_BYTES = 20,
    IO_BYTES = 32,
    GC_FILTER_BYTES = 32
};

enum { EMPTY,
       TORN,
       RESERVED,
       COMMITTED };

struct entry {
    uint32_t offset;
    uint16_t id;
    uint16_t len;
};

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

#if ZNVS_PROFILE != 2
static void encode(uint8_t *raw, uint32_t a, uint32_t b)
{
    put32(raw, a);
    put32(raw + 4, b);
    put32(raw + 8, ~a);
    put32(raw + 12, ~b);
}
#endif

/* A subset of the intended zero bits cannot encode a different valid pair. */
static int encoded(const uint8_t *raw)
{
    return (get32(raw) ^ get32(raw + 8)) == UINT32_MAX && (get32(raw + 4) ^ get32(raw + 12)) == UINT32_MAX;
}

static int erased(const uint8_t *raw, size_t len)
{
    while (len--) {
        if (*raw++ != 0xff) {
            return 0;
        }
    }
    return 1;
}

static uint32_t crc32(uint32_t crc, const void *data, size_t len)
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

static uint32_t entry_crc(const struct entry *entry)
{
    uint8_t raw[4];
    put32(raw, entry->id | (uint32_t)entry->len << 16);
    return crc32(UINT32_MAX, raw, sizeof(raw));
}

static uint32_t align_size(const znvs_t *fs, uint32_t len)
{
    uint32_t mask = fs->write_size - 1U;
    return (len + mask) & ~mask;
}

static uint32_t meta_size(const znvs_t *fs)
{
    return fs->slot_size - fs->write_size;
}

static uint32_t slot_size(const znvs_t *fs)
{
    return fs->slot_size;
}

static uint32_t data_size(const znvs_t *fs, uint32_t len)
{
    return len ? align_size(fs, len) + fs->crc_size : 0;
}

static int fail(znvs_t *fs, int error)
{
    fs->ready = 0;
    return error;
}

static int read_flash(znvs_t *fs, uint32_t off, void *data, size_t len)
{
    if (fs->cfg->read(fs->arg, off, data, len)) {
        return fail(fs, ZNVS_EIO);
    }
    return 0;
}

/* Read only the active bank. Validate addresses before using decoded entries. */
static int load_entry(znvs_t *fs, uint32_t addr, struct entry *entry)
{
    uint8_t raw[(IO_BYTES + 1)];
    uint32_t size = meta_size(fs);
    uint32_t bank = fs->bank;
    int committed;
    if (read_flash(fs, addr, raw, size + 1)) {
        return ZNVS_EIO;
    }
    committed = raw[size] != 0xff;
    if (!encoded(raw)) {
        return committed ? fail(fs, ZNVS_ECORRUPT) : (erased(raw, size + 1) ? EMPTY : TORN);
    }
    /* The reservation CRC may itself be torn. Only the independent commit
     * makes it mandatory, before ID/length/offset can affect any lookup. */
    if (committed && get32(raw + 16) != ~crc32(UINT32_MAX, raw, 8)) {
        return fail(fs, ZNVS_ECORRUPT);
    }
    entry->id = (uint16_t)get32(raw);
    entry->len = (uint16_t)(get32(raw) >> 16);
    entry->offset = get32(raw + 4);
    if (entry->id == UINT16_MAX || entry->offset < bank + HEADER_BYTES || entry->offset > addr || data_size(fs, entry->len) > addr - entry->offset || (entry->offset & (fs->write_size - 1U))) {
        return fail(fs, ZNVS_ECORRUPT);
    }
    return committed ? COMMITTED : RESERVED;
}

static int find_entry(znvs_t *fs, uint16_t id, uint16_t history, struct entry *entry, uint32_t *found)
{
    uint32_t addr;
    int rc;
    for (addr = fs->pos.ate; addr < fs->bank + fs->bank_size; addr += slot_size(fs)) {
        rc = load_entry(fs, addr, entry);
        if (rc < 0) {
            return rc;
        }
        if (rc == COMMITTED && entry->id == id) {
            if (!history) {
                *found = addr;
                return 0;
            }
            --history;
        }
    }
    return ZNVS_ENOENT;
}

static uint32_t format_magic(const znvs_t *fs)
{
    /* B is a multiple of W, W is a power of two and B < 2^31. The trailing
     * ones uniquely encode W; the remaining bits encode B without overlap. */
    return UINT32_C(0x33564e5a) ^ (fs->bank_size << 1) ^ (fs->write_size - 1U);
}

static int bank_header(znvs_t *fs, uint32_t bank, uint32_t *sequence)
{
    uint8_t raw[HEADER_BYTES];
    unsigned i;
    int valid = 0;
    if (read_flash(fs, bank, raw, sizeof(raw))) {
        return ZNVS_EIO;
    }
    /* Either copy can publish the snapshot. A single retained bit flip cannot
     * remove both copies or select an older snapshot. */
    for (i = 0; i < HEADER_BYTES; i += HEADER_COPY_BYTES) {
        if (encoded(raw + i) && get32(raw + i) == format_magic(fs)) {
            uint32_t value = get32(raw + i + 4);
            if (valid && value != *sequence) {
                return fail(fs, ZNVS_ECORRUPT);
            }
            *sequence = value;
            valid = 1;
        }
    }
    return valid;
}

#if ZNVS_PROFILE != 2
static int check_erased(znvs_t *fs, uint32_t off, uint32_t len)
{
    uint8_t raw[IO_BYTES];
    while (len) {
        uint32_t n = len < sizeof(raw) ? len : sizeof(raw);
        if (read_flash(fs, off, raw, n)) {
            return ZNVS_EIO;
        }
        if (!erased(raw, n)) {
            return ZNVS_ECORRUPT;
        }
        off += n;
        len -= n;
    }
    return 0;
}

static int write_flash(znvs_t *fs, uint32_t off, const void *data, size_t len)
{
    if (fs->cfg->write(fs->arg, off, data, len)) {
        return fail(fs, ZNVS_EIO);
    }
    return 0;
}

static int program(znvs_t *fs, uint32_t off, const void *data, uint32_t len)
{
    uint8_t raw[IO_BYTES];
    uint32_t full = len & ~(uint32_t)(fs->write_size - 1);
    if (full && write_flash(fs, off, data, full)) {
        return ZNVS_EIO;
    }
    if (len != full) {
        memset(raw, 0xff, fs->write_size);
        memcpy(raw, (const uint8_t *)data + full, len - full);
        return write_flash(fs, off + full, raw, fs->write_size);
    }
    return 0;
}

static int erase_bank(znvs_t *fs, uint32_t bank)
{
    if (fs->cfg->erase(fs->arg, bank, fs->bank_size) || check_erased(fs, bank, fs->bank_size)) {
        return fail(fs, ZNVS_EIO);
    }
    return 0;
}

static int publish_bank(znvs_t *fs, uint32_t bank, uint32_t sequence)
{
    uint8_t raw[HEADER_BYTES];
    encode(raw, format_magic(fs), sequence);
    memcpy(raw + HEADER_COPY_BYTES, raw, HEADER_COPY_BYTES);
    /* Every supported write granule divides HEADER_BYTES. */
    return write_flash(fs, bank, raw, sizeof(raw));
}

/* Reserve before writing data. The separate commit block is written last. */
static int append(znvs_t *fs, struct znvs_pos *pos, const struct entry *entry, const void *data)
{
    uint8_t raw[IO_BYTES];
    uint32_t addr = pos->ate - slot_size(fs);
    uint32_t offset = pos->data;
    uint32_t done = 0;
    uint32_t crc;
    encode(raw, entry->id | (uint32_t)entry->len << 16, offset);
    /* ID/length is the common prefix of both CRCs. */
    crc = crc32(UINT32_MAX, raw, 4);
    put32(raw + 16, ~crc32(crc, raw + 4, 4));
    if (program(fs, addr, raw, META_BYTES)) {
        return ZNVS_EIO;
    }
    while (done < entry->len) {
        uint32_t n = entry->len - done;
        const void *chunk = data;
        if (!data) {
            if (n > IO_BYTES) {
                n = IO_BYTES;
            }
            if (read_flash(fs, entry->offset + done, raw, n)) {
                return ZNVS_EIO;
            }
            chunk = raw;
        }
        crc = crc32(crc, chunk, n);
        if (program(fs, offset + done, chunk, n)) {
            return ZNVS_EIO;
        }
        done += n;
    }
    if (entry->len) {
        if (!data) {
            if (read_flash(fs, entry->offset + align_size(fs, entry->len), raw, 4)) {
                return ZNVS_EIO;
            }
            if (get32(raw) != ~crc) {
                return fail(fs, ZNVS_ECORRUPT);
            }
        }
        put32(raw, ~crc);
        if (program(fs, offset + align_size(fs, entry->len), raw, 4)) {
            return ZNVS_EIO;
        }
    }
    raw[0] = 0;
    memset(raw + 1, 0xff, fs->write_size - 1);
    if (write_flash(fs, addr + meta_size(fs), raw, fs->write_size)) {
        return ZNVS_EIO;
    }
    pos->ate = addr;
    pos->data += data_size(fs, entry->len);
    return 0;
}

static int next_live(znvs_t *fs, uint32_t *cursor, uint16_t exclude, struct entry *entry, uint8_t *seen)
{
    struct entry latest;
    uint32_t found;
    int rc;
    while (*cursor < fs->bank + fs->bank_size) {
        uint32_t addr = *cursor;
        *cursor += slot_size(fs);
        rc = load_entry(fs, addr, entry);
        if (rc < 0) {
            return rc;
        }
        if (rc != COMMITTED || entry->id == exclude) {
            continue;
        }
#if ZNVS_PROFILE == 0
        {
            uint32_t slot = (entry->id >> 3) & (GC_FILTER_BYTES - 1);
            uint8_t mask = (uint8_t)(1U << (entry->id & 7));
            int first = !(seen[slot] & mask);
            seen[slot] |= mask;
            /* A clear bit proves this ID has not appeared in the newest-first
             * scan. Collisions fall back to an exact lookup, including deletes. */
            if (first) {
                if (entry->len) {
                    return 1;
                }
                continue;
            }
        }
#else
        (void)seen;
#endif
        if (!entry->len) {
            continue;
        }
        rc = find_entry(fs, entry->id, 0, &latest, &found);
        if (rc) {
            return rc;
        }
        if (addr == found) {
            return 1;
        }
    }
    return 0;
}

static int collect(znvs_t *fs, const struct entry *pending, const void *data)
{
    struct znvs_pos pos;
    struct entry entry;
    uint32_t bank_size = fs->bank_size;
    uint32_t bank = fs->bank ^ bank_size;
    uint32_t cursor = fs->pos.ate;
    uint32_t used = HEADER_BYTES;
#if ZNVS_PROFILE == 0
    uint8_t seen[GC_FILTER_BYTES];
#else
    uint8_t *seen = NULL;
#endif
    int rc;
    unsigned pass;
    pos.data = bank + HEADER_BYTES;
    pos.ate = bank + bank_size;
    if (pending->len) {
        used += slot_size(fs) + data_size(fs, pending->len);
    }
    /* First prove that the snapshot fits, then erase and copy it. */
    for (pass = 0; pass < 2; ++pass) {
        cursor = fs->pos.ate;
#if ZNVS_PROFILE == 0
        memset(seen, 0, sizeof(seen));
#endif
        while ((rc = next_live(fs, &cursor, pending->id, &entry, seen)) > 0) {
            if (pass) {
                rc = append(fs, &pos, &entry, NULL);
                if (rc) {
                    return rc;
                }
            } else {
                used += slot_size(fs) + data_size(fs, entry.len);
                if (used > bank_size) {
                    return ZNVS_ENOSPC;
                }
            }
        }
        if (rc < 0) {
            return rc;
        }
        if (!pass && erase_bank(fs, bank)) {
            return ZNVS_EIO;
        }
    }
    if (!rc && pending->len) {
        rc = append(fs, &pos, pending, data);
    }
    if (!rc) {
        rc = publish_bank(fs, bank, fs->sequence + 1);
    }
    if (!rc) {
        fs->bank = bank;
        ++fs->sequence;
        fs->pos = pos;
    }
    return rc;
}
#endif

static int ready(const znvs_t *fs)
{
    return !fs ? ZNVS_EINVAL : (fs->ready ? 0 : ZNVS_ESTATE);
}

static int valid_config(const znvs_cfg_t *cfg)
{
    uint32_t w, bank;
    if (!cfg || !cfg->read || (ZNVS_PROFILE != 2 && (!cfg->write || !cfg->erase))) {
        return 0;
    }
    w = cfg->write_size;
    bank = cfg->size / 2;
    return w && !(w & (w - 1)) && w <= IO_BYTES && cfg->erase_size && !(cfg->erase_size & (cfg->erase_size - 1)) && !(cfg->size & 1) && !(bank & (w - 1)) && !(bank & (cfg->erase_size - 1));
}

int znvs_init(znvs_t *fs, const znvs_cfg_t *cfg, void *arg)
{
    if (!fs) {
        return ZNVS_EINVAL;
    }
    fs->cfg = NULL;
    fs->ready = 0;
    if (!valid_config(cfg)) {
        return ZNVS_EINVAL;
    }
    fs->bank_size = cfg->size / 2;
    fs->write_size = (uint8_t)cfg->write_size;
    fs->slot_size = (uint8_t)(align_size(fs, META_BYTES) + fs->write_size);
    fs->crc_size = (uint8_t)align_size(fs, 4);
    if (fs->bank_size < (uint32_t)HEADER_BYTES + fs->slot_size + fs->write_size + fs->crc_size) {
        return ZNVS_EINVAL;
    }
    fs->cfg = cfg;
    fs->arg = arg;
    return znvs_mount(fs);
}

int znvs_mount(znvs_t *fs)
{
    struct entry entry;
    uint32_t sequence[2];
    uint32_t bank_size;
    int a, b, rc;
    if (!fs || !fs->cfg) {
        return fs ? ZNVS_ESTATE : ZNVS_EINVAL;
    }
    fs->ready = 0;
    bank_size = fs->bank_size;
    a = bank_header(fs, 0, &sequence[0]);
    if (a < 0) {
        return a;
    }
    b = bank_header(fs, bank_size, &sequence[1]);
    if (b < 0) {
        return b;
    }
    if (!a && !b) {
#if ZNVS_PROFILE != 0
        return ZNVS_ECORRUPT;
#else
        uint8_t raw[HEADER_BYTES], expected[HEADER_BYTES];
        unsigned i;
        /* Only blank media or a torn first header may be initialized. */
        if ((rc = check_erased(fs, HEADER_BYTES, fs->cfg->size - HEADER_BYTES)) != 0 || read_flash(fs, 0, raw, sizeof(raw))) {
            return rc ? rc : ZNVS_EIO;
        }
        encode(expected, format_magic(fs), 0);
        memcpy(expected + HEADER_COPY_BYTES, expected, HEADER_COPY_BYTES);
        for (i = 0; i < sizeof(raw); ++i) {
            if ((raw[i] & expected[i]) != expected[i]) {
                return ZNVS_ECORRUPT;
            }
        }
        if (!erased(raw, sizeof(raw)) && erase_bank(fs, 0)) {
            return ZNVS_EIO;
        }
        if (publish_bank(fs, 0, 0)) {
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
    fs->pos.data = fs->bank + HEADER_BYTES;
    fs->pos.ate = fs->bank + bank_size;
    while (fs->pos.ate - fs->pos.data >= slot_size(fs)) {
        rc = load_entry(fs, fs->pos.ate - slot_size(fs), &entry);
        if (rc < 0) {
            return rc;
        }
        if (rc == EMPTY) {
            break;
        }
        fs->pos.ate -= slot_size(fs);
        if (rc >= RESERVED) {
            uint32_t end = entry.offset + data_size(fs, entry.len);
            if (end > fs->pos.data) {
                fs->pos.data = end;
            }
        }
    }
    fs->ready = 1;
    return 0;
}

static size_t max_payload(const znvs_t *fs)
{
    uint32_t bytes;
    bytes = fs->bank_size - HEADER_BYTES - slot_size(fs) - fs->crc_size;
    return bytes > UINT16_MAX ? UINT16_MAX : bytes;
}

size_t znvs_max_size(const znvs_t *fs)
{
    return fs && fs->cfg ? max_payload(fs) : 0;
}

#if ZNVS_PROFILE == 0
size_t znvs_available(const znvs_t *fs)
{
    uint32_t bytes;
    if (ready(fs)) {
        return 0;
    }
    bytes = fs->pos.ate - fs->pos.data;
    if (bytes <= slot_size(fs) + fs->crc_size) {
        return 0;
    }
    bytes -= slot_size(fs) + fs->crc_size;
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
    struct entry entry;
    uint32_t addr;
    int rc;
    if (length) {
        *length = 0;
    }
    if ((rc = ready(fs)) != 0) {
        return rc;
    }
    if (id == UINT16_MAX || (!data && (capacity || !length))) {
        return ZNVS_EINVAL;
    }
    if ((rc = find_entry(fs, id, history, &entry, &addr)) != 0) {
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
    if (read_flash(fs, entry.offset, data, entry.len)) {
        return ZNVS_EIO;
    }
    {
        uint8_t raw[4];
        if (read_flash(fs, entry.offset + align_size(fs, entry.len), raw, sizeof(raw))) {
            return ZNVS_EIO;
        }
        if (get32(raw) != ~crc32(entry_crc(&entry), data, entry.len)) {
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
    struct entry entry;
#if ZNVS_PROFILE == 0
    uint8_t raw[IO_BYTES];
    uint32_t done = 0;
#endif
    uint32_t addr;
    int rc = ready(fs);
    if (rc) {
        return rc;
    }
    if (id == UINT16_MAX || (!data && len) || len > max_payload(fs)) {
        return ZNVS_EINVAL;
    }
    if (ZNVS_PROFILE == 0 || !len) {
        rc = find_entry(fs, id, 0, &entry, &addr);
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
            if (read_flash(fs, entry.offset + done, raw, n)) {
                return ZNVS_EIO;
            }
            if (memcmp(raw, (const uint8_t *)data + done, n)) {
                break;
            }
            done += n;
        }
        if (done == len) {
            if (len) {
                if (read_flash(fs, entry.offset + align_size(fs, entry.len), raw, 4)) {
                    return ZNVS_EIO;
                }
                if (get32(raw) == ~crc32(entry_crc(&entry), data, len)) {
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
    if (slot_size(fs) + data_size(fs, entry.len) <= fs->pos.ate - fs->pos.data) {
        rc = append(fs, &fs->pos, &entry, data);
        return rc;
    }
    return collect(fs, &entry, data);
}

int znvs_delete(znvs_t *fs, uint16_t id)
{
    return znvs_write(fs, id, NULL, 0);
}

#if ZNVS_PROFILE == 0
int znvs_rotate(znvs_t *fs)
{
    const struct entry pending = {0, UINT16_MAX, 0};
    int rc = ready(fs);
    return rc ? rc : collect(fs, &pending, NULL);
}

int znvs_format(znvs_t *fs)
{
    int rc;
    if (!fs || !fs->cfg) {
        return fs ? ZNVS_ESTATE : ZNVS_EINVAL;
    }
    fs->ready = 0;
    rc = erase_bank(fs, 0);
    if (!rc) {
        rc = erase_bank(fs, fs->bank_size);
    }
    return rc ? rc : znvs_mount(fs);
}
#endif
#endif

#if ZNVS_PROFILE == 0
void *znvs_arg(const znvs_t *fs)
{
    return fs && fs->cfg ? fs->arg : NULL;
}
#endif
