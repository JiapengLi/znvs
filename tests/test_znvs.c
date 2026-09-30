/* Host-only NOR simulator and regression tests. SPDX-License-Identifier: Apache-2.0 */
#include "znvs.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FLASH (4U * 65536U)
#define GUARD 32U
#define KEYS 12U
#define VALUE_MAX 80U
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s [test=%s cut=%" PRId64 " mask=%02x]\n", \
                    __FILE__, __LINE__, #x, test_name, cut_label, mask_label);      \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

static uint64_t checks, fault_cases, random_steps;
static const char *test_name = "startup";
static int64_t cut_label = -1;
static unsigned mask_label;

typedef struct {
    uint8_t raw[MAX_FLASH + 2 * GUARD];
    uint32_t size, erase_size;
    uint16_t write_size;
    int dead, read_fail, positive_error;
    int64_t cut;
    uint8_t torn_mask;
    uint64_t reads, read_bytes, writes, erases, mutated;
    int operation_cut;
    uint32_t tear_seed;
} flash_t;

typedef struct {
    size_t len;
    uint8_t data[VALUE_MAX];
} value_t;

/* All large memory below belongs to the TEST, never to the library. */
static flash_t flash, saved, probe, recovery_image;
static uint8_t bigbuf[65536], bigread[65536];

