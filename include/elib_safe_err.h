/* elib_safe_err.h - Safe Storage Error Codes */
#ifndef ELIB_SAFE_ERR_H
#define ELIB_SAFE_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ELIB_SAFE_OK = 0,
    ELIB_SAFE_ERR_INVALID_PARAM,
    ELIB_SAFE_ERR_NOT_INITIALIZED,
    ELIB_SAFE_ERR_WRITE_FAILED,
    ELIB_SAFE_ERR_READ_FAILED,
    ELIB_SAFE_ERR_CORRUPTED,
} elib_safe_err_t;

#ifdef __cplusplus
}
#endif

#endif /* ELIB_SAFE_ERR_H */
