# 研发任务拆解版 FSD

## 项目名称
**本地优先的条目级同步密码管理器**

## 文档信息
- 文档类型：Engineering Functional Breakdown
- 版本：v1.1
- 日期：2026-05-11
- 依据文档：
  - SRS v1.3
  - FSD v1.1
  - Architecture.md v1.1

---

# 1. 文档目的

本文档用于把产品与架构需求进一步拆解为：

- 研发模块
- 子模块职责
- 数据结构建议
- 状态机
- 核心接口
- 开发任务清单
- 联调点
- 测试点
- 实现约束

适用于：
- 架构评审
- 研发排期
- 模块分工
- QA 测试设计输入
- 安全评审输入

---

# 2. 总体模块拆分

建议拆成以下 10 个核心模块：

1. **Database Core**
2. **Entry Service**
3. **Sync Metadata Engine**
4. **Sync Engine**
5. **Conflict Resolver**
6. **Snapshot & History Manager**
7. **External Change Detector**
8. **FIDO2 & Recovery Manager**
9. **Remote Storage Adapter**
10. **Lifecycle Manager**

---

# 3. 总体分层建议

## 3.1 分层结构

### 表现层
- Qt UI
- 页面 ViewModel / Presenter

### 应用服务层
- DatabaseAppService
- EntryAppService
- SyncAppService
- SecurityAppService
- LifecycleAppService

### 领域服务层
- EntryMergeService
- ConflictResolutionService
- SnapshotService
- MetadataConsistencyService
- RecoveryKeyService
- EntryDiffService
- LifecyclePolicyService

### 基础设施层
- KDBX Storage Adapter
- KDBX Custom Data Adapter
- FIDO2 Adapter
- SFTP Adapter
- WebDAV Adapter
- Transfer Session Manager
- Secure Memory Utility
- Audit Logger

---

# 4. 模块一：Database Core

## 4.1 职责
负责数据库文件生命周期与基本读写能力：

- 创建数据库
- 打开数据库
- 锁定/解锁数据库
- 保存数据库
- 数据库完整性校验
- KDBX `Custom Data` 内部元数据读写
- KDF / 加密参数管理

---

## 4.2 子模块
- DatabaseFactory
- DatabaseSession
- DatabaseLockService
- DatabaseIntegrityService
- KdbxCustomDataStore
- SecureUnlockCoordinator

---

## 4.3 核心数据结构建议

### DatabaseOpenContext
- database_path
- unlock_mode
- has_key_file
- requires_fido2
- use_recovery_key

### DatabaseSecurityConfig
- kdf_type
- kdf_params
- cipher_type
- key_file_enabled
- fido2_enabled

### InternalMetadataRoot
- schema_version
- format_version
- device_registry
- entry_sync_metadata
- sync_baseline
- tombstone_index
- conflict_index
- snapshot_index
- lifecycle_policy
- integrity_summary

---

## 4.4 元数据存储约束
- 必须存储在 KDBX 的 `Custom Data`
- V1 使用 **JSON** 序列化
- 必须包含 `schema_version`
- 不允许 sidecar 文件承载正式同步元数据

---

## 4.5 核心接口建议

### IDatabaseService
- createDatabase(config)
- openDatabase(openContext)
- lockDatabase()
- unlockDatabase(credentials)
- saveDatabase()
- validateDatabaseIntegrity()
- loadInternalMetadata()
- saveInternalMetadata(metadata)

---

## 4.6 开发任务
1. 实现 KDBX 数据库创建流程
2. 实现数据库解锁流程
3. 实现数据库锁定与自动锁定
4. 实现数据库保存事务
5. 实现 `Custom Data` 元数据容器
6. 实现 JSON 元数据序列化/反序列化
7. 实现 `schema_version` 兼容校验
8. 实现数据库完整性校验
9. 实现数据库损坏异常返回模型
10. 在 `unlockDatabase` 解密完成后，**立即**对主密码 `SecureBuffer` 执行 zeroing

---

## 4.7 联调点
- 与 FIDO2 模块联调解锁流程
- 与 Metadata Engine 联调内部元数据读写
- 与 Snapshot 模块联调保存前/恢复后流程

