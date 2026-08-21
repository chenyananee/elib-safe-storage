# elib-safe-storage

轻量级嵌入式主备冗余存储模块。通过双区域备份确保数据可用性，支持自动恢复。

## 特性

- **主备冗余**：数据存储在两个独立区域
- **自动恢复**：主区损坏时自动从备区读取并恢复
- **零动态内存**：所有内存由用户静态分配
- **可插拔后端**：支持 NVS wear-leveling 或直接 flash 访问
- **用户自定义校验**：数据格式和校验逻辑由用户控制

## 快速入门

### 1. 定义数据结构

用户需要在数据结构中包含校验字段：

```c
typedef struct {
    uint16_t magic;      /* 魔数用于校验 */
    uint16_t crc16;      /* CRC 校验值 */
    uint32_t config_val;
    uint8_t  reserved[56];
} my_config_t;

#define MY_CONFIG_MAGIC  0xA5A5
```

### 2. 实现校验回调

```c
bool my_config_validate(const void *data, size_t len) {
    const my_config_t *cfg = (const my_config_t *)data;
    if (cfg->magic != MY_CONFIG_MAGIC) return false;
    return calculate_crc16(data, len) == cfg->crc16;
}
```

### 3. 创建后端接口

#### 方式一：直接使用 elib-nvs-wear-leveling（推荐）

如果已使用 elib-nvs-wear-leveling 库，可直接将其作为后端：

```c
#include "elib_nvs.h"
#include "elib_safe.h"

static elib_nvs_ctx_t g_nvs_ctx;

/* 后端读取函数 */
static int nvs_read(uint32_t addr, void *buf, size_t len) {
    return elib_nvs_read(&g_nvs_ctx, addr, buf, len);
}

/* 后端写入函数 */
static int nvs_write(uint32_t addr, const void *buf, size_t len) {
    return elib_nvs_write(&g_nvs_ctx, addr, buf, len);
}

static elib_safe_backend_t my_backend = {
    .read = nvs_read,
    .write = nvs_write,
};

/* 初始化 NVS wear-leveling */
void storage_init(void) {
    static elib_nvs_hal_driver_t nvs_driver = {
        .user_data = &my_flash_hw,
        .read = my_flash_read,
        .write = my_flash_write,
        .erase = my_flash_erase,
    };

    elib_nvs_init(
        &g_nvs_ctx,
        &nvs_driver,
        0x08000000,      /* Flash 起始地址 */
        12 * 1024,        /* 12KB 总空间 */
        4 * 1024,         /* 4KB 扇区大小 */
        64                /* 数据单元大小（>= sizeof(my_config_t)） */
    );
}
```

#### 方式二：直接 Flash 访问

直接实现读写函数：

```c
static int flash_read(uint32_t addr, void *buf, size_t len) {
    // 你的 flash 读取实现
    return flash_read_bytes(addr, buf, len);
}

static int flash_write(uint32_t addr, const void *buf, size_t len) {
    // 你的 flash 写入实现
    return flash_write_bytes(addr, buf, len);
}

static elib_safe_backend_t my_backend = {
    .read = flash_read,
    .write = flash_write,
};
```

### 4. 初始化并使用

```c
static elib_safe_ctx_t safe_ctx;

void app_init(void) {
    /* 初始化 NVS wear-leveling 后端（如果使用方式一） */
    storage_init();

    elib_safe_init(&safe_ctx, &my_backend,
                   0x0000,              /* 主区地址（NVS 逻辑地址） */
                   0x0100,              /* 备区地址（NVS 逻辑地址） */
                   sizeof(my_config_t), /* 数据大小 */
                   MY_CONFIG_MAGIC,     /* 魔数值 */
                   my_config_validate); /* 校验回调 */
}

void save_config(my_config_t *cfg) {
    cfg->magic = MY_CONFIG_MAGIC;
    cfg->crc16 = calculate_crc16(cfg, sizeof(my_config_t));
    elib_safe_write(&safe_ctx, cfg);
}

void load_config(my_config_t *cfg) {
    elib_safe_read(&safe_ctx, cfg);
}
```

