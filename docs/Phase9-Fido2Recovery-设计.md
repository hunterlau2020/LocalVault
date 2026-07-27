# Phase 9 设计文档 — FIDO2 & Recovery Manager（方案 B：接口+Mock）

> 本文档为 Phase 9 实现设计，供专家评审。评审通过后再实现。

## Context

Phase 9 是最后一个阶段。FIDO2 作为解锁第二因子（主密码 AND FIDO2），恢复密钥包作为 FIDO2 设备丢失时的绕过令牌。V1 用 `IFido2Adapter` 抽象 + Mock 适配器（测试用）+ Unavailable 适配器（生产，标 `unavailable` → 门跳过）。真 `webauthn.dll` 适配器后续补即激活门。

**关键约束**：
- FIDO2 是 **GATE** 不是 CompositeKey 组件（不参与主密钥派生）。
- 恢复密钥包**不参与数据库解密**，仅授权令牌（绕过 FIDO2 校验）。
- 恢复流程中仍需主密码正常解密数据库。
- 恢复成功后状态标 `rebind_required`。

## 探索确认（代码侧）

| 发现 | 详情 |
|---|---|
| **FIDO2 门插入点** | `Database::open()` line 207-214（readDatabase 成功后、emit databaseOpened 前）。此刻主密码已验证 + DB 完全解析 + CustomData 可读 |
| **FIDO2 ≠ ChallengeResponseKey** | YubiKey CR 是 KEY（折进 CompositeKey::rawKey()）；FIDO2 是 GATE（独立授权检查，不碰 key 派生） |
| **Protected CustomData** | 加 `KPXC_FIDO2_BINDING` / `KPXC_RECOVERY_ENVELOPE`，扩展 `isProtected()`。同 Phase 10 RemoteConfigService 模式。注意：Protected 只控制显示语义（db-show 打 [PROTECTED]、FDO 过滤），落盘加密靠 KDBX 外层密码器——同 Phase 10 |
| **BIP-39 词表** | 无 BIP-39 wordlist。须嵌入 `share/wordlists/bip39_english.wordlist`（2048 词，公有领域），`share/CMakeLists.txt:19-20` 自动安装 |
| **verification_hash** | `CryptoHash::hash(salt + recovery_key_bytes, Sha256)`（src/crypto/CryptoHash.h:41），16B salt from `Random::randomArray(16)`。高熵密钥 → salted SHA-256 足够（Argon2 大材小用） |
| **Random** | `randomGen()->randomArray(16)` → 128bit → 12 词；`randomArray(32)` → 256bit → 24 词 |
| **CLI 模式** | RemoteCommand 的 --bind/--unbind/--status 子命令风格 |

---

## 设计

### 1. BIP-39 助记词编码器（src/core/Bip39Mnemonic.h/.cpp）

```cpp
class Bip39Mnemonic {
public:
    // 熵 → 助记词（含校验词）
    static QStringList generate(int wordCount = 12);  // 12 or 24
    // 助记词 → 验证合法性（校验词 + 词表）
    static bool validate(const QStringList& words);
    // 助记词 → 熵字节（用于 verification_hash）
    static QByteArray toEntropy(const QStringList& words);
};
```

**编码逻辑**：
- `randomArray(16)` → 128bit → 12 词（11bit×11 + 1bit×1 + 4bit 校验）
- `randomArray(32)` → 256bit → 24 词
- 校验词 = SHA-256(entropy) 的前 ENT/32 bit

**词表**：`share/wordlists/bip39_english.wordlist`（2048 词）。运行时 `Resources::wordlistPath("bip39_english.wordlist")` 解析路径。

### 2. FIDO2 适配器抽象（src/core/Fido2Adapter.h）

```cpp
enum class Fido2BindingState { Disabled, Binding, Bound, RecoveryMode, RebindRequired };

struct Fido2BindingInfo {
    QString credentialId;       // FIDO2 credential ID（hex）
    QString deviceLabel;        // 用户可读标签
    QDateTime boundAt;
    QDateTime lastVerifiedAt;
    bool enabled = false;
    QJsonObject toJson() const;
    static Fido2BindingInfo fromJson(const QJsonObject&);
};

class IFido2Adapter {
public:
    virtual ~IFido2Adapter() = default;
    virtual bool isAvailable() const = 0;
    virtual Fido2BindingInfo startBinding(const QString& deviceLabel, QString* error) = 0;
    virtual bool verifyAssertion(const Fido2BindingInfo& binding, QString* error) = 0;
};
```

