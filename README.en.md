[简体中文](README.md) | **English**

# elib-safe-storage

A lightweight embedded **power-loss tolerant dual-region storage library**: every record is
written to the primary region first and the backup region second, so a torn write in one
half never destroys the last known-good copy.

Two backends, two modes:

- **Direct medium (`ELIB_SAFE_MODE_DIRECT`)**: writes overwrite in place, so a torn write
  leaves the record invalid. The primary is either the latest or unusable, and "primary
  first" is always correct -- exactly the v2 behaviour.
- **On top of a wear-leveling layer (`ELIB_SAFE_MODE_WEAR_LEVELED`)**: the layer's "latest"
  can **regress** to an older but CRC-valid record after a flash page loss. This mode hands
  the **first 4 bytes** of every record to the library as a version slot; read loads both
  regions and returns the newer one, repairing the primary when it lags. The user never
  reads, writes or interprets those 4 bytes.

## Features

- **Dual-region tolerance**: primary and backup; a torn write destroys at most one copy
- **Address agnostic**: the backend only sees a primary/backup tag; physical layout, record
  size and erase strategy stay in the user implementation
- **Externalized validation**: the user callback decides record validity (magic/CRC as you
  like); the library only touches the 4-byte version slot
- **Explicit mode choice**: DIRECT for a raw medium (zero version overhead), WEAR_LEVELED
  when stacking on elib-nvs (automatic newest selection)
- **Library-owned version**: in WEAR_LEVELED the library increments and compares it itself,
  with a fixed little-endian encoding and no endianness concerns
- **Zero dynamic memory**: the user allocates the context; C99, no static/heap buffer
- **Silent repair**: when the primary lags or is torn, the backup is read and written back
  into the primary through the same buffer
- **Transparent outcomes**: failed repair, single-region write failures, rejected writes and
  an unseeded version are all reported, nothing is silent

## Quick Start

### Step 1: Define the payload and the validate callback

DIRECT lets you lay out the record freely; WEAR_LEVELED **requires a 4-byte version slot at
the start**, and validation must skip it:

```c
/* WEAR_LEVELED: the first 4 bytes belong to the library, the payload starts at 4 */
typedef struct {
    uint8_t  version[4];  /* library owned: never read, write or validate it */
    uint16_t magic;       /* 0xA5A5 */
    uint16_t crc16;
    uint32_t config_val;
    uint8_t  reserved[52];
} my_config_t;            /* 64 bytes */

uint8_t my_validate(void *ud, const uint8_t *buf) {
    const my_config_t *cfg = (const my_config_t *)buf;
    (void)ud;
    if (cfg->magic != 0xA5A5) return 0;
    /* the CRC starts after the version slot; the first 4 bytes are excluded */
    return (uint8_t)(my_crc16(buf + 4, sizeof(my_config_t) - 4) == cfg->crc16);
}
```

```c
/* DIRECT: no version slot, the whole structure is yours */
typedef struct {
    uint16_t magic;
    uint16_t crc16;
    uint32_t config_val;
    uint8_t  reserved[56];
} my_direct_config_t;
```

The callback contract: return 1 only when the buffer holds one complete, usable record.

### Step 2: Implement the primary/backup read and write

The API only gives you a **region tag**; physical addressing is your business (put it in
`user_data`):

```c
typedef enum {
    ELIB_SAFE_REGION_PRIMARY = 0,   /* primary */
    ELIB_SAFE_REGION_BACKUP  = 1    /* backup */
} elib_safe_region_t;

typedef struct {
    uint32_t addr[2];        /* your physical address table, example only */
} my_media_t;

static int my_read(void *ud, elib_safe_region_t region, uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    return flash_read_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static int my_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    /* erase-before-write and other medium details live here; the library just calls */
    return flash_write_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static const elib_safe_ops_t my_ops = {
    .user_data = &g_my_media,      /* forwarded, never interpreted */
    .read      = my_read,
    .write     = my_write,
    .validate  = my_validate,
};
```

### Step 3: Initialize, read and write

