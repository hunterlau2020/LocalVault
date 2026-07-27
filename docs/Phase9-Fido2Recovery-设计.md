# Phase 9 设计文档（修订版）— FIDO2 & Recovery Manager

> 修订日期：2026-07-28。本版按专家评审 `Phase9-Fido2Recovery-设计_review.md` 的 4 P0 + 4 P1 + 3 P2 反馈全面修订。核心变更：fail-closed 取代 fail-open、恢复作为打开请求的一部分（打破循环依赖）、失败后明文清理、原子绑定+恢复流程、9A/9B 交付拆分。

## Context

FIDO2 作为解锁第二因子（主密码 AND FIDO2），恢复密钥包作为设备丢失时的绕过令牌。**核心原则（修订）：fail-closed——绑定启用后，适配器不可用或断言失败时拒绝打开（不得放行），引导用户走恢复路径。**

---

## P0 修复

### P0-1：门条件不依赖 move 后的 key

**问题**：`reader.readDatabase(&dbFile, std::move(key), this)` 后 key 已空。

**修复**：在 readDatabase **之前**保存标志：
```cpp
const bool hadKey = !key.isNull();  // 保存，move 前
// ... reader.readDatabase(&dbFile, std::move(key), this) ...
if (hadKey && !checkFido2Gate(error)) {  // 用 hadKey，不用 key
    clearDecryptedData();
    return false;
}
```
错误参数：`checkFido2Gate(QString* error)`，传 `error`（已是 `QString*`），不用 `&error`。

### P0-2：Unavailable 时 fail-closed（不放行）

**问题**：原设计「适配器不可用→跳过门」是 fail-open 安全洞。

**修复——完整门逻辑**：
```
disabled                     → 普通打开（无 FIDO2 检查）
bound + adapter available    → verifyAssertion → 成功才打开，失败拒绝
bound + adapter unavailable  → 拒绝打开 + 提示「FIDO2 设备不可用，请使用恢复密钥包」
bound + assertion failed     → 拒绝打开
recovery mode（显式）         → 验证主密码 + 恢复密钥包 → 打开 + 标 rebind_required
```

**V1 生产约束**：`UnavailableFido2Adapter`（available=false）→ 若绑定已启用，**打开必定失败**（除非走恢复路径）。因此 **V1 生产禁止启用 FIDO2 绑定**——`fido2 --bind` 在 adapter 不可用时拒绝绑定并报错。绑定信息/状态机/恢复密钥可预生成存储，但 `enabled` 保持 false 直到真适配器可用。

### P0-3：恢复作为打开请求的一部分（打破循环依赖）

**问题**：RecoveryKeyService 需已打开 DB 读 envelope，但 FIDO2 门阻止打开 → 循环依赖。

**修复——OpenMode 枚举 + 恢复输入作为打开参数**：

```cpp
enum class OpenAuthMode { Normal, Recovery };

struct OpenAuthContext {
    OpenAuthMode mode = OpenAuthMode::Normal;
    QStringList recoveryWords;  // Recovery 模式时传入
};
```

Database 新增成员 `m_authContext`，在 `open()` 前由调用方设置。门逻辑：
- **Normal**：解密成功后 → 若 binding.enabled → checkFido2Gate（assert 或 reject-unavailable）。
- **Recovery**：解密成功后 → **跳过 FIDO2 断言** → 读 envelope → 验证 recoveryWords 的 verification_hash → 匹配则标 rebind_required → 通过；不匹配则拒绝。

恢复流程完整时序：
```
用户选「使用恢复密钥包」→ 输入主密码 + 恢复词 →
db->setAuthContext({Recovery, words}) → db->open(filePath, key) →
  [内部：readDatabase 解密成功] →
  [Recovery 模式：读 KPXC_RECOVERY_ENVELOPE → 验证 words 的 hash] →
  匹配 → 标 rebind_required → emit databaseOpened → 成功
  不匹配 → clearDecryptedData → return false
```

**CLI**：新增 `recovery --recover <db>`（交互输入主密码 + 恢复词），设置 OpenAuthContext 后调 open。

### P0-4：失败后明文清理

**问题**：readDatabase 已完成、DB 完全解密，门失败只 return false，明文留在内存。

**修复——clearDecryptedData()**：

```cpp
void Database::clearDecryptedData() {
    m_rootGroup.reset();        // 丢弃整个条目树（所有明文条目/附件/历史）
    m_metadata.reset(new Metadata(this));  // 重置元数据（含 CustomData）
    m_data.key.reset();         // 丢弃 transformed key
    m_data.transformedDatabaseKey.reset();
    // 不 emit databaseOpened → GUI/服务不会观察到这个失败对象
}
```

