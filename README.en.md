[简体中文](README.md) | **English**

# elib-safe-storage

A lightweight embedded **power-loss-tolerant dual-region storage library**: every record is written to the primary region first and then to the backup region, so if either half is torn the other still holds the last intact record. A read always returns the **newest intact record** you could possibly get — paired with [elib-nvs-wear-leveling](https://gitee.com/chenyananee/elib-nvs-wear-leveling) it forms a complete storage stack, and it also stands on its own on top of any direct-to-medium backend.

## Features

- **Dual-region fault tolerance**: primary and backup give you two lives; a power-loss tear destroys only one of them
- **Address agnostic**: the low-level interface only distinguishes a primary/backup region id, while physical layout, record size, and erase policy are all up to your implementation — the library makes zero assumptions about the medium (EEPROM / NOR flash / NVS regions all work)
- **Validation kept outside**: whether data is valid is decided by your callback (magic/CRC, laid out however you like); the library never interprets record contents
- **Zero dynamic memory**: you allocate the ctx; no static or heap buffer anywhere inside the library, C99
- **Repair on the side**: when the primary is torn, the read takes the backup data and writes it back to the primary from the same buffer, so the repair completes silently
- **Transparent process**: repair failure (`RECOVER_WRITE_FAILED`), single-region write failure (one return code each for primary and backup), and writes rejected by validation (`DATA_INVALID`) are all reported explicitly, nothing silent

## Quick Start

### Step 1: Define the payload and validation callback

The data structure is entirely yours to define; it is recommended to include a check field so the callback can spot a tear:

```c
typedef struct {
    uint16_t magic;      /* magic number */
    uint16_t crc16;      /* CRC or any checksum */
    uint32_t config_val;
    uint8_t  reserved[56];
} my_config_t;

#define MY_CONFIG_MAGIC 0xA5A5

uint8_t my_validate(void *ud, const uint8_t *buf) {
    const my_config_t *cfg = (const my_config_t *)buf;
    (void)ud;
    if (cfg->magic != MY_CONFIG_MAGIC) return 0;
    return (uint8_t)(my_crc16(buf, sizeof(my_config_t)) == cfg->crc16);
}
```

Callback contract: return 1 only when the buffer holds **one complete, usable** record, otherwise 0. The validation algorithm is your call.

### Step 2: Implement the primary/backup read/write

The interface only gives you a **primary/backup id**; physical addresses are your own private business (stuff them into `user_data`, and a layout change touches data only, never code):

```c
typedef enum {
    ELIB_SAFE_REGION_PRIMARY = 0,   /* primary region */
    ELIB_SAFE_REGION_BACKUP  = 1    /* backup region */
} elib_safe_region_t;

typedef struct {
    uint32_t addr[2];        /* your physical address table, example only */
} my_media_t;

static int my_read(void *ud, elib_safe_region_t region, uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    /* Read one record out of m->addr[region] (sizeof(my_config_t) bytes);
     * return non-zero on failure, 0 on success */
    return flash_read_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static int my_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    /* Write one record to m->addr[region];
     * the erase-before-write that NOR flash needs is handled right here — the library only calls you */
    return flash_write_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static const elib_safe_ops_t my_ops = {
    .user_data = &g_my_media,      /* the library only forwards it, never interprets it */
    .read      = my_read,
    .write     = my_write,
    .validate  = my_validate,
};
```

### Step 3: Initialize, read, write

```c
static elib_safe_ctx_t g_safe_ctx;    /* static allocation by the user */

void app_init(void) {
    elib_safe_init(&g_safe_ctx, &my_ops);
}

/* Save: the entry validates your record first (a failure returns DATA_INVALID and nothing is written), then primary, then backup */
if (elib_safe_write(&g_safe_ctx, (const uint8_t *)cfg) == ELIB_SAFE_OK) { ... }

/* Read the newest intact record back */
if (elib_safe_read(&g_safe_ctx, (uint8_t *)cfg) == ELIB_SAFE_OK) { ... }
```

## Read Rules (primary broken, lean on backup, repair in passing)

```
read primary -> validates -> OK
primary read failed (backend non-zero) or content fails validation <- continue
    read backup -> validates -> write it back to the primary from the same buffer:
        write-back succeeds -> OK (repair completed silently)
        write-back fails -> RECOVER_WRITE_FAILED (data already handed to you, the primary is still broken)
    backup fails too:
        any backend read non-zero -> ERR_IO (medium failure reported first)
        both regions read back but neither content validates (including a brand-new 0xFF medium) -> ERR_CORRUPTED
```

## Write Rules (validate on entry + primary then backup + four-state return)

```
user validation fails -> DATA_INVALID, nothing written
write the primary first, then the backup whether the primary worked or not, and return the 2x2 truth table:

primary OK   backup OK   -> ELIB_SAFE_OK
primary OK   backup FAIL -> ELIB_SAFE_ERR_BACKUP_WRITE_FAILED (data is safely in the primary)
primary FAIL backup OK   -> ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED (data is safely in the backup)
primary FAIL backup FAIL -> ELIB_SAFE_ERR_WRITE_FAILED (this write did not happen at all)
```

## Power-Loss Timing (why a torn half costs nothing)

| Power-loss moment | Primary | Backup | read result |
|---|---|---|---|
| Halfway through writing the primary | torn | previous record intact | returns the backup data (semantics: **last intact record**, legal) |
| Primary written, backup not yet | new record intact | previous record intact | returns the new primary record |
| Halfway through writing the backup | new record intact | torn | returns the new primary record |

At every instant at least one region is standing, so a read may lag one beat but is **always intact and trustworthy**.

## Return Codes

| Code | Meaning |
|---|---|
| `ELIB_SAFE_OK` | Success (write: both regions landed; read: primary good, or backup good with the repair done) |
| `ELIB_SAFE_ERR_INVALID_PARAM` | Illegal init parameter / NULL passed to read/write |
| `ELIB_SAFE_ERR_NOT_INITIALIZED` | An API was called without init |
| `ELIB_SAFE_ERR_IO` | read: the backend read was non-zero and no intact data could be delivered |
| `ELIB_SAFE_ERR_DATA_INVALID` | write: entry validation rejected it, nothing written |
| `ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED` | write: the primary failed, the backup is safe |
| `ELIB_SAFE_ERR_BACKUP_WRITE_FAILED` | write: the backup failed, the primary is safe |
| `ELIB_SAFE_ERR_WRITE_FAILED` | write: both regions lost |
| `ELIB_SAFE_ERR_RECOVER_WRITE_FAILED` | read: the backup data is valid but repairing the primary failed |
| `ELIB_SAFE_ERR_CORRUPTED` | read: both regions read back but neither content is valid |

## Layer 1: Sitting on elib-nvs-wear-leveling (recommended)

NVS v2 is a wear-leveling library of "fixed-length append writes, newest record only"; give it two **independent NVS regions** to serve as primary and backup and safe-storage gains both integrity and cyclic wear leveling:

```c
#include "elib_nvs.h"
#include "elib_safe.h"

static elib_nvs_ctx_t nvs_primary;   /* primary region: a management area of its own */
static elib_nvs_ctx_t nvs_backup;    /* backup region: a management area of its own */

typedef struct {
    elib_nvs_ctx_t *primary;
    elib_nvs_ctx_t *backup;
} nvs_pair_t;                        /* this is the user_data */

static nvs_pair_t pair = { &nvs_primary, &nvs_backup };

static int nvs_pair_read(void *ud, elib_safe_region_t region, uint8_t *buf) {
    const nvs_pair_t *p = (const nvs_pair_t *)ud;
    elib_nvs_ctx_t *nvs = (region == ELIB_SAFE_REGION_PRIMARY) ? p->primary : p->backup;
    /* the NVS fixed-length cell is >= your record size; each NVS region manages itself */
    return elib_nvs_read(nvs, buf) == ELIB_NVS_OK ? 0 : -1;
}

static int nvs_pair_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const nvs_pair_t *p = (const nvs_pair_t *)ud;
    elib_nvs_ctx_t *nvs = (region == ELIB_SAFE_REGION_PRIMARY) ? p->primary : p->backup;
    /* an NVS write appends a new record: erasing is NVS's lazy business, no pre-erase needed */
    return elib_nvs_write(nvs, buf) == ELIB_NVS_OK ? 0 : -1;
}

static const elib_safe_ops_t nvs_ops = {
    .user_data = &pair, nvs_pair_read, nvs_pair_write, my_validate,
};
```

Wear cost: every `elib_safe_write` appends one record to each of the two regions, so each region burns one cell of life; the averaging works out to the NVS lifetime model split evenly across two regions, each wearing independently.

## Layer 2: Talking to the Medium Directly

This is yours to handle: record size / address layout / erase-before-write for NOR flash / overwrite page latency for EEPROM. The library holds none of those assumptions.

## Memory Usage

- **RAM**: only `elib_safe_ctx_t` (one ops copy + 1 bit of state, a few dozen bytes)
- **Note**: during a read the library first fills your buf and then (if needed) writes it back verbatim to repair the primary — **no** extra temporary buffer is required, which is the dividend of v2 structurally eliminating the v1 VLA defect
- **Stack**: no recursion, no large buffers

## Non-Goals

- It does not do CRC/magic itself (the payload and the validation algorithm belong entirely to your validate callback)
- It does not tell new records from old (no seq; "old but intact" is a legal read result)
- It does not provide an erase interface (the policy lives inside your implementation)
- It does not do multiple records/history (the model is one record + two copies on the medium)
