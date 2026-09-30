/* Included by the NOR harness to reuse its checked callbacks and reboot model. */
static void test_committed_corruption(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    uint32_t metadata, header, payload;
    unsigned bit;
    size_t n;
    test_name = "recovery/committed-metadata/no-rollback/no-resurrection";
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_write(&fs, 7, "OLD", 4) == 0);
    CHECK(znvs_write(&fs, 7, "NEW", 4) == 0);
    saved = flash;
    metadata = fs.pos.ate;
    for (bit = 0; bit < 64; ++bit) {
        flash = saved;
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        bytes(&flash)[metadata + bit / 8] ^= (uint8_t)(1U << (bit % 8));
        CHECK(znvs_read(&fs, 7, bigread, sizeof(bigread), &n) == ZNVS_ECORRUPT);
        CHECK(znvs_mount(&fs) == ZNVS_ECORRUPT);
    }
    flash = saved;
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_delete(&fs, 7) == 0);
    metadata = fs.pos.ate;
    saved = flash;
    for (bit = 0; bit < 64; ++bit) {
        flash = saved;
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        bytes(&flash)[metadata + bit / 8] ^= (uint8_t)(1U << (bit % 8));
        CHECK(znvs_read(&fs, 7, bigread, sizeof(bigread), &n) == ZNVS_ECORRUPT);
        CHECK(znvs_mount(&fs) == ZNVS_ECORRUPT);
    }
    /* After a successful commit, one changed commit bit cannot erase the marker. */
    flash = saved;
    for (bit = 0; bit < 8; ++bit) {
        flash = saved;
        bytes(&flash)[metadata + 8] ^= (uint8_t)(1U << bit);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        expect(&fs, 7, NULL, 0);
    }
    /* A single damaged bank-header bit must not select the previous snapshot. */
    flash = saved;
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_rotate(&fs) == 0);
    CHECK(znvs_write(&fs, 7, "NEW", 4) == 0);
    saved = flash;
    header = fs.bank;
    for (bit = 0; bit < 256; ++bit) {
        flash = saved;
        bytes(&flash)[header + bit / 8] ^= (uint8_t)(1U << (bit % 8));
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        expect(&fs, 7, "NEW", 4);
    }
    test_name = "recovery/GC-validates-payload/retains-source-on-corruption";
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_write(&fs, 7, "OLD", 4) == 0);
    payload = fs.pos.data;
    CHECK(znvs_write(&fs, 7, "NEW", 4) == 0);
    bytes(&flash)[payload] ^= 1;
    saved = flash;
    CHECK(znvs_rotate(&fs) == ZNVS_ECORRUPT);
    CHECK(memcmp(bytes(&flash), bytes(&saved), c.size / 2) == 0);
    CHECK(znvs_mount(&fs) == 0);
    CHECK(znvs_read_hist(&fs, 7, 1, bigread, sizeof(bigread), &n) == 0);
    CHECK(n == 4 && memcmp(bigread, "OLD", 4) == 0);
    CHECK(znvs_write(&fs, 7, "FIX", 4) == 0);
    CHECK(znvs_rotate(&fs) == 0);
    expect(&fs, 7, "FIX", 4);
}

static int alias_failure(void *arg, uint32_t addr, const void *data, size_t len)
{
    const uint8_t *src = data;
    uint8_t partial[8];
    if (len == 8 && src[0] == 0 && src[1] == 0) {
        memcpy(partial, src, sizeof(partial));
        partial[0] = 0xd9;
        partial[1] = 1;
        CHECK(sim_write(arg, addr, partial, sizeof(partial)) == 0);
        return ZNVS_EIO;
    }
    return sim_write(arg, addr, data, len);
}

static void test_partial_metadata_alias(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    size_t n;
    test_name = "recovery/regression/failed-id0-must-not-overwrite-id473";
    c.write = alias_failure;
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_write(&fs, 473, "OLD", 4) == 0);
    CHECK(znvs_write(&fs, 0, "NEW", 4) == ZNVS_EIO);
    CHECK(znvs_mount(&fs) == 0);
    expect(&fs, 473, "OLD", 4);
    CHECK(znvs_read(&fs, 0, bigread, sizeof(bigread), &n) == ZNVS_ENOENT);
    CHECK(znvs_write(&fs, 9, "NEXT", 5) == 0);
    CHECK(znvs_mount(&fs) == 0);
    expect(&fs, 9, "NEXT", 5);
    expect(&fs, 473, "OLD", 4);
}

static int partial_fixture_enabled;

