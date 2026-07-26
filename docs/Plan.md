# Plan

## 项目名称
基于 KeePassXC 的本地优先条目级同步密码管理器

## 文档目的
本文档用于指导项目启动阶段的开发准备工作，包括：

- 仓库与分支策略
- 本地开发环境准备
- 第一阶段目标
- 代码阅读重点
- 里程碑建议

---

# 1. 启动结论

本项目建议以 **KeePassXC** 为基础进行二次开发，但**不要直接在官方仓库上开发**。  
推荐方式是：

1. 在 GitHub 上 **fork KeePassXC 官方仓库**
2. 在自己的 GitHub 仓库中开展开发
3. 从上游稳定分支创建一个**长期开发分支**
4. 后续每个功能模块再单独创建子分支开发

---

# 2. 仓库策略

## 2.1 推荐仓库方式
推荐使用以下 GitHub 仓库结构：

- `upstream`：官方 KeePassXC 仓库
- `origin`：你自己的 fork 仓库

## 2.2 原因
这样做的好处：

- 方便持续同步上游修复
- 避免直接污染官方主线
- 适合长期维护自己的产品分支
- 便于功能模块独立开发和测试

---

# 3. Git 工作流

## 3.1 Fork 官方仓库
在 GitHub 上 fork `KeePassXC/KeePassXC` 到自己的账号或组织下。

## 3.2 Clone 自己的仓库
```bash
git clone https://github.com/<your-name>/KeePassXC.git
cd KeePassXC
```

## 3.3 添加 upstream
```bash
git remote add upstream https://github.com/KeePassXC/KeePassXC.git
git remote -v
```

## 3.4 同步上游默认分支
先确认官方默认分支名，不要假设一定是 `main`。  
例如若默认分支是 `develop`：

```bash
git fetch upstream
git checkout develop
git pull upstream develop
```

## 3.5 创建长期开发分支
建议创建一个长期分支，例如：

```bash
git checkout -b feature/local-sync-merge
git push -u origin feature/local-sync-merge
```

---

# 4. 分支策略

## 4.1 长期主开发分支
建议使用一个长期开发分支承载整个产品方向：

- `feature/local-sync-merge`

## 4.2 子功能分支
每个功能模块单独开发，完成后合并回长期分支。例如：

- `feat/custom-data-metadata`
- `feat/entry-versioning`
- `feat/entry-diff-engine`
- `feat/version-vector-engine`
- `feat/sync-engine`
- `feat/conflict-resolver`
- `feat/fido2-recovery`
- `feat/lifecycle-manager`

---

# 5. 本地开发环境准备

## 5.1 推荐环境
- Windows 10 / 11
- Visual Studio 2022
- CMake
- Qt（与 MSVC 匹配版本）
- KeePassXC 所需第三方依赖

## 5.2 开发要求
在正式改代码前，必须先完成：

1. 本地完整编译 KeePassXC
2. 能成功启动程序
3. 能创建数据库
4. 能新增/编辑条目
5. 能用 VS2022 断点调试关键流程

---

# 6. 第一阶段目标

项目启动初期，不建议马上做大规模功能改造。  
建议按以下顺序推进。

## 6.1 Phase 1：编译与运行打通
目标：

- 本地可以成功编译 KeePassXC
- 程序可正常运行
- 熟悉项目目录结构
- 跑通创建数据库、保存数据库、编辑条目流程

## 6.2 Phase 2：元数据能力验证
目标：

- 验证 KDBX `Custom Data` 是否可作为同步元数据存储区域
- 初步写入和读取：
  - `schema_version`
  - `device_registry`
  - `snapshot_index`

说明：  
此阶段先不做同步功能，只验证元数据读写能力。

## 6.3 Phase 3：条目版本与 diff
目标：

- 为条目生成完整版本快照
- 对比旧版本和新版本，计算 `changed_fields`
- 维护 `field_version_map`

