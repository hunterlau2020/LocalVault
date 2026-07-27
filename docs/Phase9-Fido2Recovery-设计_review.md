# Phase 9 FIDO2 & Recovery Manager 设计评审

评审对象：`docs/Phase9-Fido2Recovery-设计.md`  
评审日期：2026-07-28  
评审结论：**拒绝通过，修订后复审**

## 1. 总体结论

当前方案的接口抽象、BIP-39 编码器和 Mock 测试思路可以作为基础设施预研，但尚不能作为满足 V1 安全要求的实现方案。

设计存在以下阻断问题：

1. FIDO2 门插入点使用了已经被 `std::move` 的 `key`，门可能永远不会执行。
2. 适配器不可用时直接放行，违反 SRS 规定的“主密码 + FIDO2（AND）”及失败关闭要求。
3. 恢复服务依赖已经打开的数据库，但设备丢失时正常打开流程会被 FIDO2 门阻断，恢复链路无法闭环。
4. FIDO2 校验发生在数据库完全解密之后，失败分支没有定义明文数据、对象状态和敏感缓存的清理。

在以上问题解决前，不应修改生产 `Database::open()` 来启用该门，也不应允许生产环境创建 `enabled=true` 的 FIDO2 绑定。

## 2. 评审发现

### P0-1：门条件使用移动后的 `key`，FIDO2 校验可能永远不执行

设计位置：原文第 158-165 行。

现有 `Database::open()` 在计划插入点之前执行：

```cpp
reader.readDatabase(&dbFile, std::move(key), this)
```

此后参数 `key` 通常已经为空。设计中的判断：

```cpp
if (key && !checkFido2Gate(&error))
```

会因 `key` 为空而短路，导致 FIDO2 门不执行。

同时，`error` 已经是 `QString*`，使用 `&error` 会得到 `QString**`，与通常的 `checkFido2Gate(QString*)` 接口不匹配。

整改要求：

- 门逻辑不得依赖移动后的 `key`。
- 明确 `checkFido2Gate()` 的错误参数类型及所有权。
- 增加生产调用路径测试，证明绑定启用时门确实执行，而不只是直接调用服务的单元测试。

### P0-2：Unavailable 时放行构成安全降级绕过

设计位置：原文第 167-179、217-222 行。

当前方案规定：绑定已启用但 `adapter.isAvailable() == false` 时跳过 FIDO2 门。攻击者只需使适配器不可用，即可仅凭主密码解锁数据库。

这与以下现有需求直接冲突：

- 启用 FIDO2 后必须采用“主密码 + FIDO2（AND）”模式。
- FIDO2 设备不可用时不得按普通路径解锁。
- 系统必须明确提示错误并引导用户进入恢复路径。

整改要求：

```text
disabled                     -> 普通打开
bound + adapter available    -> 验证 assertion，成功才打开
bound + adapter unavailable  -> 拒绝打开并提示恢复
bound + assertion failed     -> 拒绝打开
explicit recovery mode       -> 校验主密码及恢复密钥包
```

若当前阶段不实现真实 `WebauthnFido2Adapter`：

- 生产环境必须禁止绑定或启用 FIDO2；
- `UnavailableFido2Adapter` 仅能报告能力缺失，不能把已绑定数据库降级为单因子解锁；
- 不得声称 V1 已交付 FIDO2 AND 解锁能力。

### P0-3：恢复流程没有可执行入口，存在先打开还是先恢复的循环依赖

设计位置：原文第 119-154 行。

`RecoveryKeyService` 依赖一个已经解密的 `Database` 才能读取 `KPXC_RECOVERY_ENVELOPE`。但在 FIDO2 已启用且设备丢失时，普通 `Database::open()` 应被 FIDO2 门拒绝，因此调用者无法先获得数据库再调用 `recover()`。

此外：

- CLI 只有 `recovery --generate` 和 `recovery --verify`，没有真正执行恢复打开的 `--recover`。
- 文件清单未包含 GUI 恢复入口及打开流程的上下文传递。
- 未说明普通打开、恢复打开如何选择不同认证策略。

整改要求：

