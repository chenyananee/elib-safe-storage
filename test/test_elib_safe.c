/* test_elib_safe.c - RAM-mock media test suite (v3)
 *
 * Record layout used by the tests:
 *
 *   [0..3]  version slot (library owned in WEAR_LEVELED, spare in DIRECT)
 *   [4..5]  magic 0xA5A5
 *   [6]     checksum over [7..63]
 *   [7..63] payload
 *
 * validate() skips the version slot, exactly as the library requires in
 * WEAR_LEVELED mode; in DIRECT mode those bytes are just unvalidated spare.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "elib_safe.h"

#define REC_SIZE      64u
#define PRIM_OFFSET   0u
#define BACK_OFFSET   64u
#define MAGIC         0xA5A5u

static uint8_t mock_mem[REC_SIZE * 2];

/* user_data: layout inside the mock medium + per-region fault flags */
typedef struct {
    uint16_t  offset[2];
    uint8_t   read_fail[2];
    uint8_t   write_fail[2];
} mock_t;

static mock_t mock = { { PRIM_OFFSET, BACK_OFFSET }, { 0, 0 }, { 0, 0 } };

static int mock_read(void *ud, elib_safe_region_t region, uint8_t *buf) {
    const mock_t *m = (const mock_t *)ud;
    if (m->read_fail[region]) {
        return -1;
    }
    memcpy(buf, &mock_mem[m->offset[region]], REC_SIZE);
    return 0;
}

static int mock_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const mock_t *m = (const mock_t *)ud;
    if (m->write_fail[region]) {
        return -1;
    }
    memcpy(&mock_mem[m->offset[region]], buf, REC_SIZE);
    return 0;
}

static uint8_t mock_validate(void *ud, const uint8_t *buf) {
    uint8_t sum = 0;
    uint16_t i;
    (void)ud;
    if (buf[4] != (uint8_t)(MAGIC & 0xFFu) ||
        buf[5] != (uint8_t)((MAGIC >> 8) & 0xFFu)) {
        return 0;
    }
    for (i = 7; i < REC_SIZE; i++) {
        sum += buf[i];
    }
    return (uint8_t)(sum == buf[6]);
}

static const elib_safe_ops_t mock_ops = {
    &mock, mock_read, mock_write, mock_validate,
};

/* Compose one valid record: magic + checksum + seeded payload.
 * The 4 version bytes are left untouched (the library owns them). */
static void make_record(uint8_t *buf, uint8_t seed) {
    uint8_t sum = 0;
    uint16_t i;
    memset(buf, seed, REC_SIZE);
    buf[4] = (uint8_t)(MAGIC & 0xFFu);
    buf[5] = (uint8_t)((MAGIC >> 8) & 0xFFu);
    for (i = 7; i < REC_SIZE; i++) {
        sum += buf[i];
    }
    buf[6] = (uint8_t)sum;
}

/* Corrupt a region into a torn-write look (readable, but invalid) */
static void inject_torn(uint32_t region) {
    memset(&mock_mem[mock.offset[region]], 0x3Cu, REC_SIZE);
}

static uint32_t buf_version(const uint8_t *buf) {
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

static uint32_t region_version(uint32_t region) {
    return buf_version(&mock_mem[mock.offset[region]]);
}

static void set_region_version(uint32_t region, uint32_t v) {
    uint8_t *p = &mock_mem[mock.offset[region]];
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static elib_safe_ctx_t ctx;
static uint8_t rec[REC_SIZE];

static void base_setup(elib_safe_mode_t mode) {
    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops, mode) == ELIB_SAFE_OK);
}

/* ---- DIRECT mode: v2 behaviour preserved ---- */

static void test_init_valid_and_rejects(void) {
    printf("Test: init validates the configuration... ");

    assert(elib_safe_init(&ctx, &mock_ops, ELIB_SAFE_MODE_DIRECT) ==
           ELIB_SAFE_OK);
    assert(ctx.bit_flags.initialized == 1);
    assert(ctx.bit_flags.wear_leveled == 0);
    assert(elib_safe_init(&ctx, &mock_ops, ELIB_SAFE_MODE_WEAR_LEVELED) ==
           ELIB_SAFE_OK);
    assert(ctx.bit_flags.wear_leveled == 1);

    assert(elib_safe_init(NULL, &mock_ops, ELIB_SAFE_MODE_DIRECT) ==
           ELIB_SAFE_ERR_INVALID_PARAM);
    assert(elib_safe_init(&ctx, NULL, ELIB_SAFE_MODE_DIRECT) ==
           ELIB_SAFE_ERR_INVALID_PARAM);
    assert(elib_safe_init(&ctx, &mock_ops, (elib_safe_mode_t)2) ==
           ELIB_SAFE_ERR_INVALID_PARAM);
    {
        elib_safe_ops_t bad = mock_ops;
        bad.read = NULL;
        assert(elib_safe_init(&ctx, &bad, ELIB_SAFE_MODE_DIRECT) ==
               ELIB_SAFE_ERR_INVALID_PARAM);
        bad = mock_ops;
        bad.write = NULL;
        assert(elib_safe_init(&ctx, &bad, ELIB_SAFE_MODE_DIRECT) ==
               ELIB_SAFE_ERR_INVALID_PARAM);
        bad = mock_ops;
        bad.validate = NULL;
        assert(elib_safe_init(&ctx, &bad, ELIB_SAFE_MODE_DIRECT) ==
               ELIB_SAFE_ERR_INVALID_PARAM);
    }

    printf("PASSED\n");
}