static int crc_valid_partial_metadata(void *arg, uint32_t addr, const void *data, size_t len)
{
    static const uint8_t expected[8] = {0x07, 0x00, 0x04, 0x00, 0xa1, 0x22, 0xff, 0xd8};
    static const uint8_t partial[8] = {0x0f, 0x46, 0x96, 0x16, 0xf5, 0xa6, 0xff, 0xde};
    unsigned i;
    if (partial_fixture_enabled && len == sizeof(expected) && memcmp(data, expected, sizeof(expected)) == 0) {
        for (i = 0; i < sizeof(expected); ++i) {
            CHECK((expected[i] & partial[i]) == expected[i]);
        }
        CHECK(sim_write(arg, addr, partial, sizeof(partial)) == 0);
        return ZNVS_EIO;
    }
    return sim_write(arg, addr, data, len);
}

static void test_sealed_tail(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    uint64_t writes, erases;
    unsigned w;
    test_name = "recovery/CRC-valid-partial-metadata-must-not-reserve-false-length";
    c.write = crc_valid_partial_metadata;
    partial_fixture_enabled = 0;
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    CHECK(znvs_write(&fs, 7, "OLD", 4) == 0);
    partial_fixture_enabled = 1;
    CHECK(znvs_write(&fs, 7, "NEW", 4) == ZNVS_EIO);
    partial_fixture_enabled = 0;
    writes = flash.writes;
    erases = flash.erases;
    CHECK(znvs_mount(&fs) == 0);
    CHECK(flash.writes == writes && flash.erases == erases);
    CHECK(znvs_available(&fs) == 0);
    expect(&fs, 7, "OLD", 4);
    expect(&fs, 17935, NULL, 0);
    CHECK(znvs_write(&fs, 9, "NEXT", 5) == 0);
    CHECK(flash.erases == erases + 1);
    expect(&fs, 7, "OLD", 4);
    expect(&fs, 9, "NEXT", 5);
    guards(&flash);
    for (w = 1; w <= 32; w *= 2) {
        c = config(1024, 2, (uint16_t)w);
        test_name = "recovery/sealed-partial-payload/committed-prefix-remains-readable";
        fresh(&flash, &c);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        CHECK(znvs_write(&fs, 7, "OLD", 4) == 0);
        CHECK(znvs_write(&fs, 6, "COLD-LONGER", 12) == 0);
        flash.operation_cut = 1;
        flash.tear_seed = UINT32_C(0x396cf827);
        memset(bigbuf, 0x96, 33);
        CHECK(znvs_write(&fs, 7, bigbuf, 33) == ZNVS_EIO);
        recover_device(&flash);
        writes = flash.writes;
        erases = flash.erases;
        CHECK(znvs_mount(&fs) == 0 && znvs_available(&fs) == 0);
        CHECK(flash.writes == writes && flash.erases == erases);
        expect(&fs, 7, "OLD", 4);
        expect(&fs, 6, "COLD-LONGER", 12);
        CHECK(znvs_write(&fs, 9, "NEXT", 5) == 0);
        CHECK(flash.erases == erases + 1);
        CHECK(znvs_mount(&fs) == 0);
        expect(&fs, 7, "OLD", 4);
        expect(&fs, 6, "COLD-LONGER", 12);
        expect(&fs, 9, "NEXT", 5);
        guards(&flash);
    }
}

static void test_unordered_failures(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    value_t model[KEYS] = {{0, {0}}}, after;
    unsigned k, phase, seed, operation, operations;
    uint64_t mutations;
    for (phase = 0; phase < 3; ++phase) {
        test_name = "recovery/unordered-program-and-erase";
        fresh(&flash, &c);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        for (k = 0; k < KEYS; ++k) {
            model[k].len = 17;
            memset(model[k].data, (int)k + 1, 17);
            CHECK(znvs_write(&fs, (uint16_t)k, model[k].data, 17) == 0);
        }
        if (phase) {
            CHECK(znvs_rotate(&fs) == 0);
            CHECK(znvs_rotate(&fs) == 0);
        }
        after.len = phase == 2 ? 0 : 61;
        memset(after.data, 0x96, sizeof(after.data));
        if (phase == 1) {
            while (znvs_available(&fs) >= after.len) {
                ++model[0].data[0];
                CHECK(znvs_write(&fs, 0, model[0].data, model[0].len) == 0);
            }
        }
        saved = flash;
        probe = saved;
        mutations = probe.writes + probe.erases;
        CHECK(znvs_init(&fs, &c, &probe) == 0);
        CHECK((phase == 2 ? znvs_rotate(&fs) : znvs_write(&fs, 0, after.data, after.len)) == 0);
        operations = (unsigned)(probe.writes + probe.erases - mutations);
        if (phase == 2) {
            after = model[0];
        }
        for (operation = 0; operation < operations; ++operation) {
            for (seed = 0; seed < 128; ++seed) {
                flash = saved;
                CHECK(znvs_init(&fs, &c, &flash) == 0);
                flash.operation_cut = (int)operation;
                flash.tear_seed = seed + 0x78356921U;
                CHECK((phase == 2 ? znvs_rotate(&fs) : znvs_write(&fs, 0, after.data, after.len)) == ZNVS_EIO);
                recover_device(&flash);
                mutations = flash.mutated;
                CHECK(znvs_init(&fs, &c, &flash) == 0);
                CHECK(flash.mutated == mutations);
                check_power_state(&fs, model, 0, &after, 0);
                CHECK(znvs_write(&fs, 60000, "NEXT", 5) == 0);
                CHECK(znvs_mount(&fs) == 0);
                expect(&fs, 60000, "NEXT", 5);
                guards(&flash);
                ++fault_cases;
            }
        }
    }
}

