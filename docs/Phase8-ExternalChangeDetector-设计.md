# Phase 8 实现计划 — External Change Detector（外部变更检测器）

## Context（为什么做）

LocalVault 的同步以「本地库可信」为前提：SyncEngine（Phase 3-4）直接读 `KPXC_SYNC_METADATA` 里的版本向量/墓碑/冲突索引来判定并发。但**目前没有任何机制验证本地 KDBX 是否被 LocalVault 之外的手段改过**——如果用户用别的工具、或被同步盘/恶意程序篡改了库，SyncEngine 会基于被污染的元数据继续合并，污染扩散。

Phase 8 要在「打开后 / 同步前 / 修复前 / 压缩前」检测外部修改，并在检测到时**阻断同步、进入受控修复模式**（重建索引 / 重置基线 / 标记新分支），保证同步基线可信。这是完整同步链路「本地可信」这道关（Phase 10 远端通道的上游依赖）。

**用户已锁定的两个决策**：
1. **完整 Phase 8 一次交付**（5 子模块 + 三层检测 + 3 修复模式 + CLI + 完整测试矩阵）。
2. **基线并入 `KPXC_SYNC_METADATA`**：新增 `integrity_summary` 嵌套对象，`schema_version` 1→2，写 v1→v2 迁移（纯增量，非破坏性）。

---

## 关键设计决策（探索后已确定）

### A. 文件哈希的「自引用循环」问题与解法

把「整个 KDBX 文件的 SHA-256」存进**该文件内部**是自相矛盾的：存储动作改变了被哈希的字节，导致每次保存哈希都滞后一个周期、检测时恒为误报（已推演确认会震荡）。

**解法**：存储字段名仍叫 `file_sha256`（对齐 FSD 报告字段 `file_hash_changed`），但**计算对象是「逻辑内容的规范摘要」，且把 `integrity_summary` 自身排除在外**。这样摘要可重现、自洽、不震荡。具体两个摘要：

- **`metadata_root_digest`**（快速层信号）= SHA-256 over 规范 JSON `{device_registry, sync_baselines, tombstones, conflicts, + 所有 entry 的 VV}`，**排除 `integrity_summary`**。检测「元数据被外部篡改」。
- **`content_sha256`**（驱动 `file_hash_changed`）= SHA-256 over 规范 JSON `{所有 entry 的 uuid/title/username/url/notes/custom attrs/timeinfo, group 树}` + `metadata_root_digest`。检测「任意内容被外部改动」。

两个摘要都从**内存逻辑状态**计算、在**同一次 save** 内写入 → 完全自洽。

### B. `file_size` / `file_mtime_utc` 定位为「廉价 advisory 信号」

它们同样存在「存进去就改变文件」的滞后，且无法做到严格自洽。因此 V1 中**密码学权威信号是上面两个摘要**；`file_size`/`mtime` 仅作快速预筛/提升 severity，不作为可信判据。spec 的「文件大小/时间变化能检测」由 advisory + 摘要共同覆盖。

### C. 三个修复模式的语义边界

| RepairPlanType | 做什么 | 复用 |
|---|---|---|
| `REBUILD_INDEX` | 重建 entry→VV 映射、tombstone/conflict/snapshot 索引一致性修复（只修元数据，不动业务条目） | SyncMetadataEngine + SnapshotService |
| `RESET_SYNC_BASELINE` | 清空 `sync_baselines`（失效 cursor），把当前库标记为新同步基线 | SyncMetadataEngine |
| `MARK_NEW_BRANCH` | 当前库视为新分支：抬升所有 entry 的 VV（`incrementEntryCounter`），旧冲突/历史保留为只读参考 | SyncMetadataEngine + ConflictResolver 既有标记 |

### D. 磁盘格式影响范围（评审重点）

**整个 Phase 8 对 KDBX 存储格式的改动只有一处**：往 `KPXC_SYNC_METADATA` 这个 CustomData 值的 JSON 里**新增一个顶层 key** `integrity_summary`，并把 `schema_version` 1→2。其余全部是代码层改动（新增 C++ 类、检测逻辑、CLI、SyncEngine 钩子），不碰磁盘格式。

变更前后（仅 `KPXC_SYNC_METADATA` 的 JSON）：