static uint8_t *bytes(flash_t *f)
{
    return f->raw + GUARD;
}
static void bounds(flash_t *f, uint32_t off, size_t len)
{
    CHECK(off <= f->size && len <= f->size - off);
}
static int unordered_cut(flash_t *f, uint32_t off, const uint8_t *src, size_t len)
{
    size_t i;
    if (f->operation_cut < 0) {
        return 0;
    }
    if (f->operation_cut-- != 0) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        uint8_t mask;
        f->tear_seed = f->tear_seed * 1664525U + 1013904223U;
        mask = (uint8_t)(f->tear_seed >> 24);
        if (src) {
            bytes(f)[off + i] &= (uint8_t)(src[i] | mask);
        } else {
            bytes(f)[off + i] |= mask;
        }
    }
    f->dead = 1;
    return 1;
}
static int sim_read(void *arg, uint32_t off, void *buf, size_t len)
{
    flash_t *f = (flash_t *)arg;
    bounds(f, off, len);
    ++f->reads;
    f->read_bytes += len;
    if (f->dead || f->read_fail == 0) {
        f->dead = 1;
        return f->positive_error ? 7 : -99;
    }
    if (f->read_fail > 0) {
        --f->read_fail;
    }
    memcpy(buf, bytes(f) + off, len);
    return 0;
}
static int sim_write(void *arg, uint32_t off, const void *buf, size_t len)
{
    flash_t *f = (flash_t *)arg;
    const uint8_t *p = (const uint8_t *)buf;
    size_t i;
    bounds(f, off, len);
    CHECK((off % f->write_size) == 0 && (len % f->write_size) == 0);
    ++f->writes;
    if (f->dead) {
        return -99;
    }
    if (unordered_cut(f, off, p, len)) {
        return -99;
    }
    for (i = 0; i < len; ++i) {
        CHECK((bytes(f)[off + i] & p[i]) == p[i]);
        if (f->cut == 0) {
            /* Selected bit subset of the next byte may also have been programmed. */
            bytes(f)[off + i] &= (uint8_t)(p[i] | (uint8_t)~f->torn_mask);
            f->dead = 1;
            return f->positive_error ? 7 : -99;
        }
        if (f->cut > 0) {
            --f->cut;
        }
        bytes(f)[off + i] &= p[i];
        ++f->mutated;
    }
    return 0;
}
static int sim_erase(void *arg, uint32_t off, size_t len)
{
    flash_t *f = (flash_t *)arg;
    size_t i;
    bounds(f, off, len);
    CHECK((off % f->erase_size) == 0 && (len % f->erase_size) == 0);
    ++f->erases;
    if (f->dead) {
        return -99;
    }
    if (unordered_cut(f, off, NULL, len)) {
        return -99;
    }
    for (i = 0; i < len; ++i) {
        if (f->cut == 0) {
            bytes(f)[off + i] |= f->torn_mask;
            f->dead = 1;
            return f->positive_error ? 7 : -99;
        }
        if (f->cut > 0) {
            --f->cut;
        }
        bytes(f)[off + i] = 0xff;
        ++f->mutated;
    }
    return 0;
}
static znvs_cfg_t config(uint32_t sector, uint32_t count, uint16_t wbs)
{
    znvs_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.read = sim_read;
    c.write = sim_write;
    c.erase = sim_erase;
    c.size = sector * count;
    c.erase_size = sector / 2U;
    c.write_size = wbs;
    return c;
}
static void fresh(flash_t *f, const znvs_cfg_t *c)
{
    memset(f, 0, sizeof(*f));
    f->size = c->size;
    f->erase_size = c->erase_size;
    f->write_size = c->write_size;
    f->cut = -1;
    f->read_fail = -1;
    f->operation_cut = -1;
    memset(f->raw, 0xa5, sizeof(f->raw));
    memset(bytes(f), 0xff, c->size);
}
static void recover_device(flash_t *f)
{
    f->cut = -1;
    f->read_fail = -1;
    f->dead = 0;
    f->torn_mask = 0;
    f->operation_cut = -1;
}
static void guards(flash_t *f)
{
    unsigned i;
    for (i = 0; i < GUARD; ++i) {
        CHECK(f->raw[i] == 0xa5);
        CHECK(bytes(f)[f->size + i] == 0xa5);
    }
}
static void expect(znvs_t *fs, uint16_t id, const void *data, size_t len)
{
    uint8_t buf[VALUE_MAX + 1];
    size_t n = 999;
    int rc = znvs_read(fs, id, buf, sizeof(buf), &n);
    if (len == 0) {
        CHECK(rc == ZNVS_ENOENT && n == 0);
    } else {
        CHECK(rc == 0 && n == len && memcmp(data, buf, len) == 0);
    }
}
static void test_basic(void)
{
    znvs_cfg_t c = config(1024, 3, 4);
    znvs_t fs, reboot, zero = {0};
    size_t n;
    uint64_t wr;
    uint8_t small[3] = {0xa5, 0xa5, 0xa5};
    static const uint8_t a[] = "123456789";
    static const uint8_t b[] = "second value";
    test_name = "basic/API/history/CRC";
    fresh(&flash, &c);
    CHECK(znvs_init(NULL, &c, &flash) == ZNVS_EINVAL);
    CHECK(znvs_mount(&zero) == ZNVS_ESTATE);
    CHECK(znvs_read(&zero, 0, small, sizeof(small), &n) == ZNVS_ESTATE);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_arg(&fs) == &flash && znvs_arg(NULL) == NULL);
    CHECK(znvs_max_size(&fs) == c.size / 2U - 32U - 20U - 4U);
    CHECK(znvs_available(&fs) <= znvs_max_size(&fs));
    expect(&fs, 0, NULL, 0);
    CHECK(znvs_write(&fs, 0, a, sizeof(a) - 1) == 0);
    CHECK(znvs_write(&fs, ZNVS_ID_MAX, b, sizeof(b)) == 0);
    expect(&fs, 0, a, sizeof(a) - 1);
    expect(&fs, ZNVS_ID_MAX, b, sizeof(b));
    n = 0;
    CHECK(znvs_read(&fs, 0, NULL, 0, &n) == 0 && n == sizeof(a) - 1);
    CHECK(znvs_read(&fs, 0, small, sizeof(small), &n) == ZNVS_ENOSPC);
    CHECK(n == sizeof(a) - 1 && small[0] == 0xa5 && small[2] == 0xa5);
    CHECK(znvs_read(&fs, 0, NULL, 1, &n) == ZNVS_EINVAL);
    CHECK(znvs_read(&fs, 0, NULL, 0, NULL) == ZNVS_EINVAL);
    CHECK(znvs_write(&fs, 65535, a, sizeof(a)) == ZNVS_EINVAL);
    CHECK(znvs_write(&fs, 0, a, SIZE_MAX) == ZNVS_EINVAL);
    CHECK(znvs_write(&fs, 0, NULL, 1) == ZNVS_EINVAL);
    CHECK(znvs_read(&fs, 65535, small, sizeof(small), &n) == ZNVS_EINVAL);
    wr = flash.writes;
    CHECK(znvs_write(&fs, 0, a, sizeof(a) - 1) == 0 && flash.writes == wr);
    CHECK(znvs_write(&fs, 0, b, sizeof(b)) == 0);
    CHECK(znvs_read_hist(&fs, 0, 1, bigread, sizeof(bigread), &n) == 0);
    CHECK(n == sizeof(a) - 1 && memcmp(bigread, a, n) == 0);
    CHECK(znvs_read_hist(&fs, 0, 2, bigread, sizeof(bigread), &n) == ZNVS_ENOENT);
    CHECK(znvs_read_hist(&fs, 0, UINT16_MAX, bigread, sizeof(bigread), &n) == ZNVS_ENOENT);
    CHECK(znvs_delete(&fs, 0) == 0);
    expect(&fs, 0, NULL, 0);
    CHECK(znvs_read_hist(&fs, 0, 1, bigread, sizeof(bigread), &n) == 0);
    CHECK(n == sizeof(b) && memcmp(bigread, b, n) == 0);
    wr = flash.writes;
    CHECK(znvs_delete(&fs, 0) == 0 && znvs_delete(&fs, 1234) == 0);
    CHECK(flash.writes == wr);
    CHECK(znvs_init(&reboot, &c, &flash) == 0);
    expect(&reboot, 0, NULL, 0);
    expect(&reboot, ZNVS_ID_MAX, b, sizeof(b));
    CHECK(znvs_format(&reboot) == 0);
    expect(&reboot, ZNVS_ID_MAX, NULL, 0);
    CHECK(znvs_write(&reboot, 7, a, sizeof(a) - 1) == 0);
    /* IEEE CRC32 of LE id/length followed by payload, padded tail location. */
    CHECK(bytes(&flash)[44] == 0xd7 && bytes(&flash)[45] == 0x53);
    CHECK(bytes(&flash)[46] == 0xb7 && bytes(&flash)[47] == 0x24);
    bytes(&flash)[44] ^= 1;
    CHECK(znvs_read(&reboot, 7, bigread, sizeof(bigread), &n) == ZNVS_ECORRUPT);
    wr = flash.writes;
    CHECK(znvs_write(&reboot, 7, a, sizeof(a) - 1) == 0 && flash.writes > wr);
    expect(&reboot, 7, a, sizeof(a) - 1);
    /* Damage the newest payload and ensure a complete read detects it. */
    bytes(&flash)[48] ^= 1;
    CHECK(znvs_read(&reboot, 7, bigread, sizeof(bigread), &n) == ZNVS_ECORRUPT);
    guards(&flash);
}
static void test_geometry(void)
{
    znvs_cfg_t c = config(1024, 3, 4), bad;
    znvs_t fs;
    unsigned i;
    test_name = "geometry/64KiB/max-payload";
    fresh(&flash, &c);
    for (i = 0; i < 12; ++i) {
        bad = c;
        switch (i) {
        case 0:
            bad.write_size = 0;
            break;
        case 1:
            bad.write_size = 3;
            break;
        case 2:
            bad.size = 0;
            break;
        case 3:
            bad.write_size = 64;
            break;
        case 4:
            bad.erase_size = 1000;
            break;
        case 5:
            bad.erase_size = 0;
            break;
        case 6:
            bad.erase_size = 2048;
            break;
        case 7:
            bad.size = 32;
            break;
        case 8:
            bad.size = 3073;
            break;
        case 9:
            bad.read = NULL;
            break;
        case 10:
            bad.write = NULL;
            break;
        default:
            bad.erase = NULL;
            break;
        }
        CHECK(znvs_init(&fs, &bad, &flash) == ZNVS_EINVAL);
        CHECK(flash.reads == 0 && flash.writes == 0 && flash.erases == 0);
    }
    CHECK(znvs_init(&fs, NULL, &flash) == ZNVS_EINVAL);
    for (i = 1; i <= 32; i *= 2) {
        size_t n, len;
        c = config(65536, 2, (uint16_t)i);
        fresh(&flash, &c);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        len = znvs_max_size(&fs);
        memset(bigbuf, 0x36, sizeof(bigbuf));
        CHECK(znvs_write(&fs, 0, bigbuf, len + 1) == ZNVS_EINVAL);
        CHECK(znvs_write(&fs, 0, bigbuf, len) == 0);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        CHECK(znvs_read(&fs, 0, bigread, sizeof(bigread), &n) == 0);
        CHECK(n == len && memcmp(bigbuf, bigread, len) == 0);
        bigbuf[0]++;
        CHECK(znvs_write(&fs, 0, bigbuf, len) == 0); /* GC of a maximum record */
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        CHECK(znvs_read(&fs, 0, bigread, sizeof(bigread), &n) == 0);
        CHECK(n == len && memcmp(bigbuf, bigread, len) == 0);
        CHECK(znvs_delete(&fs, 0) == 0);
        expect(&fs, 0, NULL, 0);
        guards(&flash);
    }
}
static uint32_t rng(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}
static void test_random(void)
{
    uint16_t wbs;
    uint32_t count;
    test_name = "random-model/GC";
    for (count = 2; count <= 4; ++count) {
        for (wbs = 1; wbs <= 32; wbs *= 2) {
            znvs_cfg_t c = config(wbs > 32 ? (uint32_t)wbs * 32U : 1024U, count, wbs);
            znvs_t fs;
            value_t model[KEYS] = {{0, {0}}};
            uint32_t seed = 0x91aec5d7U;
            unsigned step, k;
            fresh(&flash, &c);
            CHECK(znvs_init(&fs, &c, &flash) == 0);
            for (step = 0; step < 1800; ++step) {
                unsigned id = rng(&seed) % KEYS;
                size_t len = rng(&seed) % 55U;
                uint8_t buf[VALUE_MAX];
                int rc;
                for (k = 0; k < len; ++k) {
                    buf[k] = (uint8_t)rng(&seed);
                }
                rc = znvs_write(&fs, (uint16_t)id, buf, len);
                CHECK(rc == 0 || rc == ZNVS_ENOSPC);
                if (rc == 0) {
                    model[id].len = len;
                    memcpy(model[id].data, buf, len);
                }
                if (step % 17U == 0) {
                    CHECK(znvs_init(&fs, &c, &flash) == 0);
                }
                if (step % 23U == 0) {
                    for (k = 0; k < KEYS; ++k) {
                        expect(&fs, (uint16_t)k, model[k].data, model[k].len);
                    }
                }
                ++random_steps;
            }
            CHECK(flash.erases > 5);
            CHECK(znvs_init(&fs, &c, &flash) == 0);
            for (k = 0; k < KEYS; ++k) {
                expect(&fs, (uint16_t)k, model[k].data, model[k].len);
            }
            guards(&flash);
        }
    }
}
static void test_full_and_errors(void)
{
    znvs_cfg_t c = config(512, 2, 4);
    znvs_t fs;
    unsigned i, count;
    uint64_t erases, writes;
    size_t n;
    test_name = "full/deletion/I-O-failures/no-auto-format";
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    memset(bigbuf, 0x53, 60);
    for (i = 1; i < 100; ++i) {
        int rc = znvs_write(&fs, (uint16_t)i, bigbuf, 60);
        if (rc == ZNVS_ENOSPC) {
            break;
        }
        CHECK(rc == 0);
    }
    CHECK(i < 100 && i > 2);
    count = i - 1;
    erases = flash.erases;
    writes = flash.writes;
    CHECK(znvs_write(&fs, 300, bigbuf, 60) == ZNVS_ENOSPC);
    CHECK(flash.erases == erases);
    CHECK(flash.writes == writes);
    for (i = 1; i <= count; ++i) {
        expect(&fs, (uint16_t)i, bigbuf, 60);
    }
    CHECK(znvs_delete(&fs, 1) == 0);
    CHECK(znvs_write(&fs, 300, bigbuf, 60) == 0);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    expect(&fs, 1, NULL, 0);
    for (i = 0; i < 100; ++i) {
        bigbuf[0] = (uint8_t)i;
        CHECK(znvs_write(&fs, 300, bigbuf, 60) == 0);
        expect(&fs, 1, NULL, 0);
    }
    flash.read_fail = 0;
    flash.positive_error = 1;
    CHECK(znvs_read(&fs, 300, bigread, sizeof(bigread), &n) == ZNVS_EIO);
    CHECK(znvs_write(&fs, 300, bigbuf, 60) == ZNVS_ESTATE);
    recover_device(&flash);
    CHECK(znvs_mount(&fs) == 0);
    expect(&fs, 300, bigbuf, 60);
    /* Read failures at each early mount read must not be treated as closed sectors. */
    saved = flash;
    probe = saved;
    CHECK(znvs_init(&fs, &c, &probe) == 0);
    for (i = 0; i < probe.reads - saved.reads; ++i) {
        flash = saved;
        erases = flash.erases;
        writes = flash.writes;
        flash.read_fail = (int)i;
        CHECK(znvs_init(&fs, &c, &flash) == ZNVS_EIO);
        CHECK(flash.erases == erases && flash.writes == writes);
        recover_device(&flash);
        CHECK(znvs_mount(&fs) == 0);
    }
    /* Invalid media: preserve it rather than implicitly formatting it. */
    fresh(&flash, &c);
    memset(bytes(&flash), 0, c.size);
    CHECK(znvs_init(&fs, &c, &flash) == ZNVS_ECORRUPT);
    CHECK(flash.writes == 0 && flash.erases == 0);
    CHECK(znvs_format(&fs) == 0);
    expect(&fs, 300, NULL, 0);
    guards(&flash);
}