## 6.4 Phase 4：同步主链路 ✅
目标：

- 手动同步
- 拉取远端数据
- 本地比较版本关系
- 自动 merge 不冲突字段
- 冲突分流到冲突处理模块

### 完成内容（2026-05-14）
- `SyncData.h` — SyncOperation（DirectApply/AutoMerge/Conflict/Skipped）、SyncResult、SyncFieldConflict 数据结构
- `SyncEngine` — `analyzeDiffs()` 差异分析 + `applyMerges()` 自动合并
- `SyncCommand` — CLI `keepassxc-cli sync --remote` 双库同步命令
- 单元测试 4 个用例覆盖：完全相同/仅远端新增/字段冲突/时间戳偏斜自动合并

### 修复 Bug
- `classifyEntry()` 时间戳判定反转：`msecsTo` 正值表示 remote 更新，但原代码取了 local 值
- `applyMerges()` remoteEntries 条件不可靠：`indexEntries` 可能查不到 entry，改用 `findEntryByUuid`

---

# 7. 代码阅读重点

正式开发前，建议优先梳理 KeePassXC 以下部分代码：

## 7.1 数据库打开/保存入口
关注：
- 数据库创建
- 数据库打开
- 数据库保存
- 数据库锁定/解锁

## 7.2 条目编辑保存入口
关注：
- Entry 的创建与修改流程
- 修改后如何落库
- 历史记录如何保存

## 7.3 KDBX 自定义数据能力
重点确认：
- 是否已有 `Custom Data` 相关结构
- 是否适合存储内部同步元数据
- 写入后是否会影响兼容性

## 7.4 历史版本实现
关注：
- KeePassXC 当前如何处理条目历史
- 能否直接扩展为“条目版本链”
- 如何与冲突处理结合

## 7.5 自动锁定逻辑
关注：
- 锁定触发点
- 解锁后的状态恢复
- 是否适合插入 FIDO2 增强解锁逻辑

## 7.6 安全相关逻辑
关注：
- 主密码处理
- 内存清理
- 剪贴板清理
- 密钥文件支持
- 可能插入恢复密钥包的位置

---

# 8. 第一周启动清单

建议第一周完成以下事项：

## Day 1
- Fork 官方仓库
- Clone 到本地
- 配置 upstream
- 熟悉分支结构

## Day 2
- 在本地编译成功
- 启动程序并验证基本功能
- 记录构建问题与依赖项

## Day 3
- 调试数据库创建、打开、保存流程
- 记录关键类和函数入口

## Day 4
- 调试条目新增、编辑、删除流程
- 定位历史记录实现位置

## Day 5
- 验证 KDBX `Custom Data` 可用性
- 设计 `InternalMetadataRoot` 初版结构

## Day 6
- 输出项目源码结构理解文档
- 输出首批技术风险列表

## Day 7
- 创建第一批功能分支
- 准备进入元数据能力开发

---

# 9. 项目初始里程碑建议

## Milestone 1：开发环境打通
交付标准：
- 本地可编译、可运行、可调试

## Milestone 2：内部元数据落地
交付标准：
- `Custom Data` 内可保存并读取同步元数据
- 支持 `schema_version`

## Milestone 3：条目版本化
交付标准：
- 条目修改后可记录完整版本
- 可计算字段变更集合

## Milestone 4：最小可用同步链路 ✅
交付标准：
- 支持手动同步
- 支持条目级 merge
- 支持冲突识别

---

# 10. 完整开发路线图

## Phase 1-7, 11：核心底座 ✅（已完成）