```c
static elib_safe_ctx_t g_safe_ctx;    /* statically allocated by the user */

void app_init(void) {
    /* DIRECT for a raw medium; WEAR_LEVELED when stacking on elib-nvs */
    elib_safe_init(&g_safe_ctx, &my_ops, ELIB_SAFE_MODE_WEAR_LEVELED);
}

/* read first at boot: it also seeds the version counter (WEAR_LEVELED) */
if (elib_safe_read(&g_safe_ctx, (uint8_t *)cfg) == ELIB_SAFE_OK) { ... }

/* save: entry validation first (DATA_INVALID and nothing is written), then primary -> backup */
if (elib_safe_write(&g_safe_ctx, (uint8_t *)cfg) == ELIB_SAFE_OK) { ... }
```

## Read Rules

### DIRECT (primary first, fall back and repair)

```
read primary -> valid -> OK
primary unreadable (backend non-zero) or invalid -> read backup:
    backup valid -> write it back into the primary through the same buffer:
        write ok  -> OK (repair is silent)
        write bad -> RECOVER_WRITE_FAILED (data delivered, primary still broken)
    backup also bad:
        any backend read non-zero -> ERR_IO
        both read fine but both invalid -> ERR_CORRUPTED
```

A standing primary means the backup is **not** read at all, so the cost matches v2.

### WEAR_LEVELED (read both, pick the newer version)

```
read primary -> readable && valid && version valid (not 0, not all-F) -> p_ver
read backup  -> readable && valid && version valid                    -> b_ver

both valid:
    p_ver newer -> re-read the primary into buf                         -> OK
    b_ver newer -> buf already holds the backup, repair the primary     -> OK / RECOVER_WRITE_FAILED
    equal       -> versions are unique, same record                     -> OK
primary only    -> re-read the primary into buf                         -> OK
backup only     -> buf already holds the backup, repair the primary     -> OK / RECOVER_WRITE_FAILED
neither valid   -> any backend read non-zero -> IO, otherwise CORRUPTED
```

Only the "primary strictly newer / primary only" degraded paths re-read the primary; after a
normal dual write the versions are equal and nothing is re-read.

## Write Rules

A rejected entry validation returns `DATA_INVALID` and writes nothing. Then the primary is
written first and the backup follows regardless of the outcome:

| Primary | Backup | Return |
|---|---|---|
| ok | ok | `ELIB_SAFE_OK` |
| ok | fail | `ELIB_SAFE_ERR_BACKUP_WRITE_FAILED` (data is safe in the primary) |
| fail | ok | `ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED` (data is safe in the backup) |
| fail | fail | `ELIB_SAFE_ERR_WRITE_FAILED` (the write did not happen) |

In WEAR_LEVELED the library rewrites the **first 4 bytes** with the incremented version
before writing (`buf` changed from const to mutable). If nothing was read yet (the version is
unseeded) it returns `ELIB_SAFE_ERR_NO_VERSION`.

## Version Slot Rules (WEAR_LEVELED)

- The **first 4 bytes** belong to the library: little-endian 32-bit, valid range
  `[1, 0xFFFFFFFE]`; `0x00000000` and `0xFFFFFFFF` are invalid (they are the all-0 / all-1
  erased states).
- Write increments it by 1, skipping both invalid values (`0xFFFFFFFE` -> `1`).
- Read compares the two regions with a wrap-safe `(int32_t)(a-b) > 0`.
- Never read, write or validate those 4 bytes; the library changes them.
- The counter is seeded by `elib_safe_read`: read at boot, then write; otherwise `NO_VERSION`.

## Power-Loss Timing (WEAR_LEVELED)

| Power cut | Primary | Backup | Read after boot |
|---|---|---|---|
| both writes done | V | V | returns V (equal) |
| primary half written | torn/old | V | returns backup V (and repairs the primary) |
| backup half written | V | torn/old | returns primary V (one re-read) |
| **primary page lost (wear-leveling regression)** | V-k (old but valid) | V | **returns backup V (and repairs the primary)** |
| version slot torn to 0 / all-F | invalid version | V | returns backup V |

At any moment the read returns the newer valid record of the two regions.

## Return Codes