```json
// 现在 (schema_version = 1)
{ "schema_version": 1,
  "device_registry": [...], "sync_baselines": [...],
  "tombstones": [...], "conflicts": [...] }

// 改后 (schema_version = 2)
{ "schema_version": 2,
  "device_registry": [...], "sync_baselines": [...],
  "tombstones": [...], "conflicts": [...],
  "integrity_summary": {              // ← 唯一新增字段
    "file_sha256": "...", "file_size": 12345,
    "file_mtime_utc": "...", "metadata_root_digest": "...",
    "checked_at_utc": "..." } }
```

**非破坏性**：`device_registry / sync_baselines / tombstones / conflicts` 不动、不重命名、不重新解释。load 时（`SyncMetadata.cpp:247` 之后）v1 库无此 key → 空 struct；save 时（`:286` 之后）追加写入并把版本号升 2。**无任何迁移代码改动旧数据**。外部影响仅限架构 §6.2.3：若有旧版只认 schema 1 的客户端开 v2 库应只读——本仓库单一演进、无遗留客户端，无实际风险。

---

## 文件清单

### 新增（src/core/，全局命名空间、普通 C++ 类、非 QObject，对齐 ConflictResolver/SnapshotService 风格）

| 文件 | 类/内容 |
|---|---|
| `src/core/ExternalChangeDetector.h/.cpp` | 类 `ExternalChangeDetector`；**只读检测**。含 4 个检测子模块为私有方法：FileHashMonitor（摘要+size+mtime）、MetadataConsistencyChecker（metadata_root_digest）、RiskDirectoryDetector（风险目录启发式）、DeepValidationService（条目级+索引一致性）。公开 `detectExternalChange(bool deep)` / `runPreSyncCheck(ExternalChangeReport*)` / `buildRepairPlan(report)`。数据结构 `ExternalChangeReport`、`RepairPlan`、enum `RepairPlanType`、enum `ChangeSeverity` 也在本头文件（对齐 ConflictResolver.h 把 struct 放头文件的惯例）。 |
| `src/core/IntegrityDigest.h/.cpp` | 自由函数 `computeMetadataRootDigest(Database*)`、`computeContentDigest(Database*)`（规范 JSON + 排除 integrity_summary + SHA-256，复用 `CryptoHash::hash(bytes, CryptoHash::Sha256)`，见 `src/crypto/CryptoHash.h:26-48`）。独立成文件便于单测且被 Detector 与 RepairMode 共用。 |

> 说明：FSD 把 RepairModeService 列为独立子模块，但 RepairMode 与检测共享同一组数据结构且职责紧密（plan→execute），且现有 ConflictResolverService 把「构建草稿 + 4 种解决策略」打包在一个类。为减少文件数、对齐既有风格，**`executeRepairPlan` 作为 `ExternalChangeDetector` 的成员方法**（写路径），内部复用 SnapshotService（修复前快照）+ SyncMetadataEngine（索引/基线/分支）。若实现后期该类过大（>600 行），再拆出 `RepairModeService`。

### 修改