---

## 4.8 测试点
- 数据库可创建、可打开、可锁定
- `Custom Data` 元数据可持久化
- 未知高版本 `schema_version` 能被正确拦截
- 解锁失败不会污染会话状态
- 保存失败不会破坏原数据库
- 主密码缓冲会在解密后立即清零

---

# 5. 模块二：Entry Service

## 5.1 职责
负责条目 CRUD、历史版本读取、条目恢复。

---

## 5.2 子模块
- EntryRepository
- EntryVersionService
- EntryHistoryService
- EntryRestoreService
- EntryRetentionPolicyService

---

## 5.3 核心数据结构建议

### EntryModel
- entry_id
- title
- username
- password
- url
- notes
- custom_fields
- tags
- deleted_flag

### EntryVersionRecord
- version_id
- entry_id
- parent_version_ids
- created_by_device
- created_at_display
- change_summary
- source_type
- changed_fields
- conflict_origin_ids

### EntryChangeSet
- changed_fields
- old_values
- new_values

---

## 5.4 字段级变更检测实现约束
系统不采用字段增量事件链为主存储模型。  
字段级变更检测采用：

- 保存完整旧版本
- 保存完整新版本
- 通过结构化 diff 生成 `changed_fields`

---

## 5.5 历史保留约束
- 单个条目默认最多保留 **10** 条历史版本
- 超出后优先清理最早的**非冲突版本**
- 冲突源版本优先保留
- 最近版本不可被清理至 0

---

## 5.6 核心接口建议

### IEntryService
- createEntry(data)
- updateEntry(entryId, patch)
- deleteEntry(entryId)
- restoreEntry(entryId)
- getEntry(entryId)
- listEntries(filter)
- getEntryHistory(entryId)
- restoreEntryVersion(entryId, versionId)
- trimEntryHistory(entryId, policy)

---

## 5.7 开发任务
1. 实现条目新增
2. 实现条目编辑与 patch 计算
3. 实现整条目版本快照保存
4. 实现旧版本与新版本的结构化 diff
5. 实现 `changed_fields` 生成
6. 实现逻辑删除（墓碑）
7. 实现条目恢复
8. 实现历史版本记录
9. 实现历史版本恢复生成新版本
10. 实现历史版本上限清理逻辑
11. 实现条目搜索与过滤

---

## 5.8 联调点
- 与 Metadata Engine 联动条目版本更新
- 与 Conflict Resolver 联动冲突后的版本生成
- 与 Lifecycle Manager 联动历史清理

---

## 5.9 测试点
- 条目新增/编辑/删除正常
- 删除生成墓碑
- `changed_fields` 计算准确
- 恢复历史版本生成新版本
- 历史记录不会丢失
- 达到 10 条后能按策略清理最早非冲突版本

---

# 6. 模块三：Sync Metadata Engine

## 6.1 职责
负责同步所需的内部元数据维护：

- 设备 ID 管理
- 条目版本向量
- 字段级版本记录
- 冲突索引
- 墓碑索引
- 同步基线记录

---

## 6.2 子模块
- DeviceRegistryService
- VersionVectorService
- EntryMetadataService
- SyncBaselineService
- ConflictIndexService
- TombstoneIndexService
- MetadataSchemaGuard

---

## 6.3 核心数据结构建议

### DeviceIdentity
- device_id
- device_name
- registered_at
- last_seen_at

### EntrySyncMetadata
- entry_id
- current_version_id
- version_vector
- field_version_map
- last_sync_base
- deleted_flag
- tombstone_version_id
- conflict_state

### field_version_map
- `map<field_name, version_id>`

### SyncBaseline
- remote_id
- last_pulled_cursor
- last_pushed_cursor
- last_success_sync_at
- last_snapshot_id

---

## 6.4 核心接口建议

### ISyncMetadataService
- registerDevice()
- getDeviceIdentity()
- loadEntryMetadata(entryId)
- updateEntryMetadata(entryId, metadata)
- updateVersionVector(entryId, vector)
- updateFieldVersionMap(entryId, map)
- markConflict(entryId, conflictInfo)
- markTombstone(entryId, tombstoneInfo)
- updateSyncBaseline(remoteId, baseline)

