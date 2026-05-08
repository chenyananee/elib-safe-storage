/* elib_safe_types.h - Safe Storage Type Definitions */
#ifndef ELIB_SAFE_TYPES_H
#define ELIB_SAFE_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include "elib_safe_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backend interface - user implements read/write functions */
typedef struct {
    int (*read)(uint32_t addr, void *buf, size_t len);
    int (*write)(uint32_t addr, const void *buf, size_t len);
} elib_safe_backend_t;

/* Validation callback - returns true if data is valid */
typedef bool (*elib_safe_validate_fn)(const void *data, size_t len);

/* Context structure - user statically allocates */
typedef struct {
    elib_safe_backend_t backend;
    uint32_t primary_addr;
    uint32_t backup_addr;
    uint32_t data_size;
    uint16_t magic;
    elib_safe_validate_fn validate;
    int initialized;
} elib_safe_ctx_t;

#ifdef __cplusplus
}
#endif

#endif /* ELIB_SAFE_TYPES_H */