| 文件 | 改动 | 锚点 |
|---|---|---|
| `src/core/SyncMetadata.h` | 新增 `struct IntegritySummary{fileSha256, fileSize, qint64, fileMtimeUtc, metadataRootDigest, checkedAtUtc, bool isEmpty; toJson/fromJson}`（持久化记录，对齐 DeviceIdentity/SyncBaseline 的 toJson/fromJson 惯例）；SyncMetadataEngine 增 `m_integritySummary` 成员 + `integritySummary()` / `setIntegritySummary()` / `clearIntegritySummary()`。 | 在 `SyncBaseline` 之后、SyncMetadataEngine 类内 |
| `src/core/SyncMetadata.cpp` | **load**：在 conflicts 解析后（:247）加 `integrity_summary` 读取（v1 缺失→空 struct）。**save**：`m_schemaVersion` 默认改 2；在 conflicts 写入后（:286）加 `integrity_summary` 写入。**迁移**：load 时若 `schema_version==1` 且无 `integrity_summary`，置空 struct（非破坏性）；首次由 Detector 写入后自然升 v2。 | load :247 / save :258,:286 |
| `src/core/SyncEngine.cpp` | `analyzeDiffs()` 起始处（现 :226-230 创建 `sync_pre` 快照**之前**）插入 `runPreSyncCheck`：栈构造 `ExternalChangeDetector detector(local, m_metadataEngine)`；若返回 blocking 级 report，**置 `SyncResult` 为中止**（`success=false` + 填诊断信息，字段名实现时核对 `src/core/SyncData.h` 的 SyncResult 定义），直接 return，不创建快照、不合并。 | :226 之前 |
| `src/core/Database.cpp`（基线刷新钩子，**推荐**） | 在 `save()` 成功路径末尾：若存在 sync 元数据，计算当前 `IntegritySummary`（摘要从内存、file_size/mtime 从写后 `QFileInfo`）并经 SyncMetadataEngine 落盘。**备选**（若不愿耦合 Database）：改为在每个执行 save 的 CLI 命令 + `SyncEngine::applyMerges`（:410-412 saveToDatabase 之后）显式调用 `refreshIntegrityBaseline()`。计划采用推荐方案，实现时二选一。 | save 成功后 |
| `src/cli/DbCheckCommand.h/.cpp`（新） | 只读命令 `db-check`：解锁→`detectExternalChange(--deep)`→打印 report（`--json` 机器可读）→exit code 映射 severity。对齐 `DbCleanupCommand.cpp` 骨架。 | — |
| `src/cli/RepairCommand.h/.cpp`（新） | 写命令 `repair`：`--type rebuild-index|reset-sync-baseline|mark-new-branch`、`--auto`（执行建议 plan）、`--dry-run`（只打印 plan）。执行前 RepairModeService 内部自动建快照。save 回写用 `db->save(Database::Atomic, {}, &error)`（对齐 `SyncCommand.cpp:182-186`）。 | — |
| `src/cli/Command.cpp` | 注册：顶部加 `#include "DbCheckCommand.h"`/`"RepairCommand.h"`；`setupCommands()` 内（:193-195 旁）加两行 `s_commands.insert("db-check", ...)` / `"repair", ...`。 | :46-48, :193-195 |
| `src/CMakeLists.txt` | `core_SOURCES` 集合（:19-107）按**字母序**插入 `core/ExternalChangeDetector.cpp`、`core/IntegrityDigest.cpp`（**只加 .cpp**）。 | :19-107 |
| `tests/CMakeLists.txt` | 加 `add_unit_test(NAME testexternalchangedetector SOURCES TestExternalChangeDetector.cpp LIBS testsupport ${TEST_LIBRARIES})`（:114 之后）。 | :114 之后 |
| `tests/TestCli.cpp` | :251 与 :283 的硬编码命令数 `26` 已陈旧（实际 30），加 db-check+repair 后为 **32**，需更新否则 CLI 注册测试失败。 | :251, :283 |

---

## 分步实现（每步以「可编译 + 可测」收尾，即便一次性交付也按此顺序推进）

**Step 1 — 持久化基线 + 迁移（地基，所有后续依赖）**
- `SyncMetadata.h/.cpp`：加 `IntegritySummary` struct（5 子字段，`toJson/fromJson`）+ `m_integritySummary` 成员 + `integritySummary()/setIntegritySummary()/clearIntegritySummary()`；load（`:247` 后）读取、save（`:286` 后）写入；`m_schemaVersion` 默认 1→2。变更范围与非破坏性说明见上方**决策 D**。
- 单测：JSON 往返；**v1 库（无 integrity_summary）load 后为空、四个旧字段原样保留**；save 后 schema 升 v2 且 `integrity_summary` 存在、旧字段不丢；v2 库 round-trip 不丢字段。

**Step 2 — 摘要计算器**
- `IntegrityDigest.h/.cpp`：`computeMetadataRootDigest` + `computeContentDigest`（规范 JSON、排除 integrity_summary、SHA-256）。
- 单测：确定性（同输入同输出）；输入变化能感知；**确认排除 integrity_summary 自身**（改 integrity_summary 不影响摘要）。

**Step 3 — 检测（只读）**
- `ExternalChangeDetector.h/.cpp`：`ExternalChangeReport`（file_hash_changed/metadata_mismatch/risky_directory_detected/severity/suggested_actions）+ `ChangeSeverity` enum + `detectExternalChange(deep)` + `runPreSyncCheck`。
- 快速层：比对 content_sha256 / metadata_root_digest / file_size / mtime。
- 深度层（`deep=true` 或快速失败时）：条目级状态摘要比对 + tombstone/conflict 索引一致性（复用 SyncMetadataEngine 的 conflicts/tombstones）。
- RiskDirectoryDetector：DB 文件所在目录匹配风险前缀（默认 OneDrive/Dropbox/Google Drive/iCloud，可配置）→ 置 `risky_directory_detected`。
- 单测覆盖 `测试用例大纲` 6.9.1（哈希/时间/大小/根摘要变化可检测）+ 6.9.2（条目级/墓碑索引/冲突索引不一致可识别）。