## 完整示例：结合 elib-nvs-wear-leveling

```c
#include "elib_nvs.h"
#include "elib_safe.h"

/* 数据结构 */
typedef struct {
    uint16_t magic;
    uint16_t crc16;
    uint32_t config_val;
    uint8_t  reserved[56];
} my_config_t;

#define MY_CONFIG_MAGIC  0xA5A5

/* 校验回调 */
bool my_config_validate(const void *data, size_t len) {
    const my_config_t *cfg = (const my_config_t *)data;
    if (cfg->magic != MY_CONFIG_MAGIC) return false;
    return calculate_crc16(data, len) == cfg->crc16;
}

/* NVS 后端 */
static elib_nvs_ctx_t g_nvs_ctx;

static int nvs_read(uint32_t addr, void *buf, size_t len) {
    return elib_nvs_read(&g_nvs_ctx, addr, buf, len);
}

static int nvs_write(uint32_t addr, const void *buf, size_t len) {
    return elib_nvs_write(&g_nvs_ctx, addr, buf, len);
}

static elib_safe_backend_t g_backend = {
    .read = nvs_read,
    .write = nvs_write,
};

/* 安全存储上下文 */
static elib_safe_ctx_t g_safe_ctx;

/* 初始化 */
void app_init(void) {
    /* 1. 初始化 NVS wear-leveling */
    static elib_nvs_hal_driver_t nvs_driver = {
        .user_data = &my_flash_hw,
        .read = my_flash_read,
        .write = my_flash_write,
        .erase = my_flash_erase,
    };

    elib_nvs_init(
        &g_nvs_ctx,
        &nvs_driver,
        0x08000000,      /* Flash 起始地址 */
        12 * 1024,        /* 12KB 总空间 */
        4 * 1024,         /* 4KB 扇区 */
        64                /* 数据单元大小（>= sizeof(my_config_t)） */
    );

    /* 2. 初始化安全存储 */
    elib_safe_init(
        &g_safe_ctx,
        &g_backend,
        0x0000,              /* 主区逻辑地址 */
        0x0100,              /* 备区逻辑地址 */
        sizeof(my_config_t),
        MY_CONFIG_MAGIC,
        my_config_validate
    );
}

/* 使用示例 */
void save_config(my_config_t *cfg) {
    cfg->magic = MY_CONFIG_MAGIC;
    cfg->crc16 = calculate_crc16(cfg, sizeof(my_config_t));
    elib_safe_write(&g_safe_ctx, cfg);
}

void load_config(my_config_t *cfg) {
    elib_safe_read(&g_safe_ctx, cfg);
}
```

## API 参考

### elib_safe_init

初始化安全存储实例。

```c
elib_safe_err_t elib_safe_init(
    elib_safe_ctx_t *ctx,
    const elib_safe_backend_t *backend,
    uint32_t primary_addr,
    uint32_t backup_addr,
    uint32_t data_size,
    uint16_t magic,
    elib_safe_validate_fn validate
);
```

### elib_safe_write

写入数据到主区和备区。

```c
elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, const void *data);
```

### elib_safe_read

读取数据，支持自动恢复。主区损坏时从备区读取并恢复主区。

```c
elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, void *out);
```

### elib_safe_recover

强制从备区恢复数据到主区。

```c
elib_safe_err_t elib_safe_recover(elib_safe_ctx_t *ctx);
```

### elib_safe_deinit

反初始化实例。

```c
void elib_safe_deinit(elib_safe_ctx_t *ctx);
```

## 错误码

| 错误码                             | 描述       |
| ------------------------------- | -------- |
| `ELIB_SAFE_OK`                  | 成功       |
| `ELIB_SAFE_ERR_INVALID_PARAM`   | 无效参数     |
| `ELIB_SAFE_ERR_NOT_INITIALIZED` | 未初始化     |
| `ELIB_SAFE_ERR_WRITE_FAILED`    | 写入失败     |
| `ELIB_SAFE_ERR_READ_FAILED`     | 读取失败     |
| `ELIB_SAFE_ERR_CORRUPTED`       | 主区和备区均损坏 |

# 

## 许可证

MIT License
