**简体中文** | [English](README.en.md)

# elib-safe-storage

轻量级嵌入式**断电容错双区存储库**：每条记录先写主区、再写备区，任何一半撕裂，
另一半都保持着最后一条完好数据。

两种后端、两种模式：

- **直连介质（`ELIB_SAFE_MODE_DIRECT`）**：写是覆盖，撕裂后记录直接无效，主区要么是最新、
  要么不可用，"主区优先"永远正确——与 v2 完全一致。
- **垫在磨损均衡层上（`ELIB_SAFE_MODE_WEAR_LEVELED`）**：均衡层的"最新"可能因 flash 页丢失
  **回退**到一条更旧但 CRC 合法的记录。此模式把每条记录的**前 4 字节**交给库放版本号，
  读时两区都读、按版本取较新，主区落后时用备区修复。用户不读、不写、不解释这 4 字节。

## 特性

- **双区容错**：主备两条命，掉电撕裂只毁一份
- **地址无关**：底层接口只区分主/备区标识，物理布局、记录大小、擦除策略全在用户实现内自理
- **校验外置**：数据是否有效由用户回调判定（magic/CRC 随你摆），库只碰版本槽 4 字节
- **两种语义显式选择**：直连用 DIRECT（零版本开销），垫 NVS 用 WEAR_LEVELED（自动选新）
- **版本库自管**：WEAR_LEVELED 下库自动递增、自动比较，固定小端编码，无大小端问题
- **零动态内存**：用户分配 ctx，库内无任何 static/堆缓冲，C99
- **顺手修复**：主区落后或撕裂时读备区，同一个缓冲回写主区，修复静默完成
- **过程透明**：修复失败、单区写失败、写入被校验拒绝、版本未播种全部显式上报，零静默

## 快速上手

### 第 1 步：定义载荷与校验回调

DIRECT 模式记录布局全自由；WEAR_LEVELED 模式**必须在前 4 字节留版本槽**，且校验要跳过它：

```c
/* WEAR_LEVELED：前 4 字节归库，用户从第 4 字节起定义载荷 */
typedef struct {
    uint8_t  version[4];  /* 库独占：不要读、不要写、不参与校验 */
    uint16_t magic;       /* 0xA5A5 */
    uint16_t crc16;
    uint32_t config_val;
    uint8_t  reserved[52];
} my_config_t;            /* 64 字节 */

uint8_t my_validate(void *ud, const uint8_t *buf) {
    const my_config_t *cfg = (const my_config_t *)buf;
    (void)ud;
    if (cfg->magic != 0xA5A5) return 0;
    /* 注意：CRC 从 version 之后算起，前 4 字节不参与 */
    return (uint8_t)(my_crc16(buf + 4, sizeof(my_config_t) - 4) == cfg->crc16);
}
```

```c
/* DIRECT：前 4 字节没有版本槽，整个结构都归你 */
typedef struct {
    uint16_t magic;
    uint16_t crc16;
    uint32_t config_val;
    uint8_t  reserved[56];
} my_direct_config_t;
```

回调约定：缓冲里是**一条完整、可用**的记录才返回 1，否则 0。校验算法你说了算。

### 第 2 步：实现主备读/写

接口只给**主备标识**，物理地址是你的私事（塞进 `user_data`）：

```c
typedef enum {
    ELIB_SAFE_REGION_PRIMARY = 0,   /* 主区 */
    ELIB_SAFE_REGION_BACKUP  = 1    /* 备区 */
} elib_safe_region_t;

typedef struct {
    uint32_t addr[2];        /* 你的物理地址表，仅示例 */
} my_media_t;

static int my_read(void *ud, elib_safe_region_t region, uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    return flash_read_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static int my_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    /* 先擦后写等介质细节在这里自理，库只管调用 */
    return flash_write_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static const elib_safe_ops_t my_ops = {
    .user_data = &g_my_media,      /* 库只转发，不解读 */
    .read      = my_read,
    .write     = my_write,
    .validate  = my_validate,
};
```

### 第 3 步：初始化、读、写