| Code | Meaning |
|---|---|
| `ELIB_SAFE_OK` | success (write landed in both; read found a valid record) |
| `ELIB_SAFE_ERR_INVALID_PARAM` | bad init arguments (including a bad mode) / NULL passed to read or write |
| `ELIB_SAFE_ERR_NOT_INITIALIZED` | API called before init |
| `ELIB_SAFE_ERR_IO` | read: backend read failed with no valid data to deliver |
| `ELIB_SAFE_ERR_DATA_INVALID` | write: entry validation rejected the record, nothing written |
| `ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED` | write: primary failed, backup is safe |
| `ELIB_SAFE_ERR_BACKUP_WRITE_FAILED` | write: backup failed, primary is safe |
| `ELIB_SAFE_ERR_WRITE_FAILED` | write: both regions failed |
| `ELIB_SAFE_ERR_RECOVER_WRITE_FAILED` | read: backup data is valid but the primary repair failed |
| `ELIB_SAFE_ERR_CORRUPTED` | read: both regions read fine but both are invalid |
| `ELIB_SAFE_ERR_NO_VERSION` | write: WEAR_LEVELED with no read yet to seed the version |

## Layering 1: On Top of elib-nvs-wear-leveling (WEAR_LEVELED)

Give the NVS two **independent managed regions** as primary and backup, and safe-storage uses
`WEAR_LEVELED` to get integrity, cyclic wear leveling and newest selection. NVS v3 is pure
wear leveling (`read`/`write`, no erase), which is exactly the byte-writable backend needed
here:

```c
#include "elib_nvs.h"
#include "elib_safe.h"

static elib_nvs_ctx_t nvs_primary;   /* primary: independent managed region */
static elib_nvs_ctx_t nvs_backup;    /* backup: independent managed region */

typedef struct {
    elib_nvs_ctx_t *primary;
    elib_nvs_ctx_t *backup;
} nvs_pair_t;                        /* this is user_data */

static nvs_pair_t pair = { &nvs_primary, &nvs_backup };

static int nvs_pair_read(void *ud, elib_safe_region_t region, uint8_t *buf) {
    const nvs_pair_t *p = (const nvs_pair_t *)ud;
    elib_nvs_ctx_t *nvs = (region == ELIB_SAFE_REGION_PRIMARY) ? p->primary : p->backup;
    return elib_nvs_read(nvs, buf) == ELIB_NVS_OK ? 0 : -1;
}

static int nvs_pair_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const nvs_pair_t *p = (const nvs_pair_t *)ud;
    elib_nvs_ctx_t *nvs = (region == ELIB_SAFE_REGION_PRIMARY) ? p->primary : p->backup;
    return elib_nvs_write(nvs, buf) == ELIB_NVS_OK ? 0 : -1;
}

static const elib_safe_ops_t nvs_ops = {
    .user_data = &pair,
    .read      = nvs_pair_read,
    .write     = nvs_pair_write,
    .validate  = my_validate,        /* must skip the 4-byte version slot */
};

/* the NVS user_size must equal the safe-storage record size */
elib_safe_init(&g_safe_ctx, &nvs_ops, ELIB_SAFE_MODE_WEAR_LEVELED);
```

**Why WEAR_LEVELED is required**: when the NVS wraps it overwrites old slots, and after a
flash page loss it returns an older but CRC-valid record. The DIRECT "primary first"
behaviour would hand that stale record back as the latest -- a silent rollback.

## Layering 2: Directly on the Medium (DIRECT)

The record lives at a fixed primary and backup address and a torn write is simply invalid, so
`ELIB_SAFE_MODE_DIRECT` is enough and costs no version bytes:

```c
elib_safe_init(&g_safe_ctx, &my_ops, ELIB_SAFE_MODE_DIRECT);
```

You handle record size, address layout, flash erase-before-write and EEPROM write latency.

## Memory Usage

- **RAM**: only `elib_safe_ctx_t` (an ops copy + a version counter + a few flags)
- **Buffers**: the caller needs one record buffer; read compares and repairs through the same
  `buf`, no second buffer is required
- **Stack**: no recursion, no large buffers

## Non-Goals

- No CRC/magic itself (payload and validation belong to the user callback)
- No old/new distinction in DIRECT (a raw medium has no "old but valid" regression)
- No erase interface (the policy lives in the user implementation)
- No multi-record/history (single record stored twice)
