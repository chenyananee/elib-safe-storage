/* test_elib_safe.c - RAM-mock media test suite
 *
 * The suite only compiles on this host toolchain (-Werror, no run);
 * every case is verified by static timeline tracing. The mock medium
 * is a byte array holding two 64-byte regions; injections per test:
 * torn content (garbage bytes), backend read/write failures per
 * region. Payload validity = magic (bytes 0..1) + checksum (byte 2 =
 * sum of bytes 3..63). */
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
    if (buf[0] != (uint8_t)(MAGIC & 0xFFu) ||
        buf[1] != (uint8_t)((MAGIC >> 8) & 0xFFu)) {
        return 0;
    }
    for (i = 3; i < REC_SIZE; i++) {
        sum += buf[i];
    }
    return (uint8_t)(sum == buf[2]);
}

static const elib_safe_ops_t mock_ops = {
    &mock, mock_read, mock_write, mock_validate,
};

/* Compose one valid record: magic + checksum + seeded payload */
static void make_record(uint8_t *buf, uint8_t seed) {
    uint8_t sum = 0;
    uint16_t i;
    memset(buf, seed, REC_SIZE);
    buf[0] = (uint8_t)(MAGIC & 0xFFu);
    buf[1] = (uint8_t)((MAGIC >> 8) & 0xFFu);
    for (i = 3; i < REC_SIZE; i++) {
        sum += buf[i]; /* sum of `seed` bytes = seed * 61 */
    }
    buf[2] = (uint8_t)(sum);
}

/* Corrupt a region into a torn-write look (readable, but invalid) */
static void inject_torn(uint32_t region) {
    memset(&mock_mem[mock.offset[region]], 0x3Cu, REC_SIZE);
}

static elib_safe_ctx_t ctx;
static uint8_t rec[REC_SIZE];

/* ---- tests ---- */

static void test_init_valid_and_rejects(void) {
    printf("Test: init validates the configuration... ");

    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);
    assert(ctx.bit_flags.initialized == 1);

    assert(elib_safe_init(NULL, &mock_ops) == ELIB_SAFE_ERR_INVALID_PARAM);
    assert(elib_safe_init(&ctx, NULL) == ELIB_SAFE_ERR_INVALID_PARAM);
    {
        elib_safe_ops_t bad = mock_ops;
        bad.read = NULL;
        assert(elib_safe_init(&ctx, &bad) == ELIB_SAFE_ERR_INVALID_PARAM);
        bad = mock_ops;
        bad.write = NULL;
        assert(elib_safe_init(&ctx, &bad) == ELIB_SAFE_ERR_INVALID_PARAM);
        bad = mock_ops;
        bad.validate = NULL;
        assert(elib_safe_init(&ctx, &bad) == ELIB_SAFE_ERR_INVALID_PARAM);
    }

    printf("PASSED\n");
}

static void test_read_fresh_media(void) {
    printf("Test: fresh never-written medium reports CORRUPTED... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    /* Both regions read fine (0xFF content), both fail validation */
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    printf("PASSED\n");
}

static void test_write_happy_cycle(void) {
    printf("Test: write lands in both regions, read returns it... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    make_record(rec, 0x11);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    assert(memcmp(&mock_mem[PRIM_OFFSET], rec, REC_SIZE) == 0);
    assert(memcmp(&mock_mem[BACK_OFFSET], rec, REC_SIZE) == 0);

    /* Successful write repairs the fresh-media CORRUPTED state */
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[3] == 0x11); /* seeded payload came back */

    /* Second write shadows the first in both regions */
    {
        uint8_t rec2[REC_SIZE];
        make_record(rec2, 0x22);
        assert(elib_safe_write(&ctx, rec2) == ELIB_SAFE_OK);
        memset(rec, 0, REC_SIZE);
        assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
        assert(rec[3] == 0x22);
    }

    printf("PASSED\n");
}

static void test_write_partial_failures(void) {
    printf("Test: write surfaces every per-region outcome... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    /* Base state: one good record everywhere */
    make_record(rec, 0x30);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    /* Primary ok, backup blocked -> BACKUP_WRITE_FAILED */
    mock.write_fail[ELIB_SAFE_REGION_BACKUP] = 1;
    {
        uint8_t rec2[REC_SIZE];
        make_record(rec2, 0x31);
        assert(elib_safe_write(&ctx, rec2) == ELIB_SAFE_ERR_BACKUP_WRITE_FAILED);
        assert(memcmp(&mock_mem[PRIM_OFFSET], rec2, REC_SIZE) == 0);
        assert(memcmp(&mock_mem[BACK_OFFSET], rec, REC_SIZE) == 0);
    }
    mock.write_fail[ELIB_SAFE_REGION_BACKUP] = 0;

    /* Re-base: one good record in both regions again (content = rec) */
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    /* Primary blocked, backup ok -> PRIMARY_WRITE_FAILED */
    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    {
        uint8_t rec2[REC_SIZE];
        make_record(rec2, 0x32);
        assert(elib_safe_write(&ctx, rec2) == ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED);
        assert(memcmp(&mock_mem[BACK_OFFSET], rec2, REC_SIZE) == 0);
        assert(memcmp(&mock_mem[PRIM_OFFSET], rec, REC_SIZE) == 0);
    }
    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 0;

    /* Re-base: one good record in both regions again (content = rec) */
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    /* Both blocked -> WRITE_FAILED, media untouched */
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

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    /* Bad checksum */
    make_record(rec, 0x40);
    rec[2] ^= 0x01u;
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_ERR_DATA_INVALID);
    /* Bad magic */
    rec[2] ^= 0x01u;
    rec[0] ^= 0xFFu;
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_ERR_DATA_INVALID);

    /* Nothing ever reached the media */
    assert(mock_mem[PRIM_OFFSET] == 0xFF);
    assert(mock_mem[BACK_OFFSET] == 0xFF);

    printf("PASSED\n");
}