| Phase | 模块 | 交付物 |
|---|---|---|
| **Phase 1** ✅ | 编译与运行打通 | KeePassXC 本地编译、创建/打开/保存数据库 |
| **Phase 2** ✅ | 元数据能力验证 | CLI `db-show-metadata`、Custom Data 持久化 |
| **Phase 3** ✅ | 条目版本与 diff | `EntrySnapshot` / `EntryDiff` 字段级差异计算 |
| **Phase 4** ✅ | 同步主链路 | `SyncEngine` 差异分析+自动合并、CLI `sync` |
| **Phase 5** ✅ | 同步元数据引擎 | 版本向量/设备注册/墓碑/冲突索引 |
| **Phase 6** ✅ | 冲突解决器 | 4 种策略、VV 更新、冲突记录标记，17 测试 |
| **Phase 7** ✅ | 快照与历史管理器 | 快照 CRUD/保护/恢复、历史截断，14 测试 |
| **Phase 11** ✅ | 生命周期管理器 | 按策略清理快照/历史/墓碑，8 测试 |

## Phase 8-10：后续阶段（待启动）

### Phase 6 ✅: Conflict Resolver（冲突解决器）

**职责**：冲突建模、4 种解析策略、版本向量更新、冲突记录标记。

| 项目 | 内容 |
|---|---|
| 文件 | `src/core/ConflictResolver.h/.cpp`（新建）、`src/core/SyncMetadata.h/.cpp`（修改）、`src/core/SyncEngine.h/.cpp`（修改）、`src/cli/SyncCommand.cpp`（修改） |
| 数据结构 | `ConflictItem`(conflictId, entryId, localEntry/remoteEntry clones, conflictingFields, deviceIds), `ConflictResolutionCommand`(resolutionType, mergedFieldValues), `ConflictResolutionResult`(success, conflictMarkedResolved, createdCopyEntryId) |
| 解决策略 | KeepLocal / KeepRemote / ManualMerge / CreateCopy |
| 核心接口 | `resolve(item, command)`, `resolveAll(result, strategy)`, `buildConflictDraft(op)`, `setFieldValue()`, `validateManualMerge()` |
| CLI | `keepassxc-cli sync --remote A B --resolve keep-remote` |
| 测试 | 17 个用例覆盖 4 策略 + VV 更新 + 批处理 + 边界条件，全部通过 ✅ |
| 状态 | **已实现并验证通过** — 编译、链接、运行时全部 17 个测试通过；已有测试无回归 |

### 修复的 Bug

| # | 问题 | 根因 | 修复 |
|---|---|---|---|
| 1 | custom_fields 用错 API | `setDefaultAttribute()` 仅接受标准字段 | 改用 `entry->attributes()->set(key, value)` |
| 2 | VV 被 copyDataFrom 覆写 | KeepRemote 调用 `copyDataFrom` 覆盖 CustomData/VV | `finalizeResolution` 从 `item.localEntry`（克隆）读取 VV |
| 3 | CreateCopy 组放置失效 | `Group::addEntry()` 不设 `Entry::m_group`，`group()` 返回 null | 改用 `findEntryGroup()` 递归搜索组树 |

### Phase 7 ✅: Snapshot & History Manager（快照与历史管理器）

**职责**：数据库级快照与条目级历史记录的创建、恢复、清理。

| 项目 | 内容 |
|---|---|
| 子模块 | SnapshotService, HistoryRetentionService |
| 核心数据 | `SnapshotRecord`(snapshot_id, created_at, reason, file_size, local_path), `HistoryRetentionPolicy` |
| 约束 | 快照存本地专用目录，KDBX 内仅存 snapshot_index；条目历史默认上限 10 条 |
| 核心接口 | `createSnapshot()`, `restoreSnapshot()`, `deleteSnapshot()`, `listSnapshots()`, `enforceEntryHistoryLimit()` |
| 测试 | 14 个用例：JSON 往返 / 文件集成 CRUD / 保护机制 / 恢复快照 / 历史截断 / SyncEngine 集成 |
| CLI | `keepassxc-cli snapshot <db> --create/--list/--delete/--restore/--protect/--unprotect` |

### Phase 8 🔲: External Change Detector（外部变更检测器）

**职责**：外部修改检测与修复模式入口。

