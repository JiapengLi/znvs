/* Included by the NOR harness; exercises integrity and repeated recovery. */
static void integrity_put32(uint8_t *p, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) {
        p[i] = (uint8_t)(value >> (8 * i));
    }
}

static void integrity_metadata_fault(const znvs_cfg_t *cfg, uint32_t metadata, unsigned bit, unsigned second)
{
    znvs_t fs;
    size_t len;
    uint64_t writes, erases;
    uint8_t mask = (uint8_t)(1U << (bit & 7));
    flash = saved;
    CHECK(znvs_init(&fs, cfg, &flash) == 0);
    bytes(&flash)[metadata + bit / 8] ^= mask;
    if (second < 64) {
        bytes(&flash)[metadata + second / 8] ^= (uint8_t)(1U << (second & 7));
    }
    writes = flash.writes;
    erases = flash.erases;
    CHECK(znvs_read(&fs, 7, bigread, sizeof(bigread), &len) == ZNVS_ECORRUPT);
    CHECK(znvs_mount(&fs) == ZNVS_ECORRUPT);
    CHECK(flash.writes == writes && flash.erases == erases);
    guards(&flash);
    ++fault_cases;
}

static void test_integrity_metadata(void)
{
    static const char *names[] = {"integrity/metadata/new-value", "integrity/metadata/updated-value", "integrity/metadata/deletion"};
    znvs_t fs;
    unsigned w, kind, bit, second;
    uint32_t metadata;
    for (w = 1; w <= 32; w *= 2) {
        znvs_cfg_t cfg = config(1024, 2, (uint16_t)w);
        for (kind = 0; kind < 3; ++kind) {
            test_name = names[kind];
            fresh(&flash, &cfg);
            CHECK(znvs_init(&fs, &cfg, &flash) == 0);
            if (kind == 2) {
                CHECK(znvs_write(&fs, 6, "SIX", 4) == 0);
            }
            CHECK(znvs_write(&fs, 7, "OLD", 4) == 0);
            if (kind == 1) {
                CHECK(znvs_write(&fs, 7, "NEW", 4) == 0);
            } else if (kind == 2) {
                CHECK(znvs_delete(&fs, 7) == 0);
            }
            metadata = fs.pos.ate;
            saved = flash;
            for (bit = 0; bit < 64; ++bit) {
                integrity_metadata_fault(&cfg, metadata, bit, 64);
            }
            /* Every pair across ID, length and checksum must be detected;
             * corrupt lengths must never redirect the implicit data cursor. */
            for (bit = 0; bit < 64; ++bit) {
                for (second = bit + 1; second < 64; ++second) {
                    integrity_metadata_fault(&cfg, metadata, bit, second);
                }
            }
        }
    }
}

static void test_integrity_geometry(void)
{
    znvs_cfg_t stored_cfg = config(65536, 2, 1), wrong_cfg = config(131072, 2, 2);
    znvs_t fs;
    uint64_t writes, erases;
    test_name = "integrity/geometry/reject-mismatched-geometry";
    fresh(&flash, &wrong_cfg);
    flash.write_size = stored_cfg.write_size;
    flash.erase_size = stored_cfg.erase_size;
    CHECK(znvs_init(&fs, &stored_cfg, &flash) == 0);
    CHECK(znvs_write(&fs, 7, "KEEP", 5) == 0);
    saved = flash;
    writes = flash.writes;
    erases = flash.erases;
    CHECK(znvs_init(&fs, &wrong_cfg, &flash) == ZNVS_ECORRUPT);
    CHECK(flash.writes == writes && flash.erases == erases);
    CHECK(memcmp(bytes(&flash), bytes(&saved), flash.size) == 0);
    CHECK(znvs_init(&fs, &stored_cfg, &flash) == 0);
    expect(&fs, 7, "KEEP", 5);
    guards(&flash);
}

static void test_integrity_minimum_bank(void)
{
    unsigned w;
    znvs_t fs;
    test_name = "integrity/geometry/exact-minimum-bank";
    for (w = 1; w <= 32; w *= 2) {
        znvs_cfg_t cfg = config(1024, 2, (uint16_t)w);
        uint32_t metadata = (8U + w - 1U) & ~(w - 1U);
        uint32_t crc = (4U + w - 1U) & ~(w - 1U);
        cfg.erase_size = 1;
        cfg.size = 2U * (32U + metadata + 2U * w + crc);
        fresh(&flash, &cfg);
        CHECK(znvs_init(&fs, &cfg, &flash) == 0);
        CHECK(znvs_max_size(&fs) == w);
        CHECK(znvs_write(&fs, 7, "A", 1) == 0);
        CHECK(znvs_mount(&fs) == 0);
        expect(&fs, 7, "A", 1);
        CHECK(znvs_write(&fs, 7, "B", 1) == 0);
        CHECK(znvs_mount(&fs) == 0);
        expect(&fs, 7, "B", 1);
        CHECK(znvs_delete(&fs, 7) == 0);
        expect(&fs, 7, NULL, 0);
        guards(&flash);
        cfg.size -= 2U * w;
        fresh(&flash, &cfg);
        CHECK(znvs_init(&fs, &cfg, &flash) == ZNVS_EINVAL);
        CHECK(znvs_max_size(&fs) == 0 && znvs_arg(&fs) == NULL);
        CHECK(flash.reads == 0 && flash.writes == 0 && flash.erases == 0);
    }
}