---

## 6.5 开发任务
1. 实现设备注册与持久化
2. 实现版本向量读写
3. 实现 `field_version_map`
4. 实现墓碑索引
5. 实现冲突索引
6. 实现同步基线记录
7. 实现 metadata schema 升级/校验机制

---

## 6.6 联调点
- 与 Sync Engine 联调变更判断
- 与 Conflict Resolver 联调冲突标记
- 与 Database Core 联调 `Custom Data` 持久化

---

## 6.7 测试点
- 版本向量可正确保存
- `field_version_map` 可正确更新
- 墓碑状态可同步持久化
- 冲突标记不会丢失
- 同步基线可正确更新

---

# 7. 模块四：Sync Engine

## 7.1 职责
负责完整的条目级本地同步合并流程。

---

## 7.2 子模块
- SyncOrchestrator
- RemoteChangeFetcher
- EntryDiffAnalyzer
- EntryMergeCoordinator
- SyncResultBuilder
- TransferProgressController

---

## 7.3 核心状态机

### SyncJobState
- idle
- precheck
- snapshotting
- fetching_remote
- decrypting_remote
- diffing
- merging
- conflict_detected
- writing_local
- updating_metadata
- completed
- failed
- cancelled

---

## 7.4 核心接口建议

### ISyncService
- runManualSync(remoteId)
- precheckSync()
- fetchRemoteChanges(remoteId, cursor)
- analyzeDiffs(localState, remoteState)
- applyMergePlan(mergePlan)
- finalizeSync(syncResult)

---

## 7.5 Merge 结果结构建议

### MergePlan
- direct_apply_entries
- auto_merge_entries
- conflict_entries
- delete_conflict_entries
- skipped_entries

### SyncResult
- added_count
- updated_count
- deleted_count
- conflict_count
- snapshot_id
- warnings
- errors

---

## 7.6 核心实现约束
- 只在本地完成 merge
- 不允许整库默认覆盖
- 同字段冲突不得自动覆盖
- 同步前必须有数据库级快照
- 远端 `cursor` 格式需支持：
  - `last_sync_timestamp`
  - `version_vector_digest`
  - `device_id`

---

## 7.7 开发任务
1. 实现同步总编排器
2. 实现同步前预检查
3. 实现同步前快照触发
4. 实现远端变更获取
5. 实现本地/远端条目差异���析
6. 实现自动 merge 分支
7. 实现冲突分流
8. 实现删除冲突分流
9. 实现同步结果汇总
10. 实现同步失败回退逻辑
11. 实现传输进度回调
12. 实现用户取消同步
13. 实现大文件断点续传接入

---

## 7.8 联调点
- 与 Remote Storage Adapter 联调远端读取
- 与 Snapshot 模块联调同步前快照
- 与 Conflict Resolver 联调冲突处理
- 与 External Change Detector 联调同步前检查

---

## 7.9 测试点
- 同步流程可完整执行
- 自动 merge 仅发生在允许条件下
- 同字段冲突不会自动覆盖
- 快照失败时同步中止
- 远端异常时本地数据库不损坏
- 大文件同步可显示进度且支持取消

---

# 8. 模块五：Conflict Resolver

## 8.1 职责
负责冲突建模、冲突展示输入数据、冲突结果落库。

---

## 8.2 子模块
- ConflictDetector
- ConflictDraftBuilder
- ConflictResolutionService
- ConflictVersionComposer
- ConflictAuditBridge

---

## 8.3 核心数据结构建议

### ConflictItem
- conflict_id
- entry_id
- local_version_id
- remote_version_id
- conflict_fields
- conflict_type
- local_device_id
- remote_device_id
- status

### ConflictResolutionCommand
- conflict_id
- resolution_type
- merged_payload
- create_copy

### ConflictResolutionResult
- new_version_id
- archived_version_ids
- created_copy_entry_id
- updated_conflict_status

---

