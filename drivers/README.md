# Backend Driver Templates

This document provides templates for implementing the `elib_safe_backend_t` interface.

## Interface Definition

```c
typedef struct {
    int (*read)(uint32_t addr, void *buf, size_t len);
    int (*write)(uint32_t addr, const void *buf, size_t len);
} elib_safe_backend_t;
```

## Return Values

Both functions should return:
- `0` on success
- Non-zero on failure

## Template 1: Direct Flash Access

```c
#include "elib_safe.h"

/* Platform-specific flash functions (user implements) */
extern int flash_read_bytes(uint32_t addr, void *buf, size_t len);
extern int flash_write_bytes(uint32_t addr, const void *buf, size_t len);

static int flash_backend_read(uint32_t addr, void *buf, size_t len) {
    return flash_read_bytes(addr, buf, len);
}

static int flash_backend_write(uint32_t addr, const void *buf, size_t len) {
    return flash_write_bytes(addr, buf, len);
}

elib_safe_backend_t flash_backend = {
    .read = flash_backend_read,
    .write = flash_backend_write,
};
```

## Template 2: NVS Wear-Leveling Backend

```c
#include "elib_nvs.h"
#include "elib_safe.h"

static elib_nvs_ctx_t nvs_ctx;

static int nvs_backend_read(uint32_t addr, void *buf, size_t len) {
    return elib_nvs_read(&nvs_ctx, addr, buf, len);
}

static int nvs_backend_write(uint32_t addr, const void *buf, size_t len) {
    return elib_nvs_write(&nvs_ctx, addr, buf, len);
}

elib_safe_backend_t nvs_backend = {
    .read = nvs_backend_read,
    .write = nvs_backend_write,
};

/* Initialize NVS first, then safe-storage */
void setup_safe_storage(void) {
    /* Initialize NVS with your hardware driver */
    elib_nvs_init(&nvs_ctx, &nvs_driver,
                  start_addr, total_size, block_size, data_unit_size);

    /* Initialize safe-storage with NVS backend */
    elib_safe_init(&safe_ctx, &nvs_backend,
                   primary_offset, backup_offset,
                   data_size, magic, validate_fn);
}
```

## Template 3: EEPROM Backend

```c
#include "elib_safe.h"

/* I2C EEPROM functions (user implements) */
extern int eeprom_read(uint8_t dev_addr, uint16_t mem_addr, uint8_t *buf, uint16_t len);
extern int eeprom_write(uint8_t dev_addr, uint16_t mem_addr, const uint8_t *buf, uint16_t len);

#define EEPROM_DEV_ADDR  0x50

static int eeprom_backend_read(uint32_t addr, void *buf, size_t len) {
    return eeprom_read(EEPROM_DEV_ADDR, (uint16_t)addr, buf, (uint16_t)len);
}

static int eeprom_backend_write(uint32_t addr, const void *buf, size_t len) {
    return eeprom_write(EEPROM_DEV_ADDR, (uint16_t)addr, buf, (uint16_t)len);
}

elib_safe_backend_t eeprom_backend = {
    .read = eeprom_backend_read,
    .write = eeprom_backend_write,
};
```

## Notes

- Addresses are passed through directly - the backend can interpret them as physical addresses, logical offsets, or any other scheme
- The backend does not manage memory allocation - all buffers are provided by the caller
- Error handling is minimal by design - return non-zero on any failure