/* Fault campaigns operate on saved flash bytes, not a saved RAM filesystem state. */
static void check_power_state(znvs_t *fs, const value_t before[KEYS],
                              unsigned target, const value_t *after, int committed)
{
    unsigned k;
    for (k = 0; k < KEYS; ++k) {
        if (k == target) {
            uint8_t buf[VALUE_MAX];
            size_t n;
            int rc = znvs_read(fs, (uint16_t)k, buf, sizeof(buf), &n);
            int old_ok = before[k].len == 0 ? rc == ZNVS_ENOENT : rc == 0 && n == before[k].len && memcmp(buf, before[k].data, n) == 0;
            int new_ok = after->len == 0 ? rc == ZNVS_ENOENT : rc == 0 && n == after->len && memcmp(buf, after->data, n) == 0;
            CHECK(committed ? new_ok : (old_ok || new_ok));
        } else {
            expect(fs, (uint16_t)k, before[k].data, before[k].len);
        }
    }
}
static void sweep_recovery(const znvs_cfg_t *c, const value_t before[KEYS], unsigned target, const value_t *after)
{
    znvs_t fs;
    uint64_t reads, start;
    unsigned cut;
    probe = recovery_image;
    recover_device(&probe);
    start = probe.reads;
    CHECK(znvs_init(&fs, c, &probe) == 0);
    reads = probe.reads - start;
    for (cut = 0; cut < reads; ++cut) {
        flash = recovery_image;
        recover_device(&flash);
        start = flash.mutated;
        flash.read_fail = (int)cut;
        CHECK(znvs_init(&fs, c, &flash) == ZNVS_EIO);
        CHECK(flash.mutated == start);
        recover_device(&flash);
        CHECK(znvs_init(&fs, c, &flash) == 0);
        CHECK(flash.mutated == start);
        check_power_state(&fs, before, target, after, 0);
        ++fault_cases;
    }
}
static void sweep_operation(const znvs_cfg_t *c, const value_t before[KEYS],
                            unsigned target, const value_t *after, int rotate,
                            int nested)
{
    znvs_t fs;
    uint64_t start, total;
    int64_t cut;
    unsigned mode;
    int have_recovery = 0;
    probe = saved;
    recover_device(&probe);
    CHECK(znvs_init(&fs, c, &probe) == 0);
    start = probe.mutated;
    CHECK((rotate ? znvs_rotate(&fs) : znvs_write(&fs, (uint16_t)target, after->data, after->len)) == 0);
    total = probe.mutated - start;
    CHECK(total > 0);
    for (mode = 0; mode < 3; ++mode) {
        for (cut = 0; cut <= (int64_t)total; ++cut) {
            int rc;
            cut_label = cut;
            mask_label = mode == 0 ? 0 : mode == 1 ? 0x55
                                                   : 0xaa;
            flash = saved;
            recover_device(&flash);
            CHECK(znvs_init(&fs, c, &flash) == 0);
            flash.cut = cut;
            flash.torn_mask = (uint8_t)mask_label;
            rc = rotate ? znvs_rotate(&fs) : znvs_write(&fs, (uint16_t)target, after->data, after->len);
            CHECK(rc == 0 || rc == ZNVS_EIO);
            if (rc == ZNVS_EIO) {
                CHECK(!fs.ready);
                CHECK(znvs_delete(&fs, 655) == ZNVS_ESTATE);
            }
            if (nested && !have_recovery && rc == ZNVS_EIO && cut > 16) {
                probe = flash;
                recover_device(&probe);
                start = probe.mutated;
                CHECK(znvs_init(&fs, c, &probe) == 0);
                CHECK(probe.mutated == start);
                {
                    recovery_image = flash;
                    have_recovery = 1;
                }
            }
            recover_device(&flash);
            CHECK(znvs_init(&fs, c, &flash) == 0);
            check_power_state(&fs, before, target, after, rotate ? 0 : rc == 0);
            guards(&flash);
            ++fault_cases;
        }
    }
    if (nested) {
        CHECK(have_recovery);
        sweep_recovery(c, before, target, after);
    }
    cut_label = -1;
    mask_label = 0;
}
static void test_power_loss(void)
{
    uint16_t wbs;
    for (wbs = 1; wbs <= 32; wbs *= 2) {
        znvs_cfg_t c = config(wbs > 32 ? (uint32_t)wbs * 32U : 1024U, 2, wbs);
        value_t model[KEYS] = {{0, {0}}}, after;
        znvs_t fs;
        unsigned i, variant;
        test_name = "power/append";
        fresh(&flash, &c);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        for (i = 0; i < 5; ++i) {
            model[i].len = 23U + i;
            memset(model[i].data, (int)(0x30U + i), model[i].len);
            CHECK(znvs_write(&fs, (uint16_t)i, model[i].data, model[i].len) == 0);
        }
        after.len = 41;
        memset(after.data, 0x96, after.len);
        saved = flash;
        sweep_operation(&c, model, 0, &after, 0, 0);
        for (variant = 0; variant < 3; ++variant) {
            test_name = variant == 0 ? "power/GC-smaller" : variant == 1 ? "power/GC-larger"
                                                                         : "power/delete";
            fresh(&flash, &c);
            CHECK(znvs_init(&fs, &c, &flash) == 0);
            for (i = 0; i < 5; ++i) {
                model[i].len = 23U + i;
                memset(model[i].data, (int)(0x30U + i), model[i].len);
                CHECK(znvs_write(&fs, (uint16_t)i, model[i].data, model[i].len) == 0);
            }
            after.len = variant == 0 ? 11 : variant == 1 ? 61
                                                         : 0;
            memset(after.data, 0x96, sizeof(after.data));
            if (variant < 2) {
                /* Make the next write rotate, while cold keys still need copying. */
                unsigned limit = 0;
                while (znvs_available(&fs) >= after.len) {
                    size_t avail = znvs_available(&fs);
                    model[0].len = avail < 23 ? avail : 23;
                    ++model[0].data[0];
                    CHECK(model[0].len > 0 && ++limit < 200);
                    CHECK(znvs_write(&fs, 0, model[0].data, model[0].len) == 0);
                }
            }
            saved = flash;
            sweep_operation(&c, model, 0, &after, 0, variant == 1 && wbs == 4);
        }
        test_name = "power/manual-rotate";
        flash = saved;
        recover_device(&flash);
        after = model[0];
        sweep_operation(&c, model, 0, &after, 1, 0);
    }
}
#include "test_v2_cases.h"

int main(void)
{
    printf("ZNVS test: fixed CRC32, sizeof(znvs_t)=%zu cfg=%zu\n", sizeof(znvs_t), sizeof(znvs_cfg_t));
    fflush(stdout);
    test_basic();
    test_geometry();
    test_random();
    test_full_and_errors();
    test_power_loss();
    test_v2();
    printf("PASS checks=%" PRIu64 " random_operations=%" PRIu64
           " fault_scenarios=%" PRIu64 "\n",
           checks, random_steps, fault_cases);
    return 0;
}