## 8.4 核心接口建议

### IConflictService
- detectConflict(localEntry, remoteEntry)
- buildConflictItem(localEntry, remoteEntry)
- resolveConflict(command)
- archiveConflictSourceVersions(conflictId)
- createConflictCopy(entryId, payload)

---

## 8.5 开发任务
1. 实现冲突模型
2. 实现冲突详情数据拼装
3. 实现保留本地
4. 实现保留远端
5. 实现手动合并
6. 实现生成副本
7. 实现冲突解决后的新版本生成规则
8. 实现未采纳版本归档到历史
9. 实现冲突状态持久化
10. 实现冲突审计打点

---

## 8.6 联调点
- 与 UI 联调冲突详情页
- 与 Entry Service 联调新版本写入
- 与 Metadata Engine 联调冲突状态

---

## 8.7 测试点
- 冲突被正确识别
- 各种解决方式均生成正确新版本
- 未采用版本进入历史记录
- 生成副本时新条目 ID 正确

---

# 9. 模块六：Snapshot & History Manager

## 9.1 职责
负责数据库级快照与条目级历史记录的创建、恢复、清理。

---

## 9.2 子模块
- SnapshotService
- SnapshotRestoreService
- HistoryRetentionService
- EntryHistoryRestoreService
- SnapshotStorageService

---

## 9.3 核心数据结构建议

### SnapshotRecord
- snapshot_id
- created_at
- reason
- file_size
- referenced_state
- protected_flag
- local_path

### HistoryRetentionPolicy
- max_snapshot_count
- max_snapshot_days
- keep_latest_restore_point
- manual_cleanup_only
- entry_history_limit

---

## 9.4 快照存储约束
- 快照文件存储于本地专用目录
- KDBX 内部仅保存 `snapshot_index`
- 不允许将快照嵌入当前 KDBX 本体

---

## 9.5 核心接口建议

### ISnapshotService
- createSnapshot(reason)
- listSnapshots()
- restoreSnapshot(snapshotId)
- deleteSnapshot(snapshotId)
- markSnapshotProtected(snapshotId)

### IHistoryService
- appendEntryHistory(entryId, versionRecord)
- listEntryHistory(entryId)
- restoreEntryHistory(entryId, versionId)
- cleanupEntryHistory(entryId, policy)

---

## 9.6 开发任务
1. 实现同步前自动快照
2. 实现手动快照
3. 实现快照恢复
4. 实现恢复前确认机制
5. 实现快照本地专用目录管理
6. 实现条目历史记录追加
7. 实现条目历史恢复
8. 实现快照保护标记
9. 实现历史记录清理接口
10. 实现条目历史 10 条上限配额

---

## 9.7 联调点
- 与 Sync Engine 联调同步前快照
- 与 Lifecycle Manager 联调快照/历史清理
- 与 UI 联调快照列表与恢复

---

## 9.8 测试点
- 快照创建成功
- 快照恢复不损坏数据库
- 条目历史恢复生成新版本
- 被保护快照不能被误删

---

# 10. 模块七：External Change Detector

## 10.1 职责
负责外部修改检测与修复模式入口。

---

## 10.2 子模块
- FileHashMonitor
- MetadataConsistencyChecker
- RiskDirectoryDetector
- DeepValidationService
- RepairModeService

---

## 10.3 核心数据结构建议

### ExternalChangeReport
- file_hash_changed
- metadata_mismatch
- risky_directory_detected
- severity
- suggested_actions

### RepairPlan
- plan_type
- rebuild_metadata_index
- reset_sync_baseline
- create_new_branch
- require_user_confirmation

### RepairPlanType
- REBUILD_INDEX
- RESET_SYNC_BASELINE
- MARK_NEW_BRANCH

---

## 10.4 分层检测策略
### 快速检测
- 文件哈希
- 文件大小
- 文件修改时间
- 内部元数据根摘要

### 深度检测
仅在快速检测失败或用户手动触发时执行：
- 条目级状态快照比对
- 元数据引用完整性检查
- 冲突索引、墓碑索引一致性检查