static void test_read_fresh_media(void) {
    printf("Test: fresh never-written medium reports CORRUPTED... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    assert(elib_safe_init(&ctx, &mock_ops, ELIB_SAFE_MODE_DIRECT) ==
           ELIB_SAFE_OK);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    printf("PASSED\n");
}

static void test_write_happy_cycle(void) {
    printf("Test: write lands in both regions, read returns it... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    make_record(rec, 0x11);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    assert(memcmp(&mock_mem[PRIM_OFFSET], rec, REC_SIZE) == 0);
    assert(memcmp(&mock_mem[BACK_OFFSET], rec, REC_SIZE) == 0);

    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x11);

    make_record(rec, 0x22);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x22);

    printf("PASSED\n");
}

static void test_write_partial_failures(void) {
    printf("Test: write surfaces every per-region outcome... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    make_record(rec, 0x30);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    mock.write_fail[ELIB_SAFE_REGION_BACKUP] = 1;
    {
        uint8_t rec2[REC_SIZE];
        make_record(rec2, 0x31);
        assert(elib_safe_write(&ctx, rec2) ==
               ELIB_SAFE_ERR_BACKUP_WRITE_FAILED);
        assert(memcmp(&mock_mem[PRIM_OFFSET], rec2, REC_SIZE) == 0);
        assert(memcmp(&mock_mem[BACK_OFFSET], rec, REC_SIZE) == 0);
    }
    mock.write_fail[ELIB_SAFE_REGION_BACKUP] = 0;
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    {
        uint8_t rec2[REC_SIZE];
        make_record(rec2, 0x32);
        assert(elib_safe_write(&ctx, rec2) ==
               ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED);
        assert(memcmp(&mock_mem[BACK_OFFSET], rec2, REC_SIZE) == 0);
        assert(memcmp(&mock_mem[PRIM_OFFSET], rec, REC_SIZE) == 0);
    }
    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 0;
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    mock.write_fail[ELIB_SAFE_REGION_BACKUP] = 1;
    {
        uint8_t rec2[REC_SIZE];
        make_record(rec2, 0x33);
        assert(elib_safe_write(&ctx, rec2) == ELIB_SAFE_ERR_WRITE_FAILED);
        assert(memcmp(&mock_mem[PRIM_OFFSET], rec, REC_SIZE) == 0);
        assert(memcmp(&mock_mem[BACK_OFFSET], rec, REC_SIZE) == 0);
    }
    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 0;
    mock.write_fail[ELIB_SAFE_REGION_BACKUP] = 0;

    printf("PASSED\n");
}

static void test_write_rejects_invalid_record(void) {
    printf("Test: write entry validation rejects bad records... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    make_record(rec, 0x40);
    rec[6] ^= 0x01u; /* bad checksum */
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_ERR_DATA_INVALID);
    rec[6] ^= 0x01u;
    rec[4] ^= 0xFFu; /* bad magic */
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_ERR_DATA_INVALID);

    assert(mock_mem[PRIM_OFFSET] == 0xFF);
    assert(mock_mem[BACK_OFFSET] == 0xFF);

    printf("PASSED\n");
}

static void test_read_repairs_primary(void) {
    printf("Test: torn primary is recovered from backup, then repaired... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    make_record(rec, 0x50);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    inject_torn(ELIB_SAFE_REGION_PRIMARY);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x50);
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET], REC_SIZE) == 0);

    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x50);

    printf("PASSED\n");
}

static void test_read_recover_write_fails(void) {
    printf("Test: backup data returned, but a failed repair is reported... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    make_record(rec, 0x60);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    inject_torn(ELIB_SAFE_REGION_PRIMARY);

    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_RECOVER_WRITE_FAILED);
    assert(rec[7] == 0x60);
    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 0;

    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET], REC_SIZE) == 0);

    printf("PASSED\n");
}