static void test_first_header(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    unsigned cut, bit;
    test_name = "recovery/first-header/interrupted-recovery";
    for (cut = 0; cut <= 32; ++cut) {
        fresh(&flash, &c);
        flash.cut = cut;
        CHECK(znvs_init(&fs, &c, &flash) == (cut < 32 ? ZNVS_EIO : 0));
        saved = flash;
        for (bit = 0; bit < 16; ++bit) {
            flash = saved;
            recover_device(&flash);
            flash.cut = bit;
            {
                int rc = znvs_init(&fs, &c, &flash);
                CHECK(rc == 0 || rc == ZNVS_EIO);
            }
            recover_device(&flash);
            CHECK(znvs_init(&fs, &c, &flash) == 0);
            CHECK(znvs_write(&fs, 7, "OK", 3) == 0);
            expect(&fs, 7, "OK", 3);
        }
    }
}

static void test_filter_collisions(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    value_t model[KEYS] = {{0, {0}}};
    uint32_t seed = 0x317853ac;
    unsigned step, k, id;
    test_name = "recovery/GC-filter-collisions-and-deletions";
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    for (step = 0; step < 1200; ++step) {
        id = rng(&seed) % KEYS;
        model[id].len = rng(&seed) % 24;
        for (k = 0; k < model[id].len; ++k) {
            model[id].data[k] = (uint8_t)rng(&seed);
        }
        CHECK(znvs_write(&fs, (uint16_t)(id * 256 + 7), model[id].data, model[id].len) == 0);
        if (step % 29 == 0) {
            CHECK(znvs_rotate(&fs) == 0);
            CHECK(znvs_mount(&fs) == 0);
            for (k = 0; k < KEYS; ++k) {
                expect(&fs, (uint16_t)(k * 256 + 7), model[k].data, model[k].len);
            }
        }
    }
}

static void test_cache_collisions(void)
{
    znvs_cfg_t c = config(1024, 2, 4);
    znvs_t fs;
    value_t model[KEYS] = {{0, {0}}};
    uint32_t seed = UINT32_C(0x5179ab34);
    unsigned kind, step, k, id;
    test_name = "recovery/GC-cache-eviction/newer-value-and-deletion-win";
    for (kind = 0; kind < 2; ++kind) {
        fresh(&flash, &c);
        CHECK(znvs_init(&fs, &c, &flash) == 0);
        CHECK(znvs_write(&fs, 0, "OLD", 4) == 0);
        CHECK(znvs_write(&fs, 514, "C", 2) == 0);
        CHECK(znvs_write(&fs, 257, "B", 2) == 0);
        CHECK(znvs_write(&fs, 0, "NEW", kind ? 0 : 4) == 0);
        /* Newest-first order is A, B, C, old A. All three IDs map to one
         * tag slot, so the old A is revisited after A's tag was evicted. */
        CHECK(znvs_rotate(&fs) == 0);
        CHECK(znvs_mount(&fs) == 0);
        expect(&fs, 0, kind ? NULL : "NEW", kind ? 0 : 4);
        expect(&fs, 257, "B", 2);
        expect(&fs, 514, "C", 2);
        CHECK(znvs_read_hist(&fs, 0, 1, bigread, sizeof(bigread), NULL) == ZNVS_ENOENT);
        guards(&flash);
    }
    test_name = "recovery/GC-cache-collisions/random-updates-and-deletions";
    fresh(&flash, &c);
    CHECK(znvs_init(&fs, &c, &flash) == 0);
    for (step = 0; step < 1200; ++step) {
        id = rng(&seed) % KEYS;
        model[id].len = rng(&seed) % 24;
        for (k = 0; k < model[id].len; ++k) {
            model[id].data[k] = (uint8_t)rng(&seed);
        }
        CHECK(znvs_write(&fs, (uint16_t)(id * 257), model[id].data, model[id].len) == 0);
        if (step % 29 == 0 || step == 1199) {
            CHECK(znvs_rotate(&fs) == 0);
            CHECK(znvs_mount(&fs) == 0);
            for (k = 0; k < KEYS; ++k) {
                expect(&fs, (uint16_t)(k * 257), model[k].data, model[k].len);
            }
        }
    }
    guards(&flash);
}

static void test_recovery(void)
{
    test_partial_metadata_alias();
    test_sealed_tail();
    test_committed_corruption();
    test_unordered_failures();
    test_first_header();
    test_filter_collisions();
    test_cache_collisions();
}