在**所有**门失败分支（assertion 失败、adapter unavailable、recovery 验证失败）统一调用 `clearDecryptedData()` 后 return false。调用方拿到 false → 丢弃 Database 对象（QSharedPointer 析构 → 剩余内存释放）。

单测验证：门失败后 `db->rootGroup()` 返回 null、`db->metadata()->customData()->keys()` 为空。

---

## P1 修复

### P1-1：状态机完整持久化

Fido2BindingInfo JSON 增加 `state` 字段 + `schema_version`：
```json
{
  "schema_version": 1,
  "state": "bound",           // disabled|bound|recovery_mode|rebind_required
  "credential_id": "...",
  "device_label": "...",
  "bound_at": "...",
  "last_verified_at": "...",
  "rp_id": "localvault.local",
  "credential_public_key": "...",  // COSE key (hex), 真适配器需要
  "sign_counter": 0                // 克隆检测
}
```
- 未知 schema/state/缺字段 → **fail-closed**（拒绝打开）。
- 重启后恢复持久状态（rebind_required 保持）。

### P1-2：原子绑定+恢复流程

`fido2 --bind` 改为原子多步流程（全成功才 enabled=true）：
```
1. adapter.startBinding() → 获取 credential
2. RecoveryKeyService.generate() → 生成恢复词
3. 显示恢复词 → 用户确认已保存（输入前 N 个词验证）
4. 原子保存 binding(state=binding) + envelope → db->save()
5. 若 save 成功 → binding.state = bound → db->save()
   若任一步失败/取消 → 不启用（state 保持 disabled/binding，不留 enabled=true）
```
V1（adapter unavailable）：步骤 1 失败 → 整个流程不执行 → FIDO2 不启用。

### P1-3：WebAuthn 数据模型（为真适配器预留）

Fido2BindingInfo 补齐字段（P1-1 已含 rp_id/credential_public_key/sign_counter）。真适配器（9B）需：
- challenge 生成/新鲜性、authenticator data 校验、RP ID hash 比对、signature 验证、sign counter 克隆检测。
- 这些由 `WebauthnFido2Adapter` 内部处理（调 webauthn.dll），上层只调 `verifyAssertion(binding)`。

### P1-4：恢复密钥 consumed 行为

**单次令牌**：
- `recover()` 成功 → envelope.consumed = true + binding.state = rebind_required → 原子保存。
- 重复使用已 consumed 的 envelope → 拒绝。
- 重绑（rebind）后 → 轮换恢复密钥（生成新 envelope，旧 envelope 失效）。

---

## P2 修复

### P2-1：敏感数据内存安全

- 恢复词/entropy/hash 中间值用 `Botan::secure_vector<char>`（零化析构，同 PasswordKey）。
- CLI 恢复词输入：无回显（`Utils::setStdinEcho(false)`，同密码输入），不接受命令行参数。
- hash 比较：constant-time（`Botan::PKCS::verify_ciphertext` 或手写常量时间比较）。
- 禁止将助记词/entropy/credential ID/assertion 写入日志。

### P2-2：BIP-39 兼容性

- 使用官方 BIP-39 测试向量验证 128/256 bit 编解码。
- 词表加载后验证恰好 2048 个唯一词。
- NFKD 规范化、大小写规则（BIP-39 小写）、连续空白处理。
- `toEntropy()` 返回 `QPair<bool, QByteArray>`（成功标志 + 熵），不以空数组同时表示「合法空」和「解析失败」。
- 词表缺失/损坏 → 拒绝生成和验证。

### P2-3：测试计划补齐

关键负向/安全/持久化场景：
- `enabled + adapter unavailable` → 拒绝打开。
- assertion 失败/取消 → DB 明文不可访问（rootGroup null）。
- 正常生产打开确实调门。
- 重启后保持 `rebind_required`。
- malformed/未知版本 JSON → fail-closed。
- 绑定成功但恢复词未确认 → 不启用。
- 恢复密钥重放（consumed=true 拒绝）。
- constant-time hash 比较。
- 缺失/损坏词表 → 拒绝。
- 认证失败不更新 `last_verified_at`。
- 状态保存失败不报告成功。

---

## 9A / 9B 交付拆分

### Phase 9A：基础设施预埋（可交付，不含安全门）

