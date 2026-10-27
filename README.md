# elib-safe-storage

轻量级嵌入式**断电容错双区存储库**：每条记录先写主区、再写备区，任何一半撕裂，
另一半都保持着最后一条完好数据。读完永远是你能拿到的**最新完好记录**——
它配在 [elib-nvs-wear-leveling](https://gitee.com/chenyananee/elib-nvs-wear-leveling)
上面就是完整存储栈，垫在任何介质直连后端上也自成立。

## 特性

- **双区容错**：主备两条命，掉电撕裂只毁一份
- **地址无关**：底层接口只区分主/备区标识，物理布局、记录大小、擦除策略全在用户实现内自理——库对介质零假设（EEPROM / NOR flash / NVS 域皆可垫）
- **校验外置**：数据是否有效由用户回调判定（magic/CRC 随你摆），库从不解读记录内容
- **零动态内存**：用户分配 ctx，库内无任何 static/堆缓冲，C99
- **顺手修复**：主区撕裂时读备区数据，同一个缓冲回写主区，修复静默完成
- **过程透明**：修复失败（`RECOVER_WRITE_FAILED`）、单区写失败（主/备各一个返回码）、写入被校验拒绝（`DATA_INVALID`）全部显式上报，零静默

## 快速上手

### 第 1 步：定义载荷与校验回调

数据结构完全由用户定义，建议塞一个校验字段让回调判裂：

```c
typedef struct {
    uint16_t magic;      /* 魔数 */
    uint16_t crc16;      /* CRC 或任意校验和 */
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

回调约定：缓冲里是**一条完整、可用**的记录才返回 1，否则 0。校验算法你说了算。

### 第 2 步：实现主备读/写

接口只给**主备标识**，物理地址是你的私事（塞进 `user_data`，换布局只换数据不换代码）：

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
    /* 从 m->addr[region] 读出一条记录（大小 sizeof(my_config_t)）；
     * 失败返回非 0，成功返回 0 */
    return flash_read_bytes(m->addr[region], buf, sizeof(my_config_t)) ? -1 : 0;
}

static int my_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const my_media_t *m = (const my_media_t *)ud;
    /* 往 m->addr[region] 写一条记录；
     * NOR flash 需要的"先擦后写"在这里自行处理——库只管调用 */
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
    elib_safe_init(&g_safe_ctx, &my_ops);
}

/* 保存：入口先校验你的记录（不过会返回 DATA_INVALID，不落笔），然后先主后备 */
if (elib_safe_write(&g_safe_ctx, (const uint8_t *)cfg) == ELIB_SAFE_OK) { ... }

/* 读出最新完好记录 */
if (elib_safe_read(&g_safe_ctx, (uint8_t *)cfg) == ELIB_SAFE_OK) { ... }
```

## 读规则（主破备倾、顺手修复）

```
读主区 → 校验过 → OK
主区没读上来（底层非 0）或内容校验不过 ← 继续
    读备区 → 校验过 → 用同一缓冲回写主区：
        回写成功 → OK（修复静默完成）
        回写失败 → RECOVER_WRITE_FAILED（数据已给你，主区还坏着）
    备区也不行：
        任一次底层读非 0 → ERR_IO（介质故障优先上报）
        两区都读上来了但内容都校验不过（含全新 0xFF 介质）→ ERR_CORRUPTED
```

## 写规则（入口校验 + 先主后备 + 四态返回）

```
用户校验不过 → DATA_INVALID，不落笔
先写主区，无论成败接着写备区，返回覆盖 2×2 真值表：

主区 ✅ 备区 ✅ → ELIB_SAFE_OK
主区 ✅ 备区 ❌ → ELIB_SAFE_ERR_BACKUP_WRITE_FAILED（数据已安全在主区）
主区 ❌ 备区 ✅ → ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED（数据已安全在备区）
主区 ❌ 备区 ❌ → ELIB_SAFE_ERR_WRITE_FAILED（本次写入等于没发生）
```

## 断电时序（为什么"任何一半撕裂都无损失"）

| 掉电时刻 | 主区 | 备区 | read 结果 |
|---|---|---|---|
| 写主区写到一半 | 撕裂 | 上一条完好 | 返回备区数据（语义：**最后一条完好记录**，合法） |
| 主区写完、备区未写 | 新记录完好 | 上一条完好 | 返回主区新记录 |
| 写备区写到一半 | 新记录完好 | 撕裂 | 返回主区新记录 |

任何时刻至少一个区站着，读到的可能旧一拍但**永远完好可信**。

## 返回码

| 码 | 含义 |
|---|---|
| `ELIB_SAFE_OK` | 成功（write 两区全落；read 主好/备好且修复成功） |
| `ELIB_SAFE_ERR_INVALID_PARAM` | init 参数非法 / read/write 传了 NULL |
| `ELIB_SAFE_ERR_NOT_INITIALIZED` | 未 init 就调 API |
| `ELIB_SAFE_ERR_IO` | read：底层读非 0 且无完好数据可交付 |
| `ELIB_SAFE_ERR_DATA_INVALID` | write：入口校验拒绝，未落笔 |
| `ELIB_SAFE_ERR_PRIMARY_WRITE_FAILED` | write：主区失败、备区已安全 |
| `ELIB_SAFE_ERR_BACKUP_WRITE_FAILED` | write：备区失败、主区已安全 |
| `ELIB_SAFE_ERR_WRITE_FAILED` | write：两区全失守 |
| `ELIB_SAFE_ERR_RECOVER_WRITE_FAILED` | read：备区数据有效但主区修复写失败 |
| `ELIB_SAFE_ERR_CORRUPTED` | read：两区读得动但内容都无效 |

## 分层一：垫在 elib-nvs-wear-leveling 上（推荐）

NVS v2 是"定长追加写、只存最新记录"的磨损均衡库；给它两个**独立 NVS 域**分别当主备，
safe-storage 就获得完整性与循环磨损双双保障：

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
    /* NVS 定长单元 >= 你的记录大小，读写两个 NVS 域各自自理 */
    return elib_nvs_read(nvs, buf) == ELIB_NVS_OK ? 0 : -1;
}