- 为数据库打开定义明确的 Normal/Recovery 上下文，禁止使用隐式全局开关。
- 恢复输入必须作为本次打开请求的一部分传入认证编排层。
- 解密后读取 envelope，但在恢复认证成功前不得向 GUI、CLI、浏览器集成或其他服务发布数据库对象。
- 补充 GUI 恢复入口和 CLI `--recover` 的交互、错误码及安全输入设计。
- 增加“设备丢失 → 主密码 + 恢复词 → 打开成功 → 持久化 rebind_required”的端到端测试。

### P0-4：第二因子失败时，已解密数据库没有清理与隔离方案

设计位置：原文第 15-23、156-171 行。

计划插入门时，`readDatabase()` 已经完成，所有条目明文及 CustomData 已解析进目标 `Database`。设计只规定返回 `false`，没有规定：

- 调用 `releaseData()` 或等价安全清理；
- 擦除条目、附件、历史记录和敏感临时缓冲；
- 回滚 file path、file hash、modified/emit 状态；
- 保证失败的 `Database` 对象不会被调用者继续访问；
- 防止失败对象被信号、GUI 或集成服务观察。

整改要求：

- 优先采用临时数据库解析/隔离模型，认证成功后再提交或发布。
- 若沿用当前对象解析，所有拒绝分支必须执行统一、可测试的失败清理。
- 增加失败后根组、条目、CustomData 和敏感缓存均不可访问的测试。
- 对打开过程中的异常、取消、适配器不可用及恢复失败执行同一清理策略。

### P1-1：状态机没有完整持久化模型

设计位置：原文第 52-103 行。

`Fido2BindingState` 定义了 `Disabled`、`Binding`、`Bound`、`RecoveryMode`、`RebindRequired`，但 `Fido2BindingInfo` 只有 `enabled`，无法区分后三种持久状态。应用重启或数据库重新打开后，`rebind_required` 会丢失或无法还原。

整改要求：

- JSON 中持久化 `schema_version` 和 `state`。
- 补齐架构已有的 `verification_required`、`unavailable` 状态，或明确它们是瞬时状态并定义映射。
- 对未知 schema、未知状态、缺字段和损坏 JSON 采用 fail-closed 策略。
- 定义每个状态允许的转换、触发者、持久化时机和失败回滚。

### P1-2：绑定与恢复密钥生成不是原子安全流程

设计位置：原文第 80-136、145-154 行。

`bind()` 与 `recovery --generate` 是两个独立操作，用户可能在绑定启用后没有生成或确认保存恢复词，从而制造永久锁定风险。这也不满足“启用 FIDO2 时必须生成恢复密钥包”的需求。

整改要求：

```text
注册 FIDO2 凭据
  -> 生成恢复词
  -> 提示离线保存
  -> 用户按抽查规则确认已保存
  -> 原子保存 binding + envelope
  -> enabled=true / state=bound
```

任一步失败或取消，都不得留下 `enabled=true` 的绑定。还需定义保存失败、数据库只读和进程崩溃时的回滚行为。

### P1-3：绑定数据不足以支持真实 WebAuthn assertion 验证

设计位置：原文第 57-78、217-222 行。

当前模型只保存 `credentialId`，不足以完成 WebAuthn 断言校验。至少需要明确：

- RP ID；
- 注册产生的 credential public key/COSE key；
- credential 类型和算法；
- user handle；
- authenticator attachment/platform binding type；
- user presence/user verification 策略；
- challenge 的生成、生命周期和新鲜性；
- authenticator data、RP ID hash、signature 的校验；
- sign counter 的处理与克隆检测策略。

整改要求：

- 在实现真实适配器前完成注册结果、断言请求和验证结果的数据模型。
- 将错误类型结构化，区分取消、超时、设备缺失、签名失败、策略不满足和内部错误。
- 不应把 CTAP2 CBOR/USB HID 描述为使用 `webauthn.dll` 的必需应用层工作；应明确哪些能力由 Windows WebAuthn API 提供，哪些验证由应用负责。

### P1-4：恢复密钥 `consumed` 字段没有行为定义

设计位置：原文第 108-136 行。

模型定义了 `consumed`，但 `recover()` 没有标记消费，也没有拒绝重复使用。