static void test_read_io_priority(void) {
    printf("Test: media failures outrank content verdicts in read... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    inject_torn(ELIB_SAFE_REGION_PRIMARY);
    inject_torn(ELIB_SAFE_REGION_BACKUP);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    mock.read_fail[ELIB_SAFE_REGION_BACKUP] = 1;
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_IO);
    mock.read_fail[ELIB_SAFE_REGION_BACKUP] = 0;

    mock.read_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_IO);
    mock.read_fail[ELIB_SAFE_REGION_PRIMARY] = 0;

    mock.read_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    mock.read_fail[ELIB_SAFE_REGION_BACKUP] = 1;
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_IO);
    mock.read_fail[ELIB_SAFE_REGION_PRIMARY] = 0;
    mock.read_fail[ELIB_SAFE_REGION_BACKUP] = 0;

    printf("PASSED\n");
}

static void test_guards(void) {
    printf("Test: NOT_INITIALIZED and NULL guards... ");

    {
        elib_safe_ctx_t raw_ctx;
        memset(&raw_ctx, 0, sizeof(raw_ctx));
        assert(elib_safe_read(&raw_ctx, rec) == ELIB_SAFE_ERR_NOT_INITIALIZED);
        assert(elib_safe_write(&raw_ctx, rec) ==
               ELIB_SAFE_ERR_NOT_INITIALIZED);

        assert(elib_safe_init(&ctx, &mock_ops, ELIB_SAFE_MODE_DIRECT) ==
               ELIB_SAFE_OK);
        assert(elib_safe_read(&ctx, NULL) == ELIB_SAFE_ERR_INVALID_PARAM);
        assert(elib_safe_write(&ctx, NULL) == ELIB_SAFE_ERR_INVALID_PARAM);
    }

    printf("PASSED\n");
}

static void test_direct_ignores_version_slot(void) {
    printf("Test: DIRECT mode never rewrites the first 4 bytes... ");

    base_setup(ELIB_SAFE_MODE_DIRECT);

    make_record(rec, 0x70);
    rec[0] = 0xDEu;
    rec[1] = 0xADu;
    rec[2] = 0xBEu;
    rec[3] = 0xEFu;
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    /* caller buffer untouched, media keeps the caller's bytes as-is */
    assert(rec[0] == 0xDEu && rec[1] == 0xADu &&
           rec[2] == 0xBEu && rec[3] == 0xEFu);
    assert(mock_mem[PRIM_OFFSET + 0] == 0xDEu);
    assert(mock_mem[BACK_OFFSET + 3] == 0xEFu);

    printf("PASSED\n");
}

/* ---- WEAR_LEVELED mode ---- */

static void test_wl_write_requires_read(void) {
    printf("Test: WEAR_LEVELED write before any read is rejected... ");

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    make_record(rec, 0x80);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_ERR_NO_VERSION);

    printf("PASSED\n");
}

static void test_wl_read_seeds_fresh(void) {
    printf("Test: WEAR_LEVELED fresh medium seeds the counter... ");

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);
    assert(ctx.bit_flags.version_known == 1);

    make_record(rec, 0x81);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    assert(ctx.version == 1u);
    assert(region_version(ELIB_SAFE_REGION_PRIMARY) == 1u);
    assert(region_version(ELIB_SAFE_REGION_BACKUP) == 1u);

    printf("PASSED\n");
}

static void test_wl_prefers_newer_backup(void) {
    printf("Test: WEAR_LEVELED returns the newer backup after primary loss... ");

    uint8_t v1_copy[REC_SIZE];

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    make_record(rec, 0x11);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 1 */
    memcpy(v1_copy, &mock_mem[PRIM_OFFSET], REC_SIZE);

    make_record(rec, 0x22);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 2 */
    assert(region_version(ELIB_SAFE_REGION_PRIMARY) == 2u);
    assert(region_version(ELIB_SAFE_REGION_BACKUP) == 2u);

    /* simulate a flash page loss: the primary regresses to version 1 */
    memcpy(&mock_mem[PRIM_OFFSET], v1_copy, REC_SIZE);

    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x22); /* the newer backup won, not the older primary */
    assert(buf_version(rec) == 2u);

    /* the read repaired the primary from the backup */
    assert(region_version(ELIB_SAFE_REGION_PRIMARY) == 2u);
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET],
                  REC_SIZE) == 0);

    printf("PASSED\n");
}