**Step 4 — 修复计划构建**
- `RepairPlan` struct + `RepairPlanType` enum + `buildRepairPlan(report)`：按 severity/变化类型映射到 REBUILD_INDEX / RESET_SYNC_BASELINE / MARK_NEW_BRANCH，`require_user_confirmation=true`。
- 单测：report→plan 映射表全覆盖。

**Step 5 — 修复执行（写路径）**
- `executeRepairPlan(plan)`：先 `{ SnapshotService ss(m_db); ss.createSnapshot("repair_pre"); }`（对齐 LifecycleManager.cpp:64-67 栈构造复用）；再按 plan_type 执行三种修复；最后 `m_metadataEngine->saveToDatabase()` + 刷新 IntegritySummary。
- 单测覆盖 6.9.3：三种修复各自生效；修复前自动建快照；**修复失败不破坏数据库**（异常路径回滚/不落盘）。

**Step 6 — SyncEngine 同步前阻断**
- `SyncEngine.cpp analyzeDiffs()` 起始插入 `runPreSyncCheck`；blocking 级→中止 SyncResult。
- 单测：主链路 6（替换/篡改 DB 后，analyzeDiffs 返回中止、不创建 sync_pre 快照、不合并）。

**Step 7 — 基线刷新钩子**
- Database.cpp save 成功后刷新 IntegritySummary（或备选拆到各命令）。
- 单测：正常 save 后 `detectExternalChange()` 返回 clean；外部改动后返回 changed。

**Step 8 — CLI 命令**
- `db-check`（只读）+ `repair`（写）；Command.cpp 注册；更新 TestCli 计数 26→32。
- 单测：execCmd 跑 db-check/repair，校验 stdout/exit code（对齐 TestCli harness）。

**Step 9 — CMake 注册 + 全量回归**
- 注册 4 个新 .cpp（2 core + 2 cli 需在 `src/cli/CMakeLists.txt` 或 src/CMakeLists.txt 的 cli 源集登记，实现时核对 cli 源集位置）+ 1 测试目标。
- `cmake --build build --config Release` 无警告；`ctest -C Release` 全绿（含既有 17+14+8 个 phase 测试无回归）。

---

## 复用清单（避免重造）

| 复用项 | 位置 |
|---|---|
| CustomData JSON 读写惯例 | `m_db->metadata()->customData()->value(KEY)` / `->set(KEY, QString::fromUtf8(json))`（SyncMetadata.cpp:200,289） |
| 字节 SHA-256 | `CryptoHash::hash(bytes, CryptoHash::Sha256)`（src/crypto/CryptoHash.h:26-48） |
| VV 递增（MARK_NEW_BRANCH） | `SyncMetadataEngine::incrementEntryCounter`（SyncMetadata.cpp:410-419） |
| 修复前快照 | 栈构造 `SnapshotService ss(m_db); ss.createSnapshot(...)`（LifecycleManager.cpp:64-67） |
| CLI 解锁/保存 | `Utils::unlockDatabase`（Utils.h:52-56）；`db->save(Database::Atomic,{},&err)`（SyncCommand.cpp:182-186） |
| 测试夹具 | 内存库 `makeDb`（TestSyncEngine.cpp:43-58）；文件库副本 `openTestDatabaseCopy`（TestSnapshotService.cpp:88-109，密码 "a"） |
| 时间相关测试 | `MockClock`（tests/mock/MockClock.h:25-48） |

---

## 验证（端到端）

1. **构建**：`cmake --build build --config Release --parallel`，MSVC 无警告。
2. **单元/集成**：`ctest -C Release -R "testexternalchangedetector|testsync|testsnapshot|testlifecycle|testcli"` 全绿。
3. **主链路手测**（CLI）：
   - `keepassxc-cli db-check <db>` → 干净库返回 clean、exit 0。
   - 用外部工具改一条 entry（或 hex 改文件）→ `db-check --deep` 报 `file_hash_changed`/`metadata_mismatch`，exit≠0。
   - `keepassxc-cli repair <db> --auto` → 自动建 `repair_pre` 快照、执行建议修复、save 回写；再 `db-check` 返回 clean。
   - 篡改后 `keepassxc-cli sync --remote <r>` → 被 `runPreSyncCheck` 阻断（中止、不合并）。
4. **schema 迁移手测**：旧 v1 库打开→save→用 `db-check` 或脚本读 `KPXC_SYNC_METADATA` 确认 `schema_version=2` 且含 `integrity_summary`，旧字段无丢失。
5. **回归**：既有 17（conflict）+14（snapshot）+8（lifecycle）+sync 测试全绿，无回归。