```c
static elib_safe_ctx_t g_safe_ctx;    /* 用户静态分配 */

void app_init(void) {
    /* 直连介质用 DIRECT；垫在 elib-nvs 上用 WEAR_LEVELED */
    elib_safe_init(&g_safe_ctx, &my_ops, ELIB_SAFE_MODE_WEAR_LEVELED);
}

/* 上电先读：不仅取回最新记录，还会播种版本计数器（WEAR_LEVELED） */
if (elib_safe_read(&g_safe_ctx, (uint8_t *)cfg) == ELIB_SAFE_OK) { ... }

/* 保存：入口先校验（不过返回 DATA_INVALID，不落笔），然后先主后备 */
if (elib_safe_write(&g_safe_ctx, (uint8_t *)cfg) == ELIB_SAFE_OK) { ... }
```

## 读规则

### DIRECT（主破备倾、顺手修复）

```
读主区 → 校验过 → OK
主区没读上来（底层非 0）或内容校验不过 → 读备区：
    备区校验过 → 用同一缓冲回写主区：
        回写成功 → OK（修复静默完成）
        回写失败 → RECOVER_WRITE_FAILED（数据已给你，主区还坏着）
    备区也不行：
        任一次底层读非 0 → ERR_IO
        两区都读上来了但内容都校验不过 → ERR_CORRUPTED
```

主区通过时**不读备区**，开销与 v2 一致。

### WEAR_LEVELED（两区都读、按版本取新）

```
读主区 → 读到 && 校验过 && 版本有效（非 0、非全 F）→ 记 p_ver
读备区 → 读到 && 校验过 && 版本有效           → 记 b_ver

两区都有效：
    p_ver 更新 → 重读主区入 buf                                  → OK
    b_ver 更新 → buf 已是备区，回写主区修复                       → OK / RECOVER_WRITE_FAILED
    版本相等   → 版本唯一 ⟹ 同一条，直接用                        → OK
仅主区有效 → 重读主区入 buf                                      → OK
仅备区有效 → buf 已是备区，回写主区修复                           → OK / RECOVER_WRITE_FAILED
都无效     → 任一次底层读非 0 → IO，否则 CORRUPTED
```

只有"主区严格更新 / 仅主区有效"两条降级路径会多一次主区读；正常双写后两区等版本，不重读。

## 写规则

入口校验不过 → `DATA_INVALID`，不落笔。然后先写主区、无论成败接着写备区：

| 主区 | 备区 | 返回 |
|---|---|---|
| ✅ | ✅ | `ELIB_SAFE_OK` |
| ✅ | ❌ | `ELIB_SAFE_ERR_BACKUP_WRITE_FAILED`（数据已安全在主区） |
| ❌ | ✅ | `ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED`（数据已安全在备区） |
| ❌ | ❌ | `ELIB_SAFE_ERR_WRITE_FAILED`（本次写入等于没发生） |

WEAR_LEVELED 下，写之前库会把**前 4 字节**改成递增后的版本（`buf` 由 const 改为可变）。
若还没读过（版本未播种）→ `ELIB_SAFE_ERR_NO_VERSION`。

## 版本槽规则（WEAR_LEVELED）

- 记录**前 4 字节**归库：小端 32 位，有效区间 `[1, 0xFFFFFFFE]`；
  `0x00000000` 与 `0xFFFFFFFF` 是无效值（对应全 0 / 全 F 擦除态）。
- 写时自动 `+1`，跳过两个无效值（`0xFFFFFFFE` → `1`）。
- 读时两区版本用回绕安全比较 `(int32_t)(a-b) > 0`，存活记录数远小于 2^31。
- 用户**不要读、不要写、不要让校验覆盖这 4 字节**（库会改它）。
- 计数器由 `elib_safe_read` 播种：上电先读，再写；否则 `NO_VERSION`。

## 断电时序（WEAR_LEVELED）

