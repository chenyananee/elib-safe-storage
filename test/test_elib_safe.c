/* test_elib_safe.c - Safe Storage Unit Tests */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../include/elib_safe.h"

/* Mock storage */
#define STORAGE_SIZE 256
static uint8_t mock_storage[STORAGE_SIZE];
static int mock_read_fail = 0;
static int mock_write_fail = 0;

/* Mock backend functions */
static int mock_read(uint32_t addr, void *buf, size_t len) {
    if (mock_read_fail) return -1;
    if (addr + len > STORAGE_SIZE) return -1;
    memcpy(buf, &mock_storage[addr], len);
    return 0;
}

static int mock_write(uint32_t addr, const void *buf, size_t len) {
    if (mock_write_fail) return -1;
    if (addr + len > STORAGE_SIZE) return -1;
    memcpy(&mock_storage[addr], buf, len);
    return 0;
}

static elib_safe_backend_t mock_backend = {
    .read = mock_read,
    .write = mock_write,
};

/* Test data structure */
typedef struct {
    uint16_t magic;
    uint16_t crc;
    uint32_t value;
} test_data_t;

#define TEST_MAGIC 0xA5A5

/* Simple validation */
static bool test_validate(const void *data, size_t len) {
    const test_data_t *td = (const test_data_t *)data;
    if (td->magic != TEST_MAGIC) return false;
    /* Simple checksum for testing */
    uint16_t sum = 0;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len - 2; i++) {
        sum += p[i];
    }
    return sum == td->crc;
}

static void reset_mock_storage(void) {
    memset(mock_storage, 0xFF, STORAGE_SIZE);
    mock_read_fail = 0;
    mock_write_fail = 0;
}

/* Test: init with valid parameters */
static void test_init_valid(void) {
    printf("Test: init with valid parameters... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_err_t err = elib_safe_init(&ctx, &mock_backend, 0, 64,
                                          sizeof(test_data_t), TEST_MAGIC, test_validate);

    assert(err == ELIB_SAFE_OK);
    assert(ctx.initialized == 1);
    assert(ctx.primary_addr == 0);
    assert(ctx.backup_addr == 64);
    assert(ctx.data_size == sizeof(test_data_t));
    assert(ctx.magic == TEST_MAGIC);
    assert(ctx.validate == test_validate);

    printf("PASSED\n");
}