static int nvs_pair_write(void *ud, elib_safe_region_t region, const uint8_t *buf) {
    const nvs_pair_t *p = (const nvs_pair_t *)ud;
    elib_nvs_ctx_t *nvs = (region == ELIB_SAFE_REGION_PRIMARY) ? p->primary : p->backup;
    /* NVS 写是追加新记录：擦除由 NVS 惰性自理，无需先擦 */
    return elib_nvs_write(nvs, buf) == ELIB_NVS_OK ? 0 : -1;
}

static const elib_safe_ops_t nvs_ops = {
    .user_data = &pair, nvs_pair_read, nvs_pair_write, my_validate,
};
```

磨损代价：每次 `elib_safe_write` → 两个域各追加一条记录 → 每域一个单元寿命，
均摊结果照 NVS 寿命模型翻倍分摊到两个域（各域磨损互相独立）。

## 分层二：直接对接介质

需要你自己处理：记录大小 / 地址布局 / NOR flash 的先擦后写 / EEPROM 的覆写页延迟。
库里没有任何这些假设。

## 内存占用

- **RAM**：仅 `elib_safe_ctx_t`（一个 ops 拷贝 + 1 位状态，几十字节）
- **注意**：read 期间库会先装满你的 buf 再（必要时）原样回写主区修复——**不需要**额外临时缓冲，这是 v1 VLA 硬伤在 v2 被结构性消灭后的红利
- **栈**：无递归、无大缓冲

## 非目标

- 不做 CRC/magic 本身（载荷与校验算法全归用户 validate 回调）
- 不做新旧记录分辨（无 seq；"旧但完好"是合法读结果）
- 不做擦除接口（策略在用户实现内）
- 不做多记录/历史（模型是单记录 + 双份落盘）
