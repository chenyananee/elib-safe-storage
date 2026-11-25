/* elib_safe_core.c - Safe Storage implementation
 *
 * Two flows, selected by the mode registered at init:
 *
 *   DIRECT        the backend is the medium. A torn write leaves the record
 *                 invalid, so the primary is either the latest record or
 *                 unusable and "primary first" is correct. This is the v2
 *                 behaviour, unchanged.
 *
 *   WEAR_LEVELED  the backend is a wear-leveling layer whose "latest" can
 *                 regress to an older but valid record. The first 4 bytes of
 *                 every record hold a library-owned version; both regions are
 *                 read, and the newer one wins. When the backup wins the
 *                 primary is repaired from it.
 *
 * Version slot (WEAR_LEVELED, library private):
 *   4 bytes, little-endian, valid range [1, 0xFFFFFFFE]; 0x00000000 and
 *   0xFFFFFFFF are invalid because they are the all-0 / all-1 erased states of
 *   common media. Because the user never touches the slot, the fixed encoding
 *   removes any struct endianness concern.
 *
 * The version counter is seeded by elib_safe_read() from the regions it could
 * fully read; write refuses to run before that (ELIB_SAFE_ERR_NO_VERSION), so a
 * freshly written record can never end up with a version lower than what is
 * already on the media.
 */
#include <stddef.h>

#include "elib_safe.h"

/* ---- version slot helpers ---- */

#define SAFE_VERSION_MIN 1u
#define SAFE_VERSION_MAX 0xFFFFFFFEu

static uint32_t load_version(const uint8_t *buf) {
    return (uint32_t)buf[0] |
           ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] << 24);
}

static void store_version(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)(v & 0xFFu);
    buf[1] = (uint8_t)((v >> 8) & 0xFFu);
    buf[2] = (uint8_t)((v >> 16) & 0xFFu);
    buf[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int version_is_valid(uint32_t v) {
    return (v >= SAFE_VERSION_MIN) && (v <= SAFE_VERSION_MAX);
}

/* wrap-safe "a is newer than b" (the invalid sentinels never reach here) */
static int version_is_newer(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) > 0;
}

/* next version, skipping both invalid sentinels (0xFFFFFFFE -> 1) */
static uint32_t next_version(uint32_t v) {
    if (v >= SAFE_VERSION_MAX) {
        return SAFE_VERSION_MIN;
    }
    return v + 1u;
}

/* ---- read: DIRECT (v2 behaviour) ---- */

static elib_safe_err_t read_direct(const elib_safe_ops_t *ops, uint8_t *buf) {
    uint8_t io_seen = 0; /* any backend read returned non-zero */

    /* Primary first. Reading fine but failing validation is deferred to the
     * final verdict; the fallback proceeds either way. */
    if (ops->read(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) == 0) {
        if (ops->validate(ops->user_data, buf) != 0) {
            return ELIB_SAFE_OK;
        }
    } else {
        io_seen = 1;
    }

    /* Fallback: the backup region. When it stands, push a copy back into the
     * primary so the repair is implicit - the data is already in buf. */
    if (ops->read(ops->user_data, ELIB_SAFE_REGION_BACKUP, buf) == 0) {
        if (ops->validate(ops->user_data, buf) != 0) {
            if (ops->write(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) == 0) {
                return ELIB_SAFE_OK;
            }
            return ELIB_SAFE_ERR_RECOVER_WRITE_FAILED;
        }
    } else {
        io_seen = 1;
    }

    return (io_seen != 0) ? ELIB_SAFE_ERR_IO : ELIB_SAFE_ERR_CORRUPTED;
}

/* ---- read: WEAR_LEVELED ---- */

