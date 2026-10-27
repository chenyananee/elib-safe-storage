/* elib_safe_core.c - Safe Storage implementation
 *
 * Single-file implementation of the dual-region tolerant storage
 * model. Terminology:
 *   region tag    - which of the two storage slots an op targets
 *                   (primary/backup); physical addressing is the
 *                   user implementation's private business
 *   standing      - a region "stands" while its record passes the
 *                   user validate callback
 *
 * Flows are entirely stateless: write pushes the record to primary
 * then backup (whatever the outcomes), read prefers primary and
 * repairs it from backup when the primary does not stand. Media-level
 * failures (backend op non-zero) outrank content verdicts (a region
 * that reads fine but fails validation) when neither region yields
 * a valid record - a torn medium deserves a different report than a
 * fresh, never-written one.
 */
#include <stddef.h>

#include "elib_safe.h"

/* ---- API ---- */

elib_safe_err_t elib_safe_init(elib_safe_ctx_t *ctx, const elib_safe_ops_t *ops) {
    if (ctx == NULL || ops == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (ops->read == NULL || ops->write == NULL || ops->validate == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }

    ctx->ops = *ops;
    ctx->bit_flags.initialized = 1;
    return ELIB_SAFE_OK;
}

elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, uint8_t *buf) {
    const elib_safe_ops_t *ops;
    uint8_t io_seen = 0;          /* any backend read returned non-zero */

    if (ctx == NULL || buf == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->bit_flags.initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }
    ops = &ctx->ops;

    /* Primary first. Reading fine but failing validation is deferred
     * to the final verdict; the fallback proceeds either way. */
    if (ops->read(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) == 0) {
        if (ops->validate(ops->user_data, buf) != 0) {
            return ELIB_SAFE_OK;
        }
    } else {
        io_seen = 1;
    }

    /* Fallback: the backup region. When it stands, push a copy back
     * into the primary so the repair is implicit - the data is already
     * in the caller's buf from this very read. */
    if (ops->read(ops->user_data, ELIB_SAFE_REGION_BACKUP, buf) == 0) {
        if (ops->validate(ops->user_data, buf) != 0) {
            if (ops->write(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) == 0) {
                return ELIB_SAFE_OK;
            }
            /* the caller got the valid data, but the primary stays
             * broken - not silent, not a hard failure */
            return ELIB_SAFE_ERR_RECOVER_WRITE_FAILED;
        }
    } else {
        io_seen = 1;
    }

    /* Nowhere valid. Media failure wins over content verdicts. */
    return (io_seen != 0) ? ELIB_SAFE_ERR_IO : ELIB_SAFE_ERR_CORRUPTED;
}

elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, const uint8_t *buf) {
    uint8_t primary_failed;
    uint8_t backup_failed;

    if (ctx == NULL || buf == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->bit_flags.initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }

    /* Entry validation is the user's own verdict on the fresh record;
     * a rejected record must never reach the media. */
    if (ctx->ops.validate(ctx->ops.user_data, buf) == 0) {
        return ELIB_SAFE_ERR_DATA_INVALID;
    }

    /* Primary first, backup second - unconditionally, so the backup
     * keeps guarding even when the primary write fails. */
    primary_failed = (ctx->ops.write(ctx->ops.user_data,
                                     ELIB_SAFE_REGION_PRIMARY, buf) != 0);
    backup_failed = (ctx->ops.write(ctx->ops.user_data,
                                    ELIB_SAFE_REGION_BACKUP, buf) != 0);

    if (!primary_failed && !backup_failed) {
        return ELIB_SAFE_OK;
    }
    if (!primary_failed) {
        return ELIB_SAFE_ERR_BACKUP_WRITE_FAILED; /* record is in the primary */
    }
    if (!backup_failed) {
        return ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED; /* record is in the backup */
    }
    return ELIB_SAFE_ERR_WRITE_FAILED;
}
