# elib-safe-storage v3 设计：分模式 + 记录开头 4 字节版本由库独占

日期：2026-11-25
状态：已实现
取代：`2026-10-27-elib-safe-storage-v2-design.md`

## 为什么改

v2 的读语义是"主区优先"：主区校验通过就直接返回，备区从不参与。这在**直连介质**上是对的，
但**垫在 elib-nvs-wear-leveling 上**会出静默回滚：

| 后端 | 掉电/撕裂后的主区状态 | 主区优先是否正确 |
|---|---|---|
| 直连介质 | 写是覆盖，撕裂 → CRC 不过 → 无效；主区要么最新要么不可用 | ✅ 正确 |
| 垫在 NVS 上 | 均衡层回绕覆盖旧槽，flash 页丢失后返回**更旧但 CRC 合法的已提交记录** | ❌ 静默回滚 |

所以 v3 分两种模式：直连保持 v2；垫 NVS 时给每条记录加一个库自管的版本号，读时取两区中
版本更大的那条。

## 两种模式

```c
typedef enum {
    ELIB_SAFE_MODE_DIRECT       = 0, /* 直连介质：主区优先（v2 语义） */
    ELIB_SAFE_MODE_WEAR_LEVELED = 1  /* 垫在 NVS 上：按版本取新 */
} elib_safe_mode_t;
```

- `DIRECT`：记录布局自由，库不碰任何字节，不引入版本开销。
- `WEAR_LEVELED`：记录**前 4 字节**归库独占，用户不读、不写、不参与校验。

## 版本槽（WEAR_LEVELED，库私有）

- 位置：记录 `[0..3]`，固定，无偏移配置。
- 编码：小端 32 位。
- 有效区间：`[1, 0xFFFFFFFE]`；`0x00000000` 与 `0xFFFFFFFF`（全 0 / 全 F 擦除态）无效。
- 递增：写时 `+1`，跳过两个无效值（`0xFFFFFFFE` → `1`）。
- 比较：回绕安全 `(int32_t)(a - b) > 0`；存活记录数远小于 2^31。
- 播种：由 `elib_safe_read` 在两区都成功读出时，取可见最大有效版本写入 ctx；
  `write` 在未播种时返回 `ELIB_SAFE_ERR_NO_VERSION`。
- 为什么库独占：用户不碰这 4 字节，字节序由库固定，**不存在结构体大小端问题**；
  版本也不会因为用户忘记递增而出错。

用户唯一需要配合的一点：**CRC 必须跳过前 4 字节**（库会改写它）。

## API

```c
typedef struct {
    void *user_data;
    int      (*read)    (void *ud, elib_safe_region_t region, uint8_t *buf);
    int      (*write)   (void *ud, elib_safe_region_t region, const uint8_t *buf);
    uint8_t  (*validate)(void *ud, const uint8_t *buf);   /* WL: 跳过前 4 字节 */
} elib_safe_ops_t;

elib_safe_err_t elib_safe_init(elib_safe_ctx_t *ctx, const elib_safe_ops_t *ops,
                               elib_safe_mode_t mode);
elib_safe_err_t elib_safe_read(elib_safe_ctx_t *ctx, uint8_t *buf);
elib_safe_err_t elib_safe_write(elib_safe_ctx_t *ctx, uint8_t *buf); /* 非 const */
```

新增错误码 `ELIB_SAFE_ERR_NO_VERSION`。`write` 的 `buf` 改为可变，因为 WL 下库要就地写版本槽。

## 读流程

### DIRECT（与 v2 一致）

1. 读主区 → `buf`；读到且 `validate` 过 → `OK`（不读备区）。
2. 否则读备区 → `buf`；过 → 回写主区修复 → `OK` / `RECOVER_WRITE_FAILED`。
3. 都不行 → 有读失败 → `IO`，否则 `CORRUPTED`。

### WEAR_LEVELED

1. 读主区 → `buf`；`p_ok = 读到 && validate && 版本有效`；记 `p_ver`。
2. 读备区 → `buf`；`b_ok = 读到 && validate && 版本有效`；记 `b_ver`。
3. 裁决：

| 两区状态 | 行为 | 返回 |
|---|---|---|
| 都有效，主区版本更新 | 重读主区入 `buf` | OK |
| 都有效，备区版本更新 | `buf` 已是备区 → 回写主区修复 | OK / RECOVER_WRITE_FAILED |
| 都有效，版本相等 | 版本唯一 ⟹ 同一条 | OK |
| 仅主区有效 | 重读主区入 `buf` | OK |
| 仅备区有效 | `buf` 已是备区 → 回写主区修复 | OK / RECOVER_WRITE_FAILED |
| 都无效 | — | 有读失败 → IO，否则 CORRUPTED |

4. 播种：两区都成功读取时 `ctx->version = max(可见有效版本, 0)`、`version_known = 1`；
   有读失败则不播种（避免用过低的版本播种）。

## 写流程

- DIRECT：与 v2 完全相同（不碰前 4 字节、不改 `buf`）。
- WEAR_LEVELED：
  1. 未播种 → `NO_VERSION`；
  2. 入口 `validate` 不过 → `DATA_INVALID`；
  3. `v = next_version(ctx->version)`，小端写入 `buf[0..3]`；
  4. 先主后备，四态返回同 v2；
  5. 至少一区写成功则 `ctx->version = v`。

## 掉电时序（WEAR_LEVELED）

| 掉电位置 | 主区 | 备区 | 上电 read |
|---|---|---|---|
| 双写完成 | V | V | V（等值） |
| 主写一半 | 撕裂/旧 | V | 备区 V + 修复主区 |
| 备写一半 | V | 撕裂/旧 | 主区 V（重读一次） |
| 主区整页丢失 | V−k（旧但合法） | V | 备区 V + 修复主区 |
| 版本槽撕成 0/全 F | 无效版本 | V | 备区 V |

## 测试

`test/test_elib_safe.c`，18 组用例：

- DIRECT：init 校验、fresh media、写四态、入口校验、主区撕裂修复、修复失败、IO 优先级、
  guards、DIRECT 不改写前 4 字节。
- WEAR_LEVELED：未读先写拒绝、fresh 播种、**主区旧备区新返回备区并修复**、主区新不误修备区、
  等版本、版本回绕、`0`/全 F 版本被跳过、掉电四点矩阵。

## 非目标

- 两区同时损坏仍按 IO / CORRUPTED 上报。
- 版本唯一性、CRC 跳过前 4 字节、版本槽在记录内，均由用户保证。
- 版本槽撕裂成非 0、非全 F 的垃圾值库无法识别（不在 CRC 保护内），属已知残余风险。
- 不做擦除接口、多记录、坏块管理。