整改要求：

- 明确恢复密钥是单次令牌还是可重复使用。
- 若为单次令牌，恢复成功与 `consumed=true`、`rebind_required` 必须原子保存。
- 重绑后应轮换恢复密钥；旧 envelope 必须失效。
- 若设计为可重复使用，应删除 `consumed` 字段并记录对应威胁模型。

### P2-1：助记词使用普通 Qt 字符串，不满足敏感数据内存要求

设计位置：原文第 31-50、105-136、217-223 行。

`QStringList`、`QString` 和普通 `QByteArray` 可能发生隐式共享及复制，无法保证可靠清零。“不持久化明文”不足以覆盖内存、剪贴板和终端 scrollback 风险。

整改要求：

- 恢复输入、规范化结果、entropy 及 hash 中间值使用可擦除缓冲区并定义清理点。
- CLI 必须通过无回显安全输入读取，不得通过命令行参数接受恢复词。
- 定义 GUI 显示、复制、剪贴板超时清理及窗口关闭后的擦除行为。
- 禁止将助记词、entropy、credential ID、assertion 或恢复 hash 写入日志。
- hash 比较使用 constant-time 比较函数。

### P2-2：缺少 BIP-39 兼容性和失败行为定义

设计位置：原文第 31-50、217-220 行。

整改要求：

- 使用官方 BIP-39 测试向量验证 128/256 bit entropy 编解码。
- 加载后验证词表恰好 2048 个唯一词且顺序正确。
- 明确大小写、连续空白、换行和 Unicode NFKD 规范化规则。
- `toEntropy()` 不应以空数组同时表示“合法空值”和“解析失败”；应返回显式结果/错误。
- 缺失或损坏词表时必须拒绝生成和验证。

### P2-3：测试计划缺少关键负向、安全与持久化场景

设计位置：原文第 181-215 行。

至少补充：

- `enabled + adapter unavailable` 必须拒绝；
- assertion 失败或用户取消后数据库明文不可访问；
- 正常生产打开路径确实调用门；
- 重启后保持 `rebind_required`；
- malformed/未知版本 JSON 必须 fail-closed；
- 绑定成功但恢复词未确认时不得启用；
- 恢复密钥重放和 consumed 行为；
- constant-time hash 比较；
- 缺失/损坏词表；
- 认证失败不得更新 `lastVerifiedAt`；
- 状态保存失败不得向用户报告成功；
- 并发打开、重复点击、取消和超时；
- CLI 非交互环境的安全失败行为。

## 3. 建议的交付拆分

### Phase 9A：基础设施预埋

- BIP-39 编解码及官方向量测试；
- versioned binding/envelope 序列化；
- 完整状态机和迁移测试；
- `IFido2Adapter`、Mock 与 Unavailable 能力探测；
- 生产环境禁止启用 FIDO2；
- 不对已绑定数据库实施任何 fail-open 降级。

### Phase 9B：可用安全功能

- Windows `WebauthnFido2Adapter`；
- fail-closed 的数据库打开编排；
- 临时解析/失败清理；
- 原子绑定和恢复包生成；
- GUI 与 CLI 恢复入口；
- 恢复密钥消费/轮换；
- 安全输入、内存清理及端到端测试。

只有 Phase 9B 完成后，才能声明满足 SRS 的 V1 FIDO2 AND 解锁及恢复能力。

## 4. 复审准入条件

修订版设计至少应提交以下内容后再复审：

1. 去除所有 adapter unavailable 时的普通解锁放行路径。
2. 给出 Normal/Recovery 两种打开模式的完整时序及对象隔离边界。
3. 给出 FIDO2/恢复失败后的统一清理方案。
4. 补齐 versioned binding/envelope 模型和状态持久化规则。
5. 补齐真实 WebAuthn 注册与 assertion 验证所需数据。
6. 将绑定、恢复词生成、用户确认和启用设计为可回滚流程。
7. 明确恢复密钥单次使用或重复使用策略。
8. 补齐 GUI/CLI 恢复入口以及敏感输入保护。
9. 补充上述负向、安全、持久化和端到端测试。

在这些条件满足前，本评审保持“拒绝通过”。