### 增量校验
- 记录最后一次校验的条目状态摘要
- 下次优先校验发生变化的条目

---

## 10.5 核心接口建议

### IExternalChangeService
- runPreSyncCheck()
- detectExternalChange()
- buildRepairPlan(report)
- executeRepairPlan(plan)

---

## 10.6 `executeRepairPlan` 行为定义
### REBUILD_INDEX
- 重建条目元数据索引
- 重建 tombstone/conflict/snapshot 索引

### RESET_SYNC_BASELINE
- 将当前数据库标记为新的同步基线
- 清理失效 cursor

### MARK_NEW_BRANCH
- 将当前数据库视为新的分支状态
- 保留旧冲突与旧历史为只读参考

---

## 10.7 开发任务
1. 实现数据库哈希监测
2. 实现元数据一致性检查
3. 实现自动同步目录风险检测
4. 实现快速检测
5. 实现深度检测
6. 实现增量校验
7. 实现修复模式入口
8. 实现内部索引重建
9. 实现新基线重建逻辑
10. 实现新分支标记逻辑

---

## 10.8 联调点
- 与 Sync Engine 联调同步前阻断
- 与 Snapshot 模块联调修复前快照
- 与 UI 联调修复模式页面

---

## 10.9 测试点
- 外部替换数据库可被识别
- 检测后能阻断同步
- 修复前自动创建快照
- 修复失败不破坏数据库
- 快速检测与深度检测路径均可运行

---

# 11. 模块八：FIDO2 & Recovery Manager

## 11.1 职责
负责 FIDO2 绑定/验证、恢复密钥包生成与恢复流程。

---

## 11.2 子模块
- Fido2RegistrationService
- Fido2AssertionService
- RecoveryKeyService
- RecoveryUnlockService
- RebindStateService

---

## 11.3 核心状态机

### Fido2BindingState
- disabled
- binding
- bound
- unavailable
- recovery_mode
- rebind_required

---

## 11.4 核心数据结构建议

### Fido2BindingInfo
- credential_id
- device_label
- bound_at
- last_verified_at
- enabled
- platform_binding_type

### RecoveryKeyEnvelope
- recovery_key_id
- mnemonic_word_count
- created_at
- consumed_flag
- recovery_mode_version
- verification_hash

约束：
- `mnemonic_word_count ∈ {12, 24}`
- 默认值：`12`

---

## 11.5 技术边界约束
- 恢复密钥包**不参与数据库主密钥派生**
- 恢复密钥包不是数据库解密密钥
- 恢复密钥包是“绕过缺失 FIDO2 的恢复授权令牌”
- 恢复流程中仍需用户输入主密码并正常解密数据库

---

## 11.6 平台兼容性约束
- Windows 平台：使用 `webauthn.dll`
- 协议模型：CTAP2 / WebAuthn 挑战-应答
- 未来跨平台：通过 `IFido2Adapter` 抽象

---

## 11.7 核心接口建议

### IFido2Service
- startBinding()
- completeBinding(attestation)
- verifyAssertion(assertion)
- removeBinding()
- getBindingStatus()

### IRecoveryKeyService
- generateRecoveryKey()
- displayRecoveryKey()
- validateRecoveryKey(input)
- recoverAccess(input, masterPassword)
- markRecoveryCompleted()

---

## 11.8 开发任务
1. 实现 FIDO2 绑定流程
2. 实现数据库解锁时 FIDO2 验证
3. 实现 Windows WebAuthn API 封装
4. 实现 FIDO2 状态检测
5. 实现恢复密钥包生成
6. 实现恢复密钥包展示确认流程
7. 实现恢复密钥包验证
8. 实现“主密码 + 恢复密钥包”恢复流程
9. 实现恢复成功后的 `rebind_required` 状态
10. 实现重新绑定 FIDO2 状态迁移

---

## 11.9 联调点
- 与 Database Core 联调解锁链路
- 与 UI 联调 FIDO2 弹窗和恢复页
- 与 Secure Memory 工具联调敏感信息清理

---