static void test_integrity_foreign_header(void)
{
    znvs_cfg_t cfg = config(1024, 2, 4);
    znvs_t fs;
    unsigned populated, addr;
    uint64_t writes, erases;
    uint32_t magic = UINT32_C(0xdeadbeef);
    test_name = "integrity/format/reject-foreign-signature-without-mutation";
    for (populated = 0; populated < 2; ++populated) {
        fresh(&flash, &cfg);
        CHECK(znvs_init(&fs, &cfg, &flash) == 0);
        if (populated) {
            CHECK(znvs_write(&fs, 7, "KEEP", 5) == 0);
        }
        for (addr = 0; addr < 32; addr += 16) {
            integrity_put32(bytes(&flash) + addr, magic);
            integrity_put32(bytes(&flash) + addr + 4, 0);
            integrity_put32(bytes(&flash) + addr + 8, ~magic);
            integrity_put32(bytes(&flash) + addr + 12, UINT32_MAX);
        }
        saved = flash;
        writes = flash.writes;
        erases = flash.erases;
        CHECK(znvs_init(&fs, &cfg, &flash) == ZNVS_ECORRUPT);
        CHECK(flash.writes == writes && flash.erases == erases);
        CHECK(memcmp(bytes(&flash), bytes(&saved), flash.size) == 0);
        guards(&flash);
    }
}

static void test_integrity_sequence(void)
{
    znvs_cfg_t cfg = config(1024, 2, 4);
    znvs_t fs;
    uint32_t sequence = UINT32_MAX - 1U;
    unsigned addr;
    test_name = "integrity/sequence/UINT32_MAX-wrap";
    fresh(&flash, &cfg);
    CHECK(znvs_init(&fs, &cfg, &flash) == 0);
    CHECK(znvs_write(&fs, 7, "KEEP", 5) == 0);
    for (addr = 0; addr < 32; addr += 16) {
        integrity_put32(bytes(&flash) + fs.bank + addr + 4, sequence);
        integrity_put32(bytes(&flash) + fs.bank + addr + 12, ~sequence);
    }
    CHECK(znvs_mount(&fs) == 0 && fs.sequence == UINT32_MAX - 1U);
    CHECK(znvs_rotate(&fs) == 0);
    CHECK(znvs_mount(&fs) == 0 && fs.sequence == UINT32_MAX);
    expect(&fs, 7, "KEEP", 5);
    CHECK(znvs_rotate(&fs) == 0);
    CHECK(znvs_mount(&fs) == 0 && fs.sequence == 0);
    expect(&fs, 7, "KEEP", 5);
    guards(&flash);
}

static void test_integrity_repeated_gc(void)
{
    znvs_t fs, probe_fs;
    value_t model[KEYS] = {{0, {0}}};
    uint32_t seed = UINT32_C(0xa875941d);
    unsigned w, id, round, operations;
    uint64_t writes, erases, mutations;
    test_name = "integrity/repeated-GC-failures/recover-and-continue";
    for (w = 1; w <= 32; w *= 2) {
        znvs_cfg_t cfg = config(4096, 2, (uint16_t)w);
        fresh(&flash, &cfg);
        CHECK(znvs_init(&fs, &cfg, &flash) == 0);
        for (id = 0; id < KEYS; ++id) {
            model[id].len = 17;
            memset(model[id].data, (int)id + 1, model[id].len);
            CHECK(znvs_write(&fs, (uint16_t)id, model[id].data, model[id].len) == 0);
        }
        for (round = 0; round < 64; ++round) {
            /* Work from the actual recovered image, never reset to a pristine
             * snapshot between failures. Probe only determines a valid cut. */
            probe = flash;
            CHECK(znvs_init(&probe_fs, &cfg, &probe) == 0);
            mutations = probe.writes + probe.erases;
            CHECK(znvs_rotate(&probe_fs) == 0);
            operations = (unsigned)(probe.writes + probe.erases - mutations);
            CHECK(operations > 1);
            flash.operation_cut = (int)(rng(&seed) % operations);
            flash.tear_seed = rng(&seed);
            CHECK(znvs_rotate(&fs) == ZNVS_EIO);
            recover_device(&flash);
            writes = flash.writes;
            erases = flash.erases;
            CHECK(znvs_mount(&fs) == 0);
            CHECK(flash.writes == writes && flash.erases == erases);
            for (id = 0; id < KEYS; ++id) {
                expect(&fs, (uint16_t)id, model[id].data, model[id].len);
            }
            if (round % 8 == 7) {
                CHECK(znvs_rotate(&fs) == 0);
                CHECK(znvs_mount(&fs) == 0);
            }
            guards(&flash);
            ++fault_cases;
        }
        CHECK(znvs_write(&fs, 60000, "NEXT", 5) == 0);
        CHECK(znvs_mount(&fs) == 0);
        expect(&fs, 60000, "NEXT", 5);
    }
}

static void test_integrity(void)
{
    test_integrity_metadata();
    test_integrity_geometry();
    test_integrity_minimum_bank();
    test_integrity_foreign_header();
    test_integrity_sequence();
    test_integrity_repeated_gc();
}