**两个具体实现**：
- `MockFido2Adapter`（测试）：可配置 isAvailable、startBinding 返回预设 credentialId、verifyAssertion 返回预设结果。用于单测模拟绑定/断言。
- `UnavailableFido2Adapter`（生产 V1）：`isAvailable()` 返回 false。所有操作返回错误「FIDO2 adapter unavailable」。后续替换为 `WebauthnFido2Adapter`（webauthn.dll）即激活。

### 3. Fido2BindingService（src/core/Fido2BindingService.h/.cpp）

```cpp
class Fido2BindingService {
public:
    explicit Fido2BindingService(QSharedPointer<Database> db, IFido2Adapter* adapter);

    Fido2BindingState state() const;           // 当前状态
    Fido2BindingInfo binding() const;           // 当前绑定信息

    bool bind(const QString& deviceLabel, QString* error);   // binding→bound
    bool unbind(QString* error);                               // bound→disabled
    bool verify(QString* error);                              // 调 adapter.verifyAssertion
    bool markRebindRequired();                                 // →RebindRequired
    bool markRecoveryCompleted();                              // RecoveryMode→RebindRequired

private:
    void load();    // 从 CustomData(KPXC_FIDO2_BINDING) 读取
    void save();    // 写入 CustomData
    ...
};
```

状态机：`Disabled → Binding → Bound →（设备丢失）→ RecoveryMode →（恢复成功）→ RebindRequired →（重新绑定）→ Bound`

### 4. RecoveryKeyService（src/core/RecoveryKeyService.h/.cpp）

```cpp
struct RecoveryKeyEnvelope {
    QString recoveryKeyId;         // UUID
    int mnemonicWordCount = 12;    // 12 or 24
    QDateTime createdAt;
    bool consumed = false;
    QByteArray salt;               // 16B random salt（hex）
    QString verificationHash;      // SHA-256(salt + entropy)（hex）
    QJsonObject toJson() const;
    static RecoveryKeyEnvelope fromJson(const QJsonObject&);
};

class RecoveryKeyService {
public:
    explicit RecoveryKeyService(QSharedPointer<Database> db);

    // 生成助记词 + 存 envelope（verification_hash = SHA-256(salt + entropy)）
    QStringList generate(int wordCount = 12);
    // 验证用户输入的助记词
    bool validate(const QStringList& words);
    // 恢复：验证助记词 + 标 rebind_required
    bool recover(const QStringList& words);
    // 当前 envelope
    RecoveryKeyEnvelope envelope() const;

private:
    void load();    // 从 CustomData(KPXC_RECOVERY_ENVELOPE)
    void save();
    ...
};
```

**恢复流程**（架构 §12.5）：
1. 用户输入主密码 + 恢复密钥包（助记词）。
2. 系统：BIP-39 验证助记词 → toEntropy → SHA-256(salt + entropy) 对比 verificationHash。
3. 匹配 → 允许主密码解密 → 标 `rebind_required`。
4. 不匹配 → 恢复失败。

### 5. CLI 命令

**`fido2` 命令**（`src/cli/Fido2Command.h/.cpp`）：
- `fido2 <db> --bind --label "my-key"` — 绑定 FIDO2 设备（V1 用 Unavailable 适配器 → 报告 unavailable）。
- `fido2 <db> --unbind` — 解除绑定。
- `fido2 <db> --status` — 显示当前绑定状态。

**`recovery` 命令**（`src/cli/RecoveryCommand.h/.cpp`）：
- `recovery <db> --generate [--words 12|24]` — 生成恢复密钥包（显示助记词 + 存 envelope）。
- `recovery <db> --verify` — 验证用户输入的助记词。

### 6. Database::open() FIDO2 门

在 `Database::open()` line 207-214 之间插入：
```cpp
// Phase 9: FIDO2 second-factor gate (after master key verified).
if (key && !checkFido2Gate(&error)) {
    if (error) *error = ...;
    return false;
}
```