## 11.10 测试点
- FIDO2 可正常绑定和验证
- FIDO2 不可用时可进入恢复流程
- 恢复密钥包错误无法恢复
- 正确恢复后数据库可由主密码正常解锁
- 恢复成功后状态正确标记为 `rebind_required`

---

# 12. 模块九：Remote Storage Adapter

## 12.1 职责
负责统一远端存储访问接口。

---

## 12.2 子模块
- SftpAdapter
- WebDavAdapter
- CloudApiAdapter
- MailApiAdapter
- RemoteCredentialVault
- TransferResumeService
- ProgressCallbackBridge

---

## 12.3 核心接口建议

### IRemoteStorageAdapter
- testConnection(config)
- fetchChanges(cursor)
- uploadChanges(payload)
- fetchSnapshot(snapshotRef)
- uploadSnapshot(snapshotPayload)
- cancelTransfer(transferId)
- resumeTransfer(transferId)

---

## 12.4 性能与传输约束
- 超过 **10MB** 的文件传输必须支持断点续传
- 同步和快照传输必须支持进度回调
- 默认网络超时 **30 秒**，支持配置
- 传输必须异步，不阻塞 UI 线程

---

## 12.5 开发任务
1. 定义统一远端接口
2. 定义 cursor 结构
3. 实现 SFTP Adapter
4. 实现 WebDAV Adapter
5. 预留 Cloud API Adapter 接口
6. 预留 Mail API Adapter 接口
7. 实现凭证安全存储
8. 实现断点续传
9. 实现进度回调桥接
10. 实现超时与取消控制

---

## 12.6 联调点
- 与 Sync Engine 联调上传下载
- 与 UI 联调同步源配置与测试连接
- 与 Security 模块联调凭证存储

---

## 12.7 测试点
- SFTP / WebDAV 连接可测试
- 上传下载流程可用
- 凭证不会明文泄露
- 大文件支持断点续传
- 进度回调正常
- 适配器失败不会破坏同步主流程状态

---

# 13. 模块十：Lifecycle Manager

## 13.1 职责
负责快照、历史记录、墓碑、数据库膨胀数据的生命周期管理。

---

## 13.2 子模块
- SnapshotCleanupService
- HistoryCleanupService
- TombstoneCleanupService
- DatabaseCompactionService
- OrphanVersionCleanupService

---

## 13.3 核心数据结构建议

### LifecyclePolicy
- snapshot_retention_mode
- max_snapshot_count
- max_snapshot_days
- manual_history_cleanup_only
- manual_tombstone_cleanup_only
- entry_history_limit
- compaction_requires_snapshot

### CleanupExecutionReport
- cleaned_snapshots
- cleaned_history_records
- cleaned_tombstones
- reclaimed_bytes
- warnings
- errors

---

## 13.4 核心接口建议

### ILifecycleService
- getLifecycleOverview()
- cleanupSnapshots(policy)
- cleanupHistory(scope, options)
- cleanupTombstones(scope, options)
- compactDatabase(options)
- cleanupOrphanVersions(options)

### `cleanupHistory(scope, options)` 约束
- `scope = per-entry | global`

说明：
- `per-entry`：清理指定条目历史
- `global`：按全局策略扫描清理

---

## 13.5 墓碑清理策略
V1 中墓碑清理以**手动清理**为主，要求：
- 清理前必须自动创建数据库快照
- 清理后必须做完整性校验
- 有冲突引用或恢复依赖的墓碑不得被直接清理

---

## 13.6 数据库瘦身功能
应支持：
1. 合并历史版本：保留最近 N 个版本
2. 清理孤儿子树：删除无任何引用的版本对象
3. 压缩数据库：触发 KDBX 内部数据紧缩

---

## 13.7 开发任务
1. 实现快照列表与大小统计
2. 实现快照手动删除
3. 实现快照自动清理策略
4. 实现历史记录清理
5. 实现墓碑清理
6. 实现数据库压缩/清理
7. 实现孤儿版本清理
8. 实现清理前自动快照
9. 实现清理结果报告

---

## 13.8 联调点
- 与 Snapshot 模块联调清理前快照
- 与 UI 联调生命周期管理页面
- 与 Database Core 联调压缩后完整性校验

