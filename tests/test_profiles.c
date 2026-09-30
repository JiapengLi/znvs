/* Full application, compact bootloader and read-only builds share one media format. */
#define main full_suite_main
#include "test_znvs.c"
#undef main

int boot_init(znvs_t *, const znvs_cfg_t *, void *);
int boot_mount(znvs_t *);
int boot_read(znvs_t *, uint16_t, void *, size_t, size_t *);
int boot_write(znvs_t *, uint16_t, const void *, size_t);
int boot_delete(znvs_t *, uint16_t);
int ro_init(znvs_t *, const znvs_cfg_t *, void *);
int ro_read(znvs_t *, uint16_t, void *, size_t, size_t *);

static void compare_profiles(znvs_t *app, znvs_t *boot, znvs_t *ro)
{
    unsigned id;
    uint8_t a[80], b[80], c[80];
    size_t na, nb, nc;
    for (id = 0; id < KEYS; ++id) {
        int ra = znvs_read(app, (uint16_t)id, a, sizeof(a), &na);
        int rb = boot_read(boot, (uint16_t)id, b, sizeof(b), &nb);
        int rc = ro_read(ro, (uint16_t)id, c, sizeof(c), &nc);
        CHECK(ra == rb && rb == rc && na == nb && nb == nc);
        CHECK(ra == ZNVS_ENOENT || (ra == 0 && !memcmp(a, b, na) && !memcmp(b, c, nb)));
    }
}

int main(void)
{
    znvs_cfg_t cfg = config(4096, 2, 4), read_cfg = cfg, bad;
    znvs_t app, boot, ro;
    uint32_t seed = 0x719acfe3;
    unsigned step, id, k, operations, cut, w, failures = 0;
    size_t len;
    uint8_t value[80];
    uint64_t writes, erases;
    value_t model[KEYS] = {{0, {0}}}, after;
    test_name = "profiles/full-boot-readonly/interoperability";
    read_cfg.write = NULL;
    read_cfg.erase = NULL;
    fresh(&flash, &cfg);
    CHECK(boot_init(&boot, &cfg, &flash) == ZNVS_ECORRUPT);
    CHECK(ro_init(&ro, &read_cfg, &flash) == ZNVS_ECORRUPT);
    CHECK(flash.writes == 0 && flash.erases == 0);
    CHECK(znvs_init(&app, &cfg, &flash) == 0);
    bad = cfg;
    bad.write_size = 3;
    CHECK(boot_init(&boot, &bad, &flash) == ZNVS_EINVAL);
    bad = cfg;
    bad.size += 1;
    CHECK(boot_init(&boot, &bad, &flash) == ZNVS_EINVAL);
    for (step = 0; step < 1800; ++step) {
        id = rng(&seed) % KEYS;
        len = rng(&seed) % sizeof(value);
        for (k = 0; k < len; ++k) {
            value[k] = (uint8_t)rng(&seed);
        }
        if (step & 1) {
            CHECK(boot_init(&boot, &cfg, &flash) == 0);
            CHECK(boot_write(&boot, (uint16_t)id, value, len) == 0);
        } else {
            CHECK(znvs_init(&app, &cfg, &flash) == 0);
            CHECK(znvs_write(&app, (uint16_t)id, value, len) == 0);
        }
        if (step % 23 == 0) {
            writes = flash.writes;
            erases = flash.erases;
            CHECK(znvs_init(&app, &cfg, &flash) == 0);
            CHECK(boot_init(&boot, &cfg, &flash) == 0);
            CHECK(ro_init(&ro, &read_cfg, &flash) == 0);
            compare_profiles(&app, &boot, &ro);
            CHECK(flash.writes == writes && flash.erases == erases);
        }
    }
    CHECK(boot_init(&boot, &cfg, &flash) == 0);
    CHECK(boot_delete(&boot, 0) == 0);
    CHECK(boot_mount(&boot) == 0);
    CHECK(boot_read(&boot, 0, value, sizeof(value), &len) == ZNVS_ENOENT);
    for (w = 1; w <= 32; w *= 2) {
        cfg = config(4096, 2, (uint16_t)w);
        read_cfg = cfg;
        read_cfg.write = NULL;
        read_cfg.erase = NULL;
        test_name = "profiles/bootloader-unordered-GC-power-loss";
        fresh(&flash, &cfg);
        CHECK(znvs_init(&app, &cfg, &flash) == 0);
        for (k = 0; k < KEYS; ++k) {
            model[k].len = 17;
            memset(model[k].data, (int)k + 1, 17);
            CHECK(znvs_write(&app, (uint16_t)k, model[k].data, 17) == 0);
        }
        after.len = 61;
        memset(after.data, 0x96, after.len);
        while (znvs_available(&app) >= after.len) {
            ++model[0].data[0];
            CHECK(znvs_write(&app, 0, model[0].data, model[0].len) == 0);
        }
        saved = flash;
        probe = saved;
        writes = probe.writes + probe.erases;
        CHECK(boot_init(&boot, &cfg, &probe) == 0);
        CHECK(boot_write(&boot, 0, after.data, after.len) == 0);
        operations = (unsigned)(probe.writes + probe.erases - writes);
        for (cut = 0; cut < operations; ++cut) {
            for (seed = 0; seed < 32; ++seed) {
                flash = saved;
                CHECK(boot_init(&boot, &cfg, &flash) == 0);
                flash.operation_cut = (int)cut;
                flash.tear_seed = 0x9180de21 + seed;
                CHECK(boot_write(&boot, 0, after.data, after.len) == ZNVS_EIO);
                recover_device(&flash);
                CHECK(boot_init(&boot, &cfg, &flash) == 0);
                CHECK(znvs_init(&app, &cfg, &flash) == 0);
                CHECK(ro_init(&ro, &read_cfg, &flash) == 0);
                compare_profiles(&app, &boot, &ro);
                check_power_state(&app, model, 0, &after, 0);
            }
        }
        failures += operations * 32;
    }
    printf("PASS profiles: 1800 alternating full/boot writes, read-only checks, %u unordered boot-GC failures across 6 write geometries\n", failures);
    return 0;
}