`checkFido2Gate()` 逻辑：
1. 读 Fido2BindingInfo from CustomData。
2. 若 `!binding.enabled` → 跳过（FIDO2 未启用）。
3. 若 `adapter.isAvailable()` → `adapter.verifyAssertion(binding)` → 失败则 return false。
4. 若 `!adapter.isAvailable()` → **V1 跳过**（门不强制；后续真适配器补上即激活）。

---

## V1 边界声明

- 生产适配器 `UnavailableFido2Adapter`（`isAvailable()=false`）→ **门跳过，V1 不强制 AND 模式**。
- 绑定信息存储 + 状态机 + 恢复密钥生成/验证全实现可测（Mock 适配器用于单测）。
- 真 `webauthn.dll` 适配器（`WebauthnFido2Adapter`）后续补 → 门自动激活。

## 文件清单

### 新增
| 文件 | 内容 |
|---|---|
| `share/wordlists/bip39_english.wordlist` | BIP-39 英文词表（2048 词） |
| `src/core/Bip39Mnemonic.h/.cpp` | 熵→词+校验 / 词→熵+验证 |
| `src/core/Fido2Adapter.h/.cpp` | `IFido2Adapter` + `Fido2BindingInfo` + `MockFido2Adapter` + `UnavailableFido2Adapter` |
| `src/core/Fido2BindingService.h/.cpp` | 状态机 + CustomData 存储 |
| `src/core/RecoveryKeyService.h/.cpp` | 助记词生成/验证/恢复 + envelope 存储 |
| `src/cli/Fido2Command.h/.cpp` | `fido2 --bind/--unbind/--status` |
| `src/cli/RecoveryCommand.h/.cpp` | `recovery --generate/--verify` |
| `tests/TestFido2.cpp` | 状态机 + Mock 绑定/断言 + 门 |
| `tests/TestRecovery.cpp` | BIP-39 往返 + 助记词生成/验证 + 恢复流程 |

### 修改
| 文件 | 改动 |
|---|---|
| `src/core/CustomData.h/.cpp` | 加 `Fido2Binding`/`RecoveryEnvelope` 常量 + 扩展 `isProtected()` |
| `src/core/Database.cpp` | line 207-214 插入 FIDO2 门 |
| `src/cli/Command.cpp` | 注册 `fido2`/`recovery` |
| `src/CMakeLists.txt` | core_SOURCES 加新 .cpp |
| `src/cli/CMakeLists.txt` | cli_SOURCES 加新 .cpp |
| `tests/CMakeLists.txt` | 加 testfido2 / testrecovery |
| `tests/TestCli.cpp` | 命令计数 +2 → 35 |

## 分步实现

1. **BIP-39 词表 + 编码器**：词表获取 + `Bip39Mnemonic.h/.cpp`。单测：往返、校验词、错误检测。
2. **FIDO2 接口 + Mock/Unavailable 适配器**：`Fido2Adapter.h`。
3. **Fido2BindingService**：状态机 + 存储。单测(Mock)：绑定/解绑/状态流转。
4. **RecoveryKeyService**：生成/验证/恢复。单测：12/24 词、验证正确/错误、恢复状态。
5. **CLI 命令**：fido2 + recovery。TestCli 计数更新。
6. **Database::open() 门**：conditional on adapter availability。单测(Mock)：bound+verify 通过/失败。
7. **CMake + 全量回归**。

## 风险与边界

1. **BIP-39 词表来源**：公有领域（bitcoin/bips 仓库）。须确保完整 2048 词、无截断。
2. **Protected CustomData 非 inner-stream 加密**：同 Phase 10。落盘靠 KDBX 外层密码器。若需额外保护，用 `SymmetricCipher Aes256_GCM`（src/crypto/SymmetricCipher.h:41）加密 envelope blob。
3. **V1 不强制 AND 模式**：Unavailable 适配器 → 门跳过。绑定/恢复逻辑全实现可测；真 webauthn.dll 补上即激活。
4. **webauthn.dll 复杂度**：CTAP2 CBOR 编码、Windows Hello 交互、USB HID — 留作后续 `WebauthnFido2Adapter` 独立工作。
5. **助记词安全**：显示后不持久化明文（仅 verification_hash 存库）；用户须离线保存。CLI `--generate` 显示一次。