static void test_read_repairs_primary(void) {
    printf("Test: torn primary is recovered from backup, then repaired... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    make_record(rec, 0x50);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);

    /* Power loss mid-primary-write: readable garbage */
    inject_torn(ELIB_SAFE_REGION_PRIMARY);
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[3] == 0x50); /* the backup copy came back */

    /* The read silently repaired the primary from this same buffer */
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET], REC_SIZE) == 0);

    /* Next read flows through the (now standing) primary again */
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(rec[3] == 0x50);

    printf("PASSED\n");
}

static void test_read_recover_write_fails(void) {
    printf("Test: backup data returned, but a failed repair is reported... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    make_record(rec, 0x60);
    assert(elib_safe_write(&ctx, rec) == ELIB_SAFE_OK);
    inject_torn(ELIB_SAFE_REGION_PRIMARY);

    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_RECOVER_WRITE_FAILED);
    assert(rec[3] == 0x60); /* caller still holds the valid data */
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET], REC_SIZE) != 0);
    mock.write_fail[ELIB_SAFE_REGION_PRIMARY] = 0;

    /* The very next read repairs the primary and reports plain OK */
    memset(rec, 0, REC_SIZE);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_OK);
    assert(memcmp(&mock_mem[PRIM_OFFSET], &mock_mem[BACK_OFFSET], REC_SIZE) == 0);

    printf("PASSED\n");
}

static void test_read_io_priority(void) {
    printf("Test: media failures outrank content verdicts in read... ");

    memset(mock_mem, 0xFF, sizeof(mock_mem));
    memset(&mock, 0, sizeof(mock));
    mock.offset[0] = PRIM_OFFSET;
    mock.offset[1] = BACK_OFFSET;
    assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);

    /* Pure content case: both read fine, both invalid -> CORRUPTED */
    inject_torn(ELIB_SAFE_REGION_PRIMARY);
    inject_torn(ELIB_SAFE_REGION_BACKUP);
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_CORRUPTED);

    /* Mixed: primary content-invalid, backup read blocked -> IO wins */
    mock.read_fail[ELIB_SAFE_REGION_BACKUP] = 1;
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_IO);
    mock.read_fail[ELIB_SAFE_REGION_BACKUP] = 0;

    /* Mixed the other way: primary read blocked, backup invalid */
    mock.read_fail[ELIB_SAFE_REGION_PRIMARY] = 1;
    assert(elib_safe_read(&ctx, rec) == ELIB_SAFE_ERR_IO);
    mock.read_fail[ELIB_SAFE_REGION_PRIMARY] = 0;

    /* Both blocked -> IO */
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
        mock_t raw_mock;
        elib_safe_ops_t raw_ops = { &raw_mock, mock_read, mock_write, mock_validate };
        uint8_t rec2[REC_SIZE];

        memset(&raw_ctx, 0, sizeof(raw_ctx));
        assert(elib_safe_read(&raw_ctx, rec2) == ELIB_SAFE_ERR_NOT_INITIALIZED);
        assert(elib_safe_write(&raw_ctx, rec2) == ELIB_SAFE_ERR_NOT_INITIALIZED);

        /* initialized instances reject NULL buffers */
        assert(elib_safe_init(&ctx, &mock_ops) == ELIB_SAFE_OK);
        assert(elib_safe_read(&ctx, NULL) == ELIB_SAFE_ERR_INVALID_PARAM);
        assert(elib_safe_write(&ctx, NULL) == ELIB_SAFE_ERR_INVALID_PARAM);
        (void)raw_ops;
    }

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

    printf("All tests PASSED\n");
    return 0;
}
