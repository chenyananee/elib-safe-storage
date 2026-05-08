/* elib_safe.h - Safe Storage Main Header */
#ifndef ELIB_SAFE_H
#define ELIB_SAFE_H

#include "elib_safe_err.h"
#include "elib_safe_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize safe storage instance
 * @param ctx User-allocated context pointer
 * @param backend Backend interface (read/write functions)
 * @param primary_addr Primary region start address
 * @param backup_addr Backup region start address
 * @param data_size Data size (user must include validation fields)
 * @param magic User-defined magic value for validation callback
 * @param validate Validation callback function
 * @return elib_safe_err_t error code
 */
elib_safe_err_t elib_safe_init(
    elib_safe_ctx_t *ctx,
    const elib_safe_backend_t *backend,
    uint32_t primary_addr,
    uint32_t backup_addr,
    uint32_t data_size,
    uint16_t magic,
    elib_safe_validate_fn validate
);

/**
 * @brief Write data to primary and backup regions
 * @param ctx Context pointer
 * @param data Data buffer to write (size = data_size)
 * @return elib_safe_err_t error code
 */
elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, const void *data);

/**
 * @brief Read data with automatic recovery
 * @param ctx Context pointer
 * @param out Output buffer (size = data_size)
 * @return elib_safe_err_t error code
 * @note If primary is corrupted, reads from backup and auto-recovers primary
 */
elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, void *out);

/**
 * @brief Force recovery from backup to primary
 * @param ctx Context pointer
 * @return elib_safe_err_t error code
 */
elib_safe_err_t elib_safe_recover(elib_safe_ctx_t *ctx);

/**
 * @brief Deinitialize safe storage instance
 * @param ctx Context pointer
 */
void elib_safe_deinit(elib_safe_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ELIB_SAFE_H */