static void test_wl_prefers_newer_primary(void) {
    printf("Test: WEAR_LEVELED returns the newer primary without repair... ");

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    make_record(rec, 0x31);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 1 */
    make_record(rec, 0x32);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 2 */

    /* make the backup look older */
    set_region_version(ELIB_SAFE_REGION_BACKUP, 1u);

    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(buf_version(rec) == 2u);
    assert(rec[7] == 0x32);

    /* primary was newer, so the backup is left alone */
    assert(region_version(ELIB_SAFE_REGION_BACKUP) == 1u);

    printf("PASSED\n");
}

static void test_wl_equal_version(void) {
    printf("Test: WEAR_LEVELED equal versions read fine... ");

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    make_record(rec, 0x41);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    assert(region_version(ELIB_SAFE_REGION_PRIMARY) ==
           region_version(ELIB_SAFE_REGION_BACKUP));

    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x41);

    printf("PASSED\n");
}

static void test_wl_version_wrap(void) {
    printf("Test: WEAR_LEVELED version wrap-around... ");

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    make_record(rec, 0x51);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 1 */

    /* wrap comparison: version 1 is newer than 0xFFFFFFFE */
    set_region_version(ELIB_SAFE_REGION_PRIMARY, 0xFFFFFFFEu);
    set_region_version(ELIB_SAFE_REGION_BACKUP, 1u);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(buf_version(rec) == 1u);

    /* next_version(0xFFFFFFFE) must skip 0xFFFFFFFF and 0 */
    set_region_version(ELIB_SAFE_REGION_PRIMARY, 0xFFFFFFFEu);
    set_region_version(ELIB_SAFE_REGION_BACKUP, 0xFFFFFFFEu);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(ctx.version == 0xFFFFFFFEu);

    make_record(rec, 0x52);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    assert(region_version(ELIB_SAFE_REGION_PRIMARY) == 1u);

    printf("PASSED\n");
}

static void test_wl_invalid_version_skipped(void) {
    printf("Test: WEAR_LEVELED skips 0 / all-F version slots... ");

    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    make_record(rec, 0x61);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 1 */
    make_record(rec, 0x62);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 2 */

    /* primary version slot looks erased (0xFFFFFFFF): must be skipped */
    set_region_version(ELIB_SAFE_REGION_PRIMARY, 0xFFFFFFFFu);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x62); /* backup (version 2) won */

    /* same with a 0 version slot on the backup */
    make_record(rec, 0x63);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* version 3 */
    set_region_version(ELIB_SAFE_REGION_BACKUP, 0u);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x63); /* primary (version 3) won */

    printf("PASSED\n");
}

static void test_wl_page_loss_matrix(void) {
    printf("Test: WEAR_LEVELED power-loss points all return the newest... ");

    uint8_t v_prev[REC_SIZE];

    /* point 1: the backup write is torn -> the primary wins (backup stays
     * torn); the next successful write re-syncs it */
    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);
    make_record(rec, 0x71);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    inject_torn(ELIB_SAFE_REGION_BACKUP);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x71);
    assert(region_version(ELIB_SAFE_REGION_PRIMARY) == 1u);

    /* point 2: the primary write is torn -> the backup wins and the primary
     * is repaired from it */
    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);
    make_record(rec, 0x72);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    inject_torn(ELIB_SAFE_REGION_PRIMARY);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x72);
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET],
                  REC_SIZE) == 0);

    /* point 3: the primary regresses to the previous generation (a wear-leveling
     * page loss) -> the backup wins and the primary is repaired */
    base_setup(ELIB_SAFE_MODE_WEAR_LEVELED);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);
    make_record(rec, 0x73);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* V1 */
    memcpy(v_prev, &mock_mem[PRIM_OFFSET], REC_SIZE);
    make_record(rec, 0x74);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK); /* V2 */
    memcpy(&mock_mem[PRIM_OFFSET], v_prev, REC_SIZE);   /* regress primary */
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[7] == 0x74); /* the newer backup won, not the older primary */
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET],
                  REC_SIZE) == 0);

    printf("PASSED\n");
}

int main(void) {
    test_init_valid_and_rejects();
    test_read_fresh_media();
    test_write_happy_cycle();
    test_write_partial_failures();
    test_write_rejects_invalid_record();
    test_read_repairs_primary();
    test_read_recover_write_fails();
    test_read_io_priority();
    test_guards();
    test_direct_ignores_version_slot();
    test_wl_write_requires_read();
    test_wl_read_seeds_fresh();
    test_wl_prefers_newer_backup();
    test_wl_prefers_newer_primary();
    test_wl_equal_version();
    test_wl_version_wrap();
    test_wl_invalid_version_skipped();
    test_wl_page_loss_matrix();

    printf("All tests PASSED\n");
    return 0;
}