static elib_safe_err_t read_wear_leveled(elib_safe_ctx_t *ctx, uint8_t *buf) {
    const elib_safe_ops_t *ops = &ctx->ops;
    uint8_t io_seen = 0;
    uint8_t p_read, b_read;
    uint8_t p_ok, b_ok;
    uint32_t p_ver = 0u, b_ver = 0u;

    p_read = (uint8_t)(ops->read(ops->user_data, ELIB_SAFE_REGION_PRIMARY,
                                 buf) == 0);
    if (!p_read) {
        io_seen = 1;
    }
    p_ok = (uint8_t)(p_read && (ops->validate(ops->user_data, buf) != 0));
    if (p_ok) {
        p_ver = load_version(buf);
        p_ok = (uint8_t)version_is_valid(p_ver);
    }

    b_read = (uint8_t)(ops->read(ops->user_data, ELIB_SAFE_REGION_BACKUP,
                                 buf) == 0);
    if (!b_read) {
        io_seen = 1;
    }
    b_ok = (uint8_t)(b_read && (ops->validate(ops->user_data, buf) != 0));
    if (b_ok) {
        b_ver = load_version(buf);
        b_ok = (uint8_t)version_is_valid(b_ver);
    }

    /* Seed the counter only on a complete read: with one region unreadable we
     * cannot know the true maximum, and a too-low seed could push a later
     * write behind the unreadable record. */
    if (p_read && b_read) {
        uint32_t max_ver = 0u;
        if (p_ok && b_ok) {
            max_ver = version_is_newer(p_ver, b_ver) ? p_ver : b_ver;
        } else if (p_ok) {
            max_ver = p_ver;
        } else if (b_ok) {
            max_ver = b_ver;
        }
        ctx->version = max_ver;
        ctx->bit_flags.version_known = 1u;
    }

    if (p_ok && b_ok) {
        if (version_is_newer(p_ver, b_ver)) {
            /* primary is newer, but buf holds the backup read: reload */
            if (ops->read(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) != 0) {
                return ELIB_SAFE_ERR_IO;
            }
            return ELIB_SAFE_OK;
        }
        if (version_is_newer(b_ver, p_ver)) {
            /* backup is newer and buf already holds it: repair the primary */
            if (ops->write(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) != 0) {
                return ELIB_SAFE_ERR_RECOVER_WRITE_FAILED;
            }
            return ELIB_SAFE_OK;
        }
        /* equal version: versions are unique, so buf == primary content */
        return ELIB_SAFE_OK;
    }

    if (b_ok) {
        /* only the backup stands; buf already holds it */
        if (ops->write(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) != 0) {
            return ELIB_SAFE_ERR_RECOVER_WRITE_FAILED;
        }
        return ELIB_SAFE_OK;
    }

    if (p_ok) {
        /* only the primary stands; buf holds the failed backup read: reload */
        if (ops->read(ops->user_data, ELIB_SAFE_REGION_PRIMARY, buf) != 0) {
            return ELIB_SAFE_ERR_IO;
        }
        return ELIB_SAFE_OK;
    }

    return (io_seen != 0) ? ELIB_SAFE_ERR_IO : ELIB_SAFE_ERR_CORRUPTED;
}

/* ---- API ---- */

elib_safe_err_t elib_safe_init(elib_safe_ctx_t *ctx, const elib_safe_ops_t *ops,
                               elib_safe_mode_t mode) {
    if (ctx == NULL || ops == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (mode != ELIB_SAFE_MODE_DIRECT && mode != ELIB_SAFE_MODE_WEAR_LEVELED) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (ops->read == NULL || ops->write == NULL || ops->validate == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }

    ctx->ops = *ops;
    ctx->version = 0u;
    ctx->bit_flags.initialized = 1u;
    ctx->bit_flags.wear_leveled =
        (mode == ELIB_SAFE_MODE_WEAR_LEVELED) ? 1u : 0u;
    ctx->bit_flags.version_known = 0u;
    return ELIB_SAFE_OK;
}

elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, uint8_t *buf) {
    if (ctx == NULL || buf == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->bit_flags.initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }
    if (ctx->bit_flags.wear_leveled) {
        return read_wear_leveled(ctx, buf);
    }
    return read_direct(&ctx->ops, buf);
}

elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, uint8_t *buf) {
    uint8_t primary_failed;
    uint8_t backup_failed;
    uint32_t v = 0u;

    if (ctx == NULL || buf == NULL) {
        return ELIB_SAFE_ERR_INVALID_PARAM;
    }
    if (!ctx->bit_flags.initialized) {
        return ELIB_SAFE_ERR_NOT_INITIALIZED;
    }

    /* Entry validation is the user's own verdict on the fresh record; a
     * rejected record must never reach the media. */
    if (ctx->ops.validate(ctx->ops.user_data, buf) == 0) {
        return ELIB_SAFE_ERR_DATA_INVALID;
    }

    if (ctx->bit_flags.wear_leveled) {
        if (!ctx->bit_flags.version_known) {
            return ELIB_SAFE_ERR_NO_VERSION;
        }
        v = next_version(ctx->version);
        store_version(buf, v);
    }

    /* Primary first, backup second - unconditionally, so the backup keeps
     * guarding even when the primary write fails. */
    primary_failed = (uint8_t)(ctx->ops.write(ctx->ops.user_data,
                                              ELIB_SAFE_REGION_PRIMARY,
                                              buf) != 0);
    backup_failed = (uint8_t)(ctx->ops.write(ctx->ops.user_data,
                                             ELIB_SAFE_REGION_BACKUP,
                                             buf) != 0);

    /* Advance the counter when the record reached at least one region, so the
     * next write is strictly newer than what was just persisted. */
    if (ctx->bit_flags.wear_leveled && (!primary_failed || !backup_failed)) {
        ctx->version = v;
    }

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