| 内容 | 文件 |
|---|---|
| BIP-39 编解码（官方测试向量） | `src/core/Bip39Mnemonic.h/.cpp` + `share/wordlists/bip39_english.wordlist` |
| Versioned Fido2BindingInfo + RecoveryKeyEnvelope 序列化 | `src/core/Fido2Binding.h/.cpp`（数据结构 + JSON 往返 + schema 验证） |
| 完整状态机（持久化 state 字段） | 同上 |
| IFido2Adapter + MockFido2Adapter + UnavailableFido2Adapter | `src/core/Fido2Adapter.h/.cpp` |
| RecoveryKeyService（生成/验证/consumed） | `src/core/RecoveryKeyService.h/.cpp` |
| **生产禁止启用 FIDO2**（`fido2 --bind` 在 unavailable 时拒绝） | `src/cli/Fido2Command.h/.cpp` |
| **不修改 Database::open()** | — |
| CLI `fido2 --status` / `recovery --generate/--verify` | `src/cli/Fido2Command.h/.cpp` + `src/cli/RecoveryCommand.h/.cpp` |
| 全面单测（BIP-39 往返、状态机、envelope 往返、consumed、词表校验、Mock 绑定/断言） | `tests/TestFido2.cpp` + `tests/TestRecovery.cpp` |

**9A 声明**：基础设施预研，**不声明 V1 FIDO2 AND 解锁能力**。生产不可启用绑定。

### Phase 9B：可用安全功能（需 webauthn.dll + 完整编排）

| 内容 |
|---|
| `WebauthnFido2Adapter`（webauthn.dll，CTAP2/WebAuthn 注册+断言） |
| Database::open() fail-closed 门（hadKey 标志 + OpenAuthContext + clearDecryptedData） |
| Normal/Recovery 打开模式编排（恢复输入作为打开参数） |
| 原子绑定+恢复流程（bind→generate→confirm→save→enable） |
| GUI 恢复入口 + CLI `recovery --recover` |
| 恢复密钥消费/轮换 |
| 安全输入（secure_vector + 无回显 + constant-time 比较） |
| 端到端测试（设备丢失→恢复词→打开→rebind_required） |

**9B 完成后**：可声明 SRS V1 FIDO2 AND 解锁+恢复能力。

---

## 复审准入条件检查清单

| # | 条件 | 本版处理 |
|---|---|---|
| 1 | 去除 adapter unavailable 时的普通解锁放行 | ✅ P0-2：fail-closed，unavailable→拒绝 |
| 2 | Normal/Recovery 打开模式完整时序 + 对象隔离边界 | ✅ P0-3：OpenAuthContext + 恢复词作为打开参数 + clearDecryptedData |
| 3 | FIDO2/恢复失败后的统一清理方案 | ✅ P0-4：clearDecryptedData() 所有失败分支 |
| 4 | Versioned binding/envelope 模型 + 状态持久化 | ✅ P1-1：schema_version + state 字段 + fail-closed |
| 5 | 真实 WebAuthn 注册/断言所需数据模型 | ✅ P1-3：rp_id/credential_public_key/sign_counter 等 |
| 6 | 绑定+恢复词生成+确认+启用可回滚 | ✅ P1-2：原子流程，任一步失败不 enabled |
| 7 | 恢复密钥单次/重复使用策略 | ✅ P1-4：单次令牌，consumed + 轮换 |
| 8 | GUI/CLI 恢复入口 + 敏感输入保护 | ✅ P2-1：CLI 无回显 + secure_vector；GUI 留 9B |
| 9 | 负向/安全/持久化/端到端测试 | ✅ P2-3：12+ 测试场景 |

---

## 9A 分步实现

1. **BIP-39 词表 + 编码器**：`bip39_english.wordlist` + `Bip39Mnemonic.h/.cpp`。单测：官方测试向量往返 + 校验词 + 错误检测 + 词表校验。
2. **数据结构 + 序列化**：`Fido2Binding.h/.cpp`（Fido2BindingInfo + RecoveryKeyEnvelope + state 枚举 + versioned JSON + fail-closed）。单测：往返 + unknown schema 拒绝。
3. **FIDO2 接口 + Mock/Unavailable**：`Fido2Adapter.h/.cpp`。Mock 可配置返回值；Unavailable 报不可用。
4. **RecoveryKeyService**：生成（BIP-39 → secure_vector entropy → salted SHA-256 hash → 存 envelope）/验证（constant-time hash 比较）/consumed 标记。单测：12/24 词、验证正确/错误/consumed 拒绝。
5. **CLI**：`fido2 --status`（只读）/ `recovery --generate [--words 12|24]`（显示助记词一次 + 存 envelope）/ `recovery --verify`（无回显输入验证）。**不提供 `fido2 --bind`**（V1 生产禁止启用，adapter unavailable）。注册 + TestCli 计数。
6. **CMake + 全量回归**。

## 9A 验证

1. 构建：`cmake --build build --config Release`（单线程）。
2. 单测：`testfido2`（BIP-39 往返 + 数据结构 + Mock 绑定/断言）+ `testrecovery`（生成/验证/consumed）。
3. CLI 手测：`recovery --generate --words 12`（显示助记词）/ `recovery --verify`（输入验证）。
4. 全量 ctest 无回归。
5. 确认：生产无 `fido2 --bind`（不可启用绑定）。