/* Test: init with null ctx */
static void test_init_null_ctx(void) {
    printf("Test: init with null ctx... ");
    reset_mock_storage();

    elib_safe_err_t err = elib_safe_init(NULL, &mock_backend, 0, 64,
                                          sizeof(test_data_t), TEST_MAGIC, test_validate);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: init with null backend */
static void test_init_null_backend(void) {
    printf("Test: init with null backend... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_err_t err = elib_safe_init(&ctx, NULL, 0, 64,
                                          sizeof(test_data_t), TEST_MAGIC, test_validate);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: init with null validate */
static void test_init_null_validate(void) {
    printf("Test: init with null validate... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_err_t err = elib_safe_init(&ctx, &mock_backend, 0, 64,
                                          sizeof(test_data_t), TEST_MAGIC, NULL);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: init with zero data_size */
static void test_init_zero_data_size(void) {
    printf("Test: init with zero data_size... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_err_t err = elib_safe_init(&ctx, &mock_backend, 0, 64,
                                          0, TEST_MAGIC, test_validate);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: write valid data */
static void test_write_valid(void) {
    printf("Test: write valid data... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    test_data_t data = {
        .magic = TEST_MAGIC,
        .value = 0x12345678
    };
    data.crc = (uint16_t)(data.magic + (data.value & 0xFFFF) + ((data.value >> 16) & 0xFFFF));

    elib_safe_err_t err = elib_safe_write(&ctx, &data);

    assert(err == ELIB_SAFE_OK);

    test_data_t primary_read;
    mock_read(0, &primary_read, sizeof(test_data_t));
    assert(primary_read.magic == TEST_MAGIC);
    assert(primary_read.value == 0x12345678);

    test_data_t backup_read;
    mock_read(64, &backup_read, sizeof(test_data_t));
    assert(backup_read.magic == TEST_MAGIC);
    assert(backup_read.value == 0x12345678);

    printf("PASSED\n");
}

/* Test: write with null ctx */
static void test_write_null_ctx(void) {
    printf("Test: write with null ctx... ");
    reset_mock_storage();

    test_data_t data = {0};
    elib_safe_err_t err = elib_safe_write(NULL, &data);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: write with null data */
static void test_write_null_data(void) {
    printf("Test: write with null data... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    elib_safe_err_t err = elib_safe_write(&ctx, NULL);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: write to uninitialized context */
static void test_write_uninit(void) {
    printf("Test: write to uninitialized context... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx = {0};
    test_data_t data = {0};

    elib_safe_err_t err = elib_safe_write(&ctx, &data);

    assert(err == ELIB_SAFE_ERR_NOT_INITIALIZED);

    printf("PASSED\n");
}

/* Test: read valid data from primary */
static void test_read_valid_primary(void) {
    printf("Test: read valid data from primary... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    test_data_t write_data = {
        .magic = TEST_MAGIC,
        .value = 0xDEADBEEF
    };
    write_data.crc = (uint16_t)(write_data.magic + (write_data.value & 0xFFFF) + ((write_data.value >> 16) & 0xFFFF));
    elib_safe_write(&ctx, &write_data);

    test_data_t read_data;
    elib_safe_err_t err = elib_safe_read(&ctx, &read_data);

    assert(err == ELIB_SAFE_OK);
    assert(read_data.magic == TEST_MAGIC);
    assert(read_data.value == 0xDEADBEEF);

    printf("PASSED\n");
}

/* Test: read recovers from backup when primary corrupted */
static void test_read_recover_from_backup(void) {
    printf("Test: read recovers from backup when primary corrupted... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    test_data_t write_data = {
        .magic = TEST_MAGIC,
        .value = 0xCAFEBABE
    };
    write_data.crc = (uint16_t)(write_data.magic + (write_data.value & 0xFFFF) + ((write_data.value >> 16) & 0xFFFF));
    elib_safe_write(&ctx, &write_data);

    mock_storage[0] = 0x00;

    test_data_t read_data;
    elib_safe_err_t err = elib_safe_read(&ctx, &read_data);

    assert(err == ELIB_SAFE_OK);
    assert(read_data.magic == TEST_MAGIC);
    assert(read_data.value == 0xCAFEBABE);

    test_data_t primary_check;
    mock_read(0, &primary_check, sizeof(test_data_t));
    assert(primary_check.magic == TEST_MAGIC);
    assert(primary_check.value == 0xCAFEBABE);

    printf("PASSED\n");
}

/* Test: read returns corrupted when both regions invalid */
static void test_read_both_corrupted(void) {
    printf("Test: read returns corrupted when both regions invalid... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    memset(mock_storage, 0x00, STORAGE_SIZE);

    test_data_t read_data;
    elib_safe_err_t err = elib_safe_read(&ctx, &read_data);

    assert(err == ELIB_SAFE_ERR_CORRUPTED);

    printf("PASSED\n");
}

/* Test: read with null ctx */
static void test_read_null_ctx(void) {
    printf("Test: read with null ctx... ");
    reset_mock_storage();

    test_data_t data;
    elib_safe_err_t err = elib_safe_read(NULL, &data);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: read with null output */
static void test_read_null_output(void) {
    printf("Test: read with null output... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    elib_safe_err_t err = elib_safe_read(&ctx, NULL);

    assert(err == ELIB_SAFE_ERR_INVALID_PARAM);

    printf("PASSED\n");
}

/* Test: recover from backup */
static void test_recover_valid(void) {
    printf("Test: recover from backup... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    test_data_t write_data = {
        .magic = TEST_MAGIC,
        .value = 0x11111111
    };
    write_data.crc = (uint16_t)(write_data.magic + (write_data.value & 0xFFFF) + ((write_data.value >> 16) & 0xFFFF));
    elib_safe_write(&ctx, &write_data);

    mock_storage[0] = 0x00;

    elib_safe_err_t err = elib_safe_recover(&ctx);

    assert(err == ELIB_SAFE_OK);

    test_data_t primary_check;
    mock_read(0, &primary_check, sizeof(test_data_t));
    assert(primary_check.magic == TEST_MAGIC);
    assert(primary_check.value == 0x11111111);

    printf("PASSED\n");
}

/* Test: recover with corrupted backup */
static void test_recover_corrupted_backup(void) {
    printf("Test: recover with corrupted backup... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    memset(mock_storage, 0x00, STORAGE_SIZE);

    elib_safe_err_t err = elib_safe_recover(&ctx);

    assert(err == ELIB_SAFE_ERR_CORRUPTED);

    printf("PASSED\n");
}

/* Test: deinit marks context uninitialized */
static void test_deinit(void) {
    printf("Test: deinit marks context uninitialized... ");
    reset_mock_storage();

    elib_safe_ctx_t ctx;
    elib_safe_init(&ctx, &mock_backend, 0, 64, sizeof(test_data_t), TEST_MAGIC, test_validate);

    assert(ctx.initialized == 1);

    elib_safe_deinit(&ctx);

    assert(ctx.initialized == 0);

    test_data_t data = {0};
    elib_safe_err_t err = elib_safe_write(&ctx, &data);
    assert(err == ELIB_SAFE_ERR_NOT_INITIALIZED);

    printf("PASSED\n");
}

int main(void) {
    printf("=== elib-safe-storage tests ===\n\n");

    test_init_valid();
    test_init_null_ctx();
    test_init_null_backend();
    test_init_null_validate();
    test_init_zero_data_size();

    test_write_valid();
    test_write_null_ctx();
    test_write_null_data();
    test_write_uninit();

    test_read_valid_primary();
    test_read_recover_from_backup();
    test_read_both_corrupted();
    test_read_null_ctx();
    test_read_null_output();

    test_recover_valid();
    test_recover_corrupted_backup();
    test_deinit();

    printf("\n=== All tests passed ===\n");
    return 0;
}