| 项目 | 内容 |
|---|---|
| 子模块 | FileHashMonitor, MetadataConsistencyChecker, RiskDirectoryDetector, DeepValidationService, RepairModeService |
| 分层策略 | 快速检测（哈希/大小/时间）→ 深度检测（条目级比对）→ 增量校验 |
| 修复模式 | REBUILD_INDEX / RESET_SYNC_BASELINE / MARK_NEW_BRANCH |
| 核心接口 | `detectExternalChange()`, `buildRepairPlan()`, `executeRepairPlan()` |

### Phase 9 🔲: FIDO2 & Recovery Manager（FIDO2 与恢复管理器）

**职责**：FIDO2 绑定/验证、恢复密钥包生成与恢复流程。

| 项目 | 内容 |
|---|---|
| 子模块 | Fido2RegistrationService, Fido2AssertionService, RecoveryKeyService, RecoveryUnlockService |
| 核心数据 | `Fido2BindingInfo`(credential_id, device_label, bound_at, enabled), `RecoveryKeyEnvelope`(mnemonic 12/24 词) |
| 约束 | 主密码 + FIDO2 AND 模式；恢复密钥包不参与主密钥派生，仅为绕过 FIDO2 的授权令牌 |
| 核心接口 | `startBinding()`, `verifyAssertion()`, `generateRecoveryKey()`, `recoverAccess()` |

### Phase 10 🔲: Remote Storage Adapter（远端存储适配器）

**职责**：统一远端存储访问接口。

| 项目 | 内容 |
|---|---|
| 子模块 | SftpAdapter, WebDavAdapter, CloudApiAdapter, MailApiAdapter, RemoteCredentialVault, TransferResumeService |
| 核心接口 | `testConnection()`, `fetchChanges(cursor)`, `uploadChanges()`, `cancelTransfer()` |
| 约束 | 超 10MB 支持断点续传，默认超时 30s，异步不阻塞 UI |

### Phase 11 ✅: Lifecycle Manager（生命周期管理器）

**职责**：快照、历史记录、墓碑、数据库膨胀数据的生命周期管理。

| 项目 | 内容 |
|---|---|
| 子模块 | LifecycleManager, TombstoneCleanupService |
| 核心数据 | `LifecyclePolicy`(maxSnapshotCount, maxSnapshotDays, entryHistoryLimit, tombstoneCleanupEnabled), `CleanupExecutionReport`(cleaned_snapshots, cleaned_history_records, cleaned_tombstones, reclaimed_bytes) |
| 约束 | 墓碑清理以手动为主；清理前必须自动创建快照；有冲突引用的墓碑不得清理 |
| 核心接口 | `runCleanup()`, `cleanupSnapshots()`, `cleanupEntryHistory()`, `cleanupTombstones()` |
| 复用 | SnapshotService (list/delete) + HistoryRetentionService (enforceEntryHistoryLimit) + SyncMetadataEngine (tombstones) |
| 测试 | 8 个用例：按年龄清理 / 按数量清理 / 保护跳过 / 历史截断 / 墓碑清理 / 冲突引用跳过 / 全流程 / 空数据库不崩溃 |
| CLI | `keepassxc-cli db-cleanup <db> [--max-snapshots N] [--max-days N] [--entry-history-limit N] [--tombstone-cleanup]` |

---

# 11. 当前阶段不要做的事情

为避免项目一开始失控，当前阶段不建议立即做：

- 浏览器插件
- 移动端适配
- 团队共享权限
- 实时同步
- 网盘目录自动同步
- 大规模 UI 重构
- 自研数据库格式

---

# 11. 一句话总结

本项目的正确启动方式是：

**先 fork KeePassXC 到自己的 GitHub 仓库，再基于上游稳定分支创建长期开发分支，从“本地可编译可运行”开始，逐步落地 Custom Data 元数据、条目版本化和条目级本地同步合并。**