---

## 风险与待实现时确认的细节

1. **Schema 升级是数据库结构变更** —— 按 CLAUDE.md 规范需通知用户（本计划 §决策 D 即为通知，含变更前后 JSON）。本次仅新增 1 个 JSON key + 版本号升 2，**纯增量、非破坏性**，旧四字段原样保留；本仓库单一演进、无遗留客户端，无实际风险。
2. **SyncResult 中止字段名** —— 实现时核对 `src/core/SyncData.h` 的 `SyncResult` 定义，确认如何表达「中止 + 诊断信息」（可能需加一个 `aborted`/`message` 字段）。
3. **基线刷新钩子位置** —— 推荐 Database.cpp save 成功路径（集中），备选拆到各 CLI 命令；二选一在 Step 7 定。
4. **cli 源集登记位置** —— 新增 DbCheckCommand/RepairCommand 的 .cpp 登记点需核对 `src/cli/CMakeLists.txt`（或 src/CMakeLists.txt 的 cli 源集）。
5. **RiskDirectoryDetector 启发式** —— V1 用固定风险前缀 + 可配置；非密码学判据，仅提示。
6. **MockClock 与 mtime** —— 文件 mtime 由 OS 控制，MockClock 只控逻辑时钟；mtime 相关测试用「真实改文件」而非 MockClock。

---

## 评审反馈处理（2026-07-26，对应 Gemini/GPT 评审附录 4 个待明确点）

### #1 content_sha256 覆盖盲区 → 已扩展 + 记录残留盲区
`IntegrityDigest::contentDigest` 现纳入：(a) 每条目附件的**内容 SHA-256**（堵「文件名不变换内容」）；(b) 数据库级元数据（`name`、`recycleBinEnabled`）。新增 `testAttachmentContentChangeDetected` / `testDbMetadataChangeDetected` 锁定。
**残留盲区（V1.1）**：KDF 参数微调（如降低 Argon2 迭代数）未纳入摘要——属低概率降级路径，完整覆盖需序列化 KDF 参数，留待后续。

### #2 一致性校验 vs 防篡改 → 已声明边界
`IntegrityDigest.h` 代码注释 + 本节明确：摘要为**无密钥 SHA-256**，与被保护数据同处可编辑 CustomData。可靠抓住「不懂本 schema 的工具/进程动了文件」（SRS 6.1 混用外部同步工具场景）；**抓不住**「存心伪造、会重算摘要的攻击者」。真防篡改需 HMAC/签名，密钥解锁前不可得（鸡生蛋困境），V1 不做。UI 文案不得包装成安全保证。

### #3 检测触发点 → 补 pre-compaction，open-DB 留 GUI
- **同步前**：`SyncEngine::analyzeDiffs` 起始 `runPreSyncCheck`（Medium/High 阻断）✅
- **修复前**：`repair` 命令自身天然覆盖 ✅
- **压缩前**：`DbCleanupCommand` 清理前 `runPreSyncCheck`（Medium/High 阻断）✅（本次新增）
- **打开数据库后**：**延后到 GUI 集成**。CLI 各命令（db-check/repair/sync/db-cleanup）开库后各自已检测；通用「开库即检测」需在 GUI 的 Database 打开流程接入，属 Phase 8 GUI 工作。

### #4 schema_version 升 2 规则 → 已收紧
`SyncMetadataEngine::saveToDatabase` 仅在 **`integrity_summary` 非空时**才把 `schema_version` 升到 2（`if (m_schemaVersion < 2 && !m_integritySummary.isEmpty())`）。避免把「从未检测过、summary 为空」的库误标 v2。`testSchemaMigrationV1ToV2` 已更新验证（空 summary save 后保持 v1，填入非空 summary 后才升 v2）。

### 附：评审 #2（AutoMerge）实现可达性修复
评审正文 #2 指出 AutoMerge（并发不重叠字段自动合并）未实现；远程 `17471bd6` 补了实现，但补 AutoMerge 单测时发现 **`Entry::beginUpdate()` 不复制 `m_customData` 到历史项 → `findCommonAncestor` 在历史里找不到 VV → AutoMerge 实际不可达**（所有并发修改仍升级为 Conflict）。已修复（beginUpdate 补 `m_customData->copyDataFrom`）+ `testAutoMergeNonOverlappingFields` 验证。

