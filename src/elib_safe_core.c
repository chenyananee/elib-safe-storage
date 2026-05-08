/* elib_safe_core.c - Safe Storage Core Implementation */
#include "../include/elib_safe.h"
#include <string.h>

elib_safe_err_t elib_safe_init(
    elib_safe_ctx_t *ctx,
    const elib_safe_backend_t *backend,
    uint32_t primary_addr,
    uint32_t backup_addr,
    uint32_t data_size,
    uint16_t magic,
    elib_safe_validate_fn validate
) {
    /* Parameter validation */
    if (ctx == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (backend == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (backend->read == NULL || backend->write == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (validate == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (data_size == 0) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }

    /* Initialize context */
    memset(ctx, 0, sizeof(elib_safe_ctx_t));
    memcpy(&ctx->backend, backend, sizeof(elib_safe_backend_t));
    ctx->primary_addr = primary_addr;
    ctx->backup_addr = backup_addr;
    ctx->data_size = data_size;
    ctx->magic = magic;
    ctx->validate = validate;
    ctx->initialized = 1;

    return ELIB_SAFE_OK;
}

elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, const void *data) {
    if (ctx == NULL || data == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }

    int result = ctx->backend.write(ctx->primary_addr, data, ctx->data_size);
    if (result != 0) {
        return ELIB_SAFE_ERR_WRITE_FAILED;
    }

    result = ctx->backend.write(ctx->backup_addr, data, ctx->data_size);
    if (result != 0) {
        return ELIB_SAFE_ERR_WRITE_FAILED;
    }

    return ELIB_SAFE_OK;
}

elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, void *out) {
    if (ctx == NULL || out == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }

    int result = ctx->backend.read(ctx->primary_addr, out, ctx->data_size);
    if (result != 0) {
        return ELIB_SAFE_ERR_READ_FAILED;
    }

    if (ctx->validate(out, ctx->data_size)) {
        return ELIB_SAFE_OK;
    }

    result = ctx->backend.read(ctx->backup_addr, out, ctx->data_size);
    if (result != 0) {
        return ELIB_SAFE_ERR_READ_FAILED;
    }

    if (ctx->validate(out, ctx->data_size)) {
        ctx->backend.write(ctx->primary_addr, out, ctx->data_size);
        return ELIB_SAFE_OK;
    }

    return ELIB_SAFE_ERR_CORRUPTED;
}

elib_safe_err_t elib_safe_recover(elib_safe_ctx_t *ctx) {
    if (ctx == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }

    uint8_t temp_data[ctx->data_size];

    int result = ctx->backend.read(ctx->backup_addr, temp_data, ctx->data_size);
    if (result != 0) {
        return ELIB_SAFE_ERR_READ_FAILED;
    }

    if (!ctx->validate(temp_data, ctx->data_size)) {
        return ELIB_SAFE_ERR_CORRUPTED;
    }

    result = ctx->backend.write(ctx->primary_addr, temp_data, ctx->data_size);
    if (result != 0) {
        return ELIB_SAFE_ERR_WRITE_FAILED;
    }

    return ELIB_SAFE_OK;
}

void elib_safe_deinit(elib_safe_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }
    ctx->initialized = 0;
}
