/* Callback counts, not wall-clock hardware throughput. */
#define main nor_suite_main
#include "test_znvs.c"
#undef main

static void counters_reset(void)
{
    flash.reads = flash.read_bytes = flash.writes = flash.erases = 0;
}

static void counters_print(const char *operation, unsigned keys)
{
    printf("BENCH keys=%u op=%s reads=%" PRIu64 " read_bytes=%" PRIu64 " writes=%" PRIu64 " erases=%" PRIu64 "\n", keys, operation, flash.reads, flash.read_bytes, flash.writes, flash.erases);
}

int main(void)
{
    znvs_cfg_t cfg = config(4096, 2, 4);
    znvs_t fs;
    unsigned keys, k;
    uint32_t value = 0x12345678, out;
    int rc;
    printf("BENCH configuration: CRC32, no lookup cache, 32-byte I/O/GC buffers\n");
    for (keys = 16; keys <= 128; keys *= 2) {
        fresh(&flash, &cfg);
        CHECK(znvs_init(&fs, &cfg, &flash) == 0);
        counters_print("blank-init", keys);
        counters_reset();
        CHECK(znvs_mount(&fs) == 0);
        counters_print("empty-remount", keys);
        for (k = 0; k < keys; ++k) {
            CHECK(znvs_write(&fs, (uint16_t)k, &value, sizeof(value)) == 0);
        }
        counters_reset();
        CHECK(znvs_read(&fs, 0, &out, sizeof(out), NULL) == 0);
        counters_print("read-oldest", keys);
        counters_reset();
        CHECK(znvs_rotate(&fs) == 0);
        counters_print("rotate", keys);
    }
    fresh(&flash, &cfg);
    CHECK(znvs_init(&fs, &cfg, &flash) == 0);
    memset(bigbuf, 0x53, 128);
    for (k = 0;; ++k) {
        rc = znvs_write(&fs, (uint16_t)k, bigbuf, 128);
        if (rc == ZNVS_ENOSPC) {
            break;
        }
        CHECK(rc == 0 && k < 100);
    }
    counters_reset();
    CHECK(znvs_write(&fs, 999, bigbuf, 128) == ZNVS_ENOSPC);
    CHECK(flash.writes == 0 && flash.erases == 0);
    counters_print("write-full", k);
    return 0;
}
