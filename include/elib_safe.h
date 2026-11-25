/* elib_safe.h - Safe Storage (single-umbrella public API)
 *
 * Power-loss tolerant dual-region storage: every record is written to a
 * primary region first and a backup region second, so a torn write in one of
 * them never destroys the last known-good copy.
 *
 * Two backends, two modes:
 *
 *  - ELIB_SAFE_MODE_DIRECT: the backend is the medium itself. A torn write
 *    leaves the record invalid, so the primary is either the latest or
 *    unusable and "primary first" is always correct. No version is involved.
 *
 *  - ELIB_SAFE_MODE_WEAR_LEVELED: the backend is a wear-leveling layer
 *    (elib-nvs-wear-leveling). Its "latest" can regress to an older but
 *    CRC-valid record after a flash page loss, so "primary first" would
 *    silently roll back. In this mode the FIRST 4 BYTES of every record are
 *    owned by the library: it keeps a monotonic version there, advances it on
 *    every write and compares the two regions on read to return the newer one.
 *    The user reserves those 4 bytes, never reads or writes them, and keeps
 *    them out of the record's own validation (the library changes them).
 *
 *    Version encoding (library private): little-endian 32-bit, valid range
 *    [1, 0xFFFFFFFE]; 0x00000000 and 0xFFFFFFFF are invalid (they are the
 *    all-0 / all-1 erased states of common media). Because the user never
 *    touches the 4 bytes, no struct endianness issue can arise.
 *
 *    The version counter is seeded by elib_safe_read(); call read before the
 *    first write of a power cycle, otherwise write returns
 *    ELIB_SAFE_ERR_NO_VERSION.
 *
 * Rules of the game:
 *  - zero dynamic memory: the user allocates the context, the library
 *    allocates nothing
 *  - the library owns no addressing: backend ops receive only a region tag;
 *    physical layout and record size live inside the user implementation
 *  - record validity is user-defined (validate callback); the library never
 *    inspects content bytes beyond the 4-byte version slot
 */
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
    ELIB_SAFE_ERR_CORRUPTED,            /* read: both regions read fine but
                                             hold invalid records (a fresh,
                                             never-written medium lands here) */
    ELIB_SAFE_ERR_NO_VERSION            /* WEAR_LEVELED: write before any read
                                             seeded the version counter */
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

/* ---- backend mode ---- */
typedef enum {
    ELIB_SAFE_MODE_DIRECT       = 0, /* raw medium: primary is authoritative */
    ELIB_SAFE_MODE_WEAR_LEVELED = 1  /* on a wear-leveling layer: 4-byte version */
} elib_safe_mode_t;

/* ---- user backend operations ----
 *
 * read/write return 0 on success and any non-zero value of the user's
 * own choosing on failure (a failed read makes that region "not
 * standing"). Records travel as one fixed, user-owned chunk of bytes
 * per call - no length parameter crosses the boundary.
 *
 * validate is the sole authority on record validity (magic, CRC,
 * whatever the user saw fit to put in the payload); it must answer 1
 * exactly when the buffer holds one complete, usable record.
 *
 * In ELIB_SAFE_MODE_WEAR_LEVELED the first 4 bytes are the library's
 * version slot: validate MUST ignore them (the library rewrites them).
 */
typedef struct {
    void *user_data; /* forwarded to every op, untouched by the library */
    int     (*read)    (void *ud, elib_safe_region_t region, uint8_t *buf);
    int     (*write)   (void *ud, elib_safe_region_t region, const uint8_t *buf);
    uint8_t (*validate)(void *ud, const uint8_t *buf);
} elib_safe_ops_t;

/* ---- context (statically allocated by the user) ---- */
typedef struct {
    elib_safe_ops_t ops;

    /* WEAR_LEVELED runtime state */
    uint32_t version;          /* latest known version, 0 = none yet */

    struct {
        uint8_t initialized   : 1;
        uint8_t wear_leveled  : 1; /* mode == ELIB_SAFE_MODE_WEAR_LEVELED */
        uint8_t version_known : 1; /* read seeded the counter */
    } bit_flags;
} elib_safe_ctx_t;

/* ---- API ---- */

/**
 * @brief 注册底层接口并初始化上下文
 * @param ctx 用户静态分配的上下文
 * @param ops 三个底层回调（read/write/validate，均不可为 NULL）
 * @param mode ELIB_SAFE_MODE_DIRECT 或 ELIB_SAFE_MODE_WEAR_LEVELED
 * @return ELIB_SAFE_OK，或 ELIB_SAFE_ERR_INVALID_PARAM */
elib_safe_err_t elib_safe_init(elib_safe_ctx_t *ctx, const elib_safe_ops_t *ops,
                               elib_safe_mode_t mode);

/**
 * @brief 读最新一条完好记录
 * @param buf 收到记录内容的缓冲（大小由用户层自理）
 * @return ELIB_SAFE_OK / ELIB_SAFE_ERR_RECOVER_WRITE_FAILED / ERR_IO
 *         / ERR_CORRUPTED / ERR_NOT_INITIALIZED / ERR_INVALID_PARAM
 *
 * WEAR_LEVELED 下同时播种版本计数器：两区都成功读出时才会播种。 */
elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, uint8_t *buf);

/**
 * @brief 写一条新记录：先校验，再先主后备（无论成败）
 * @param buf 待写记录；WEAR_LEVELED 下库会就地改写前 4 字节版本槽
 * @return ELIB_SAFE_OK / ERR_DATA_INVALID / ERR_PRIMARY_WRITE_FAILED
 *         / ERR_BACKUP_WRITE_FAILED / ERR_WRITE_FAILED / ERR_NO_VERSION / ... */
elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_SAFE_H */
