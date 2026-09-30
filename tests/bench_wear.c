/* Reproducible erase/write counts against the unmodified upstream algorithm.
 * Host NOR simulation, not hardware latency or endurance qualification.
 * SPDX-License-Identifier: Apache-2.0 */
#include "znvs.h"
#include "ref_shim.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    PAGE = 4096,
    TOTAL = 8192,
    WRITE_SIZE = 4,
    KEYS = 8,
    WARMUP = 2000,
    SAMPLE = 20000
};
#define CHECK(x)                                                                   \
    do {                                                                           \
        if (!(x)) {                                                                \
            fprintf(stderr, "wear benchmark failure line %d: %s\n", __LINE__, #x); \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

struct counters {
    uint64_t reads, read_bytes, writes, write_bytes, erases, erase_bytes, page_erases[2];
};

struct sim {
    uint8_t bytes[TOTAL];
    struct counters stats;
};

static struct sim sim;

static int rd(void *arg, uint32_t addr, void *buf, size_t len)
{
    struct sim *s = arg;
    CHECK(addr <= TOTAL && len <= TOTAL - addr);
    ++s->stats.reads;
    s->stats.read_bytes += len;
    memcpy(buf, s->bytes + addr, len);
    return 0;
}

static int wr(void *arg, uint32_t addr, const void *buf, size_t len)
{
    struct sim *s = arg;
    const uint8_t *p = buf;
    size_t i;
    CHECK(addr <= TOTAL && len <= TOTAL - addr && addr % WRITE_SIZE == 0 && len % WRITE_SIZE == 0);
    ++s->stats.writes;
    s->stats.write_bytes += len;
    for (i = 0; i < len; ++i) {
        CHECK((s->bytes[addr + i] & p[i]) == p[i]);
        s->bytes[addr + i] &= p[i];
    }
    return 0;
}

static int er(void *arg, uint32_t addr, size_t len)
{
    struct sim *s = arg;
    size_t i;
    CHECK(addr <= TOTAL && len <= TOTAL - addr && addr % PAGE == 0 && len % PAGE == 0);
    ++s->stats.erases;
    s->stats.erase_bytes += len;
    for (i = addr / PAGE; i < (addr + len) / PAGE; ++i) {
        ++s->stats.page_erases[i];
    }
    memset(s->bytes + addr, 0xff, len);
    return 0;
}

static void print_stats(const char *impl, const char *pattern, const char *phase, unsigned updates)
{
    const struct counters *c = &sim.stats;
    printf("impl=%s pattern=%s phase=%s updates=%u reads=%" PRIu64 " read_bytes=%" PRIu64 " writes=%" PRIu64 " write_bytes=%" PRIu64 " erases=%" PRIu64 " erase_bytes=%" PRIu64 " page_erases=[%" PRIu64 ",%" PRIu64 "]\n", impl, pattern, phase, updates, c->reads, c->read_bytes, c->writes, c->write_bytes, c->erases, c->erase_bytes, c->page_erases[0], c->page_erases[1]);
}

static void run_case(unsigned upstream, unsigned hot)
{
    const znvs_cfg_t cfg = {rd, wr, er, TOTAL, PAGE, WRITE_SIZE};
    struct device device = {&sim, rd, wr, er, {WRITE_SIZE, 0xff}, PAGE};
    struct nvs_fs nvs = {0};
    znvs_t znvs;
    uint32_t expected[KEYS], value, actual;
    uint64_t before_erases, interval_sum = 0;
    unsigned i, phase, count, id, gc_count = 0, last_gc = 0, intervals = 0, min_interval = UINT32_MAX, max_interval = 0;
    const char *impl = upstream ? "Zephyr" : "ZNVS";
    const char *pattern = hot ? "one-hot-seven-cold" : "round-robin-eight";
    size_t length;
    memset(&sim, 0, sizeof(sim));
    memset(sim.bytes, 0xff, sizeof(sim.bytes));
    nvs.flash_device = &device;
    nvs.sector_size = PAGE;
    nvs.sector_count = 2;
    CHECK((upstream ? nvs_mount(&nvs) : znvs_init(&znvs, &cfg, &sim)) == 0);
    print_stats(impl, pattern, "initial-mount", 0);
    memset(&sim.stats, 0, sizeof(sim.stats));
    for (i = 0; i < KEYS; ++i) {
        expected[i] = UINT32_C(0x40000000) + i;
        CHECK(upstream ? nvs_write(&nvs, (uint16_t)i, expected + i, sizeof(value)) == sizeof(value) : znvs_write(&znvs, (uint16_t)i, expected + i, sizeof(value)) == 0);
    }
    print_stats(impl, pattern, "seed-eight-keys", KEYS);
    for (phase = 0; phase < 2; ++phase) {
        memset(&sim.stats, 0, sizeof(sim.stats));
        count = phase ? SAMPLE : WARMUP;
        for (i = 1; i <= count; ++i) {
            id = hot ? 0 : (i - 1) % KEYS;
            value = (phase ? WARMUP : 0) + i;
            expected[id] = value;
            before_erases = sim.stats.erases;
            CHECK(upstream ? nvs_write(&nvs, (uint16_t)id, &value, sizeof(value)) == sizeof(value) : znvs_write(&znvs, (uint16_t)id, &value, sizeof(value)) == 0);
            if (phase && sim.stats.erases != before_erases) {
                CHECK(sim.stats.erases == before_erases + 1);
                if (gc_count) {
                    unsigned interval = i - last_gc;
                    if (interval < min_interval) {
                        min_interval = interval;
                    }
                    if (interval > max_interval) {
                        max_interval = interval;
                    }
                    interval_sum += interval;
                    ++intervals;
                }
                last_gc = i;
                ++gc_count;
            }
        }
        print_stats(impl, pattern, phase ? "sample" : "warmup", count);
    }
    CHECK(intervals > 0);
    CHECK(sim.stats.page_erases[0] + sim.stats.page_erases[1] == sim.stats.erases);
    printf("impl=%s pattern=%s complete-GC-intervals=%u min=%u max=%u mean=%.6f sample-GC-events=%u program-bytes-per-update=%.6f payload-write-amplification=%.6f\n", impl, pattern, intervals, min_interval, max_interval, (double)interval_sum / intervals, gc_count, (double)sim.stats.write_bytes / SAMPLE, (double)sim.stats.write_bytes / (SAMPLE * sizeof(value)));
    /* Validate after reporting so verification reads do not alter sample counts. */
    for (i = 0; i < KEYS; ++i) {
        CHECK(upstream ? nvs_read(&nvs, (uint16_t)i, &actual, sizeof(actual)) == sizeof(actual) : znvs_read(&znvs, (uint16_t)i, &actual, sizeof(actual), &length) == 0 && length == sizeof(actual));
        CHECK(actual == expected[i]);
    }
}

int main(void)
{
    unsigned upstream, hot;
    puts("NOR callbacks, 2x4096-byte physical pages, W=4, CRC32 enabled, no persistent lookup cache, changed 4-byte values. Counts reset between mount/seed/2000-update warmup/20000-update sample; complete GC intervals counted only within sample.");
    for (upstream = 0; upstream < 2; ++upstream) {
        for (hot = 0; hot < 2; ++hot) {
            run_case(upstream, hot);
        }
    }
    puts("PASS wear benchmark: both workloads and implementations retained all eight final values");
    return 0;
}