---

## 13.9 测试点
- 快照可按策略清理
- 历史记录可手动清理
- 墓碑可手动清理
- 可执行数据库瘦身
- 压缩失败不损坏数据库
- 清理结果统计正确

---

# 14. 共享基础能力任务

## 14.1 Secure Memory Utility

### 职责
- 敏感字符串缓冲
- secure zeroing
- 防止明文进入日志

### 任务
1. 实现安全缓冲类型
2. 实现显式擦除接口
3. 为密码、恢复密钥包、FIDO2 中间态接入安全缓冲
4. 为 `unlockDatabase`、`recoverAccess` 流程提供即时清零接口

---

## 14.2 Audit Logger

### 职责
- 安全审计记录
- 敏感字段脱敏输出

### AuditEventType
- DATABASE_CREATED
- DATABASE_OPENED
- DATABASE_UNLOCKED
- DATABASE_LOCKED
- ENTRY_CREATED
- ENTRY_UPDATED
- ENTRY_DELETED
- ENTRY_RESTORED
- SYNC_STARTED
- SYNC_COMPLETED
- SYNC_FAILED
- CONFLICT_DETECTED
- CONFLICT_RESOLVED
- SNAPSHOT_CREATED
- SNAPSHOT_RESTORED
- EXTERNAL_CHANGE_DETECTED
- REPAIR_MODE_ENTERED
- FIDO2_BOUND
- FIDO2_VERIFIED
- FIDO2_RECOVERY_STARTED
- FIDO2_RECOVERY_COMPLETED
- LIFECYCLE_CLEANUP_STARTED
- LIFECYCLE_CLEANUP_COMPLETED
- DATABASE_COMPACTED

### 任务
1. 定义审计事件模型
2. 实现敏感信息脱敏
3. 接入数据库创建、同步、冲突、恢复、清理事件

---

# 15. 研发任务 WBS 建议

## 15.1 第一阶段：核心底座
1. Database Core
2. Entry Service
3. Sync Metadata Engine
4. Secure Memory Utility
5. Audit Logger

## 15.2 第二阶段：同步主链路
1. Sync Engine
2. Conflict Resolver
3. Snapshot & History Manager
4. Remote Storage Adapter（SFTP / WebDAV）

## 15.3 第三阶段：安全与异常链路
1. External Change Detector
2. FIDO2 & Recovery Manager
3. 生命周期管理基础能力

## 15.4 第四阶段：体验完善
1. 生命周期页面
2. 冲突详情页
3. 恢复流程优化
4. 错误提示与引导优化
5. 大文件传输体验优化

---

# 16. 关键联调顺序建议
1. 数据库创建/打开/保存
2. `Custom Data` 元数据读写
3. 条目 CRUD + 历史记录
4. 字段 diff 与 `changed_fields`
5. 版本向量与 `field_version_map`
6. 快照创建/恢复
7. 同步预检查
8. 手动同步主链路
9. 冲突处理
10. 删除墓碑处理
11. FIDO2 解锁
12. 恢复密钥包恢复
13. 生命周期清理
14. 审计日志全链路

---

# 17. 开发完成定义（DoD）建议
每个模块完成需满足：

1. 核心功能实现完成
2. 单元测试通过
3. 关键异常路径已覆盖
4. 无明文敏感数据日志输出
5. 与上游/下游模块联调通过
6. 有最小可用 UI 或接口验证入口
7. 文档更新完成

---

# 18. QA 测试输入建议
QA 应至少围绕以下维度设计测试：

- 数据库创建/打开/锁定/恢复
- `Custom Data` 元数据读写与兼容
- 条目 CRUD / 历史 / 删除 / 恢复
- 同步 / 自动 merge / 冲突 / 删除冲突
- 快照 / 回滚 / 条目恢复
- 外部修改检测 / 修复模式
- FIDO2 绑定 / 验证 / 恢复密钥包
- 生命周期清理 / 数据库瘦身
- 大文件传输 / 进度 / 超时 / 续传
- 安全日志 / 敏感信息保护

---