| 掉电时刻 | 主区 | 备区 | 上电 read |
|---|---|---|---|
| 双写完成 | V | V | 返回 V（等值） |
| 主区写一半 | 撕裂/旧 | V | 返回备区 V（并修复主区） |
| 备区写一半 | V | 撕裂/旧 | 返回主区 V（重读一次） |
| **主区整页丢失（均衡层回退）** | V−k（旧但合法） | V | **返回备区 V（并修复主区）** |
| 版本槽撕裂成 0 / 全 F | 无效版本 | V | 返回备区 V |

任何时刻读到的都是两区中版本更新的一条合法记录。

## 返回码

| 码 | 含义 |
|---|---|
| `ELIB_SAFE_OK` | 成功（write 两区全落；read 取到有效记录） |
| `ELIB_SAFE_ERR_INVALID_PARAM` | init 参数非法（含 mode 非法）/ read/write 传了 NULL |
| `ELIB_SAFE_ERR_NOT_INITIALIZED` | 未 init 就调 API |
| `ELIB_SAFE_ERR_IO` | read：底层读非 0 且无完好数据可交付 |
| `ELIB_SAFE_ERR_DATA_INVALID` | write：入口校验拒绝，未落笔 |
| `ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED` | write：主区失败、备区已安全 |
| `ELIB_SAFE_ERR_BACKUP_WRITE_FAILED` | write：备区失败、主区已安全 |
| `ELIB_SAFE_ERR_WRITE_FAILED` | write：两区全失守 |
| `ELIB_SAFE_ERR_RECOVER_WRITE_FAILED` | read：备区数据有效但主区修复写失败 |
| `ELIB_SAFE_ERR_CORRUPTED` | read：两区读得动但内容都无效 |
| `ELIB_SAFE_ERR_NO_VERSION` | write：WEAR_LEVELED 下还没 read 播种版本 |

## 分层一：垫在 elib-nvs-wear-leveling 上（WEAR_LEVELED）

给 NVS 两个**独立管理区**分别当主备，safe-storage 就用 `WEAR_LEVELED` 获得
"完整 + 循环磨损 + 取最新"三重保障。NVS v3 只做磨损均衡（`read`/`write` 两 op，无 erase），
正好当这里的字节可写后端：

```c
#include "elib_nvs.h"
#include "elib_safe.h"

static elib_nvs_ctx_t nvs_primary;   /* 主域：独立管理区 */
static elib_nvs_ctx_t nvs_backup;    /* 备域：独立管理区 */

typedef struct {
    elib_nvs_ctx_t *primary;
    elib_nvs_ctx_t *backup;
} nvs_pair_t;                        /* 这就是 user_data */

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
    .validate  = my_validate,        /* 注意：跳过前 4 字节版本槽 */
};

/* NVS 的 user_size 必须等于 safe-storage 的记录大小 */
elib_safe_init(&g_safe_ctx, &nvs_ops, ELIB_SAFE_MODE_WEAR_LEVELED);
```

**为什么必须是 WEAR_LEVELED**：NVS 回绕时会覆盖旧槽，flash 页丢失后它返回的是更旧但 CRC
合法的记录；DIRECT 的"主区优先"会把这种旧数据当最新返回，造成静默回滚。

## 分层二：直接对接介质（DIRECT）

记录固定写在主/备各自地址上，撕裂即无效，用 `ELIB_SAFE_MODE_DIRECT` 即可，零版本开销：

```c
elib_safe_init(&g_safe_ctx, &my_ops, ELIB_SAFE_MODE_DIRECT);
```

需要你自己处理：记录大小 / 地址布局 / NOR flash 的先擦后写 / EEPROM 的覆写页延迟。

## 内存占用

- **RAM**：仅 `elib_safe_ctx_t`（ops 拷贝 + 版本计数器 + 几个标志位）
- **缓冲**：调用方只需一块记录缓冲；read 复用同一 `buf` 完成比较与修复，无需第二缓冲
- **栈**：无递归、无大缓冲

## 非目标

- 不做 CRC/magic 本身（载荷与校验算法全归用户 validate 回调）
- DIRECT 下不分新旧（直连介质不存在"旧但合法"的回退）
- 不做擦除接口（策略在用户实现内）
- 不做多记录/历史（模型是单记录 + 双份落盘）
