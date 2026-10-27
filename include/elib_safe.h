/* elib_safe.h - Safe Storage (single-umbrella public API)
 *
 * Power-loss tolerant dual-region storage: every record is written to
 * a primary region first and a backup region second, so a torn write
 * in one of them never destroys the last known-good copy.
 *
 * Rules of the game:
 *  - zero dynamic memory: the user allocates the context, the library
 *    allocates nothing
 *  - the library owns no addressing: backend ops receive only a region
 *    tag; physical layout, record size and erase strategy all live
 *    inside the user implementation
 *  - record validity is user-defined (validate callback); the library
 *    never inspects content bytes
 *  - semantics: "the last known-good record". After a torn primary
 *    write, reading the outdated-but-valid backup is a legal result */
#ifndef ELIB_SAFE_H
#define ELIB_SAFE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- error codes ---- */

typedef enum {
    ELIB_SAFE_OK = 0,
    ELIB_SAFE_ERR_INVALID_PARAM,        /* init rejected the configuration */
    ELIB_SAFE_ERR_NOT_INITIALIZED,      /* API called before a successful init */
    ELIB_SAFE_ERR_IO,                   /* backend read returned non-zero with
                                             nowhere valid to fall back to */
    ELIB_SAFE_ERR_DATA_INVALID,         /* write entry validation rejected
                                             the record; nothing is written */
    ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED, /* record landed in backup only */
    ELIB_SAFE_ERR_BACKUP_WRITE_FAILED,  /* record landed in primary only */
    ELIB_SAFE_ERR_WRITE_FAILED,         /* neither region took the record */
    ELIB_SAFE_ERR_RECOVER_WRITE_FAILED, /* read: data from backup is valid but
                                             the primary repair write failed */
    ELIB_SAFE_ERR_CORRUPTED             /* read: both regions read fine but
                                             hold invalid records (a fresh,
                                             never-written medium lands here) */
} elib_safe_err_t;

/* ---- address designations ----
 *
 * The physical start addresses of the primary/backup regions are
 * chosen inside the user's read/write implementation; the library only
 * ever refers to which one. */
typedef enum {
    ELIB_SAFE_REGION_PRIMARY = 0,
    ELIB_SAFE_REGION_BACKUP = 1
} elib_safe_region_t;

/* ---- user backend operations ----
 *
 * read/write return 0 on success and any non-zero value of the user's
 * own choosing on failure (a failed read makes that region "not
 * standing"). Records travel as one fixed, user-owned chunk of bytes
 * per call - no length parameter crosses the boundary.
 *
 * validate is the sole authority on record validity (magic, CRC,
 * whatever the user saw fit to put in the payload); it must answer 1
 * exactly when the buffer holds one complete, usable record. */
typedef struct {
    void *user_data; /* forwarded to every op, untouched by the library */
    int     (*read)    (void *ud, elib_safe_region_t region, uint8_t *buf);
    int     (*write)   (void *ud, elib_safe_region_t region, const uint8_t *buf);
    uint8_t (*validate)(void *ud, const uint8_t *buf);
} elib_safe_ops_t;

/* ---- context (statically allocated by the user) ---- */
typedef struct {
    elib_safe_ops_t ops;

    /* runtime state: the read/write flows are entirely stateless */
    struct {
        uint8_t initialized : 1;
    } bit_flags;
} elib_safe_ctx_t;

/* ---- API ---- */

/**
 * @brief 注册底层接口并初始化上下文
 * @param ctx 用户静态分配的上下文
 * @param ops 三个底层回调（read/write/validate，均不可为 NULL）
 * @return ELIB_SAFE_OK，或 ELIB_SAFE_ERR_INVALID_PARAM */
elib_safe_err_t elib_safe_init(elib_safe_ctx_t *ctx, const elib_safe_ops_t *ops);

/**
 * @brief 读最后一条完好记录；主区站不住时降级备区并顺手修复主区
 * @param buf 收到记录内容的缓冲（大小由用户层自理）
 * @return ELIB_SAFE_OK / ELIB_SAFE_ERR_RECOVER_WRITE_FAILED / ERR_IO
 *         / ERR_CORRUPTED / ERR_NOT_INITIALIZED / ERR_INVALID_PARAM */
elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, uint8_t *buf);

/**
 * @brief 写一条新记录：先校验，再先主后备（无论成败）
 * @param buf 待写记录（大小由用户层自理）
 * @return ELIB_SAFE_OK / ERR_DATA_INVALID / ERR_PRIMARY_WRITE_FAILED
 *         / ERR_BACKUP_WRITE_FAILED / ERR_WRITE_FAILED / ... */
elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, const uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_SAFE_H */
