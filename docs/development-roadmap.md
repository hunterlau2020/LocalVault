# Development Roadmap

## 项目名称
**LocalVault — 本地优先条目级同步密码管理器**

## 文档信息
- 文档类型：Development Roadmap & Phase Tracking
- 版本：v1.0
- 日期：2026-05-28
- 关联文档：SRS.md, Architecturev1.1.md, FSD拆解版v1.1.md, Plan.md

---

# 1. 开发阶段总览

## Phase 1-5：核心底座 ✅ 已完成

| Phase | 模块 | 交付物 | 测试 |
|---|---|---|---|
| **Phase 1** ✅ | 编译与运行打通 | KeePassXC 本地编译、创建/打开/保存数据库 | — |
| **Phase 2** ✅ | 元数据能力验证 | CLI `db-show-metadata`、Custom Data 持久化验证 | — |
| **Phase 3** ✅ | 条目版本与 diff | `EntrySnapshot` / `EntryDiff` 字段级差异计算 | 5/5 |
| **Phase 4** ✅ | 同步主链路 | `SyncEngine` 差异分析+自动合并、CLI `sync` 命令 | 5/5 |
| **Phase 5** ✅ | 同步元数据引擎 | 版本向量/设备注册/墓碑/冲突索引、`SyncMetadataEngine` | 7/7 |

## Phase 6-11：后续阶段

| Phase | 模块 | 职责 | 子模块 |
|---|---|---|---|
| **Phase 6** ✅ | **Conflict Resolver** | 冲突建模、保留本地/远端/手动合并/生成副本 | ConflictItem, ConflictResolutionService, resolveKeepLocal/Remote/ManualMerge/CreateCopy |
| **Phase 7** ✅ | **Snapshot & History Manager** | 数据库级快照创建/恢复、条目历史版本管理、清理策略 | SnapshotService, SnapshotRestoreService, HistoryRetentionService |
| **Phase 8** ✅ | **External Change Detector** | 内容/元数据规范摘要检测、风险目录、修复模式、db-check/repair CLI | ExternalChangeDetector, IntegrityDigest, CommandDelegatingAdapter |
| **Phase 9** ⏸ | **FIDO2 & Recovery Manager**（暂缓） | FIDO2 绑定验证、数据库解锁增强、恢复密钥包生成与恢复 | 两轮设计评审（fail-closed/恢复编排/9A-9B拆分），待 webauthn.dll 硬件支持后从 9A 起步 |
| **Phase 10** ✅ | **Remote Storage Adapter** | 命令委托适配器(QProcess 非 shell)、cloud-agnostic(rclone/BaiduPCS-Go/scp/curl)、Protected CustomData 存配置、remote CLI | IRemoteStorageAdapter, CommandDelegatingAdapter, RemoteConfigService |
| **Phase 11** ✅ | **Lifecycle Manager** | 快照/历史/墓碑清理、数据库压缩、孤儿版本清理 | LifecycleManager, TombstoneCleanupService |

---

# 2. 阶段详情

## Phase 1 ✅ — 编译与运行打通

KeePassXC 本地编译，创建/打开/保存数据库，熟悉代码结构。

## Phase 2 ✅ — 元数据能力验证

- CLI 命令 `db-show-metadata` 读写 `metadata->customData()`
- 验证保存→关闭→重开持久化

## Phase 3 ✅ — 条目版本与字段级 diff

- `EntrySnapshot` 捕获全字段值对象
- `EntryDiff` 字段级差异计算（含 old/new 值）
- 5 个测试用例全部通过

## Phase 4 ✅ — 手动同步主链路

- `SyncData.h` — SyncOperation/SyncResult 数据结构
- `SyncEngine` — `analyzeDiffs` 差异分析 + `applyMerges` 自动合并
- `SyncCommand` — CLI `sync` 命令（`--remote` 双库同步）
- 5 个测试用例全部通过

### 已修复 Bug
- classifyEntry 时间戳判定反转 → DirectApply 方向错误
- applyMerges remoteEntries 条件不可靠 → 改用 `findEntryByUuid`

## Phase 5 ✅ — 同步元数据引擎

- `SyncMetadata.h/.cpp` — VersionVector/DeviceIdentity/SyncBaseline/Tombstone/ConflictRecord
- `SyncMetadataEngine` — JSON 序列化（存入 KDBX CustomData）、设备注册、VV 比较/合并
- `SyncEngine` 集成 — `classifyEntry` 优先 VV 判定，回退时间戳；合并后自动推进 VV
- `Config` — 持久化本地设备 ID/名称
- 7 个测试用例全部通过

## Phase 6 ✅ — 冲突解决器

- `ConflictResolver.h/.cpp` — ConflictItem/ConflictResolutionCommand/ConflictResolutionResult + ConflictResolverService
- 4 种策略: KeepLocal/KeepRemote/ManualMerge/CreateCopy
- `SyncMetadata` — 添加 `markConflictResolved()`/`conflictById()`
- `SyncEngine` — 添加 `metadataEngine()` 公有访问器供 ConflictResolverService 使用
- `SyncCommand` — 添加 `--resolve` 选项（keep-local/keep-remote/create-copy）
- `ConflictResolutionCommand` — resolution_type (KeepLocal/KeepRemote/Merge/CreateCopy), merged_payload
- `ConflictResolutionResult` — new_version_id, archived_version_ids
- 联调: Sync Engine（接管冲突分流）、Entry Service（新版本写入）
- 17 个测试用例覆盖全部策略及边界条件，全部通过

### 已修复 Bug
- resolveKeepRemote 中 copyDataFrom 覆写 VV → finalizeResolution 从 item.localEntry 读取 VV
- setFieldValue custom_fields 用错 API → 改用 `attributes()->set()`
- CreateCopy 组放置因 Group::addEntry 不设 m_group → 改用 findEntryGroup 组树搜索

## Phase 7 ✅ — 快照与历史管理器

- `SnapshotService` — 数据库级快照创建/列表/删除/恢复/保护，KDBX CustomData 索引
- `HistoryRetentionService` — 条目历史版本上限强制截断
- `SnapshotRecord` — snapshot_id/created_at/reason/file_size/local_path/protected_
- 约束: 快照存本地专用目录，KDBX 内仅存索引
- 14 个测试用例覆盖 JSON 往返/文件集成/历史清理/路径解析/SyncEngine 集成

## Phase 8 ✅ — 外部变更检测器

- `ExternalChangeReport` — file_hash_changed, metadata_mismatch, risky_directory, severity
- `RepairPlan` — REBUILD_INDEX / RESET_SYNC_BASELINE / MARK_NEW_BRANCH
- 分层策略: 快速检测（哈希/大小/时间）→ 深度检测（条目级比对）

## Phase 9 ⏸ — FIDO2 与恢复管理器（暂缓）

- `Fido2BindingInfo` — credential_id, device_label, bound_at, enabled
- `RecoveryKeyEnvelope` — mnemonic (12/24 词), verification_hash
- 约束: 恢复密钥包不参与主密钥派生，仅为绕过 FIDO2 的授权令牌

## Phase 10 ✅ — 远端存储适配器

- 统一 `IRemoteStorageAdapter` 接口（testConnection / fetchChanges / uploadChanges / cancelTransfer）
- 约束: 超 10MB 支持断点续传，默认超时 30s，异步不阻塞 UI

## Phase 11 ✅ — 生命周期管理器

- `LifecycleManager` — 按策略清理快照/条目历史/墓碑，清理前自动保护快照
- `LifecyclePolicy` — maxSnapshotCount/maxSnapshotDays/entryHistoryLimit/tombstoneCleanupEnabled
- `CleanupExecutionReport` — cleaned_snapshots, cleaned_history_records, reclaimed_bytes
- 8 个测试用例覆盖全部清理策略和边界条件

---

# 3. CLI 命令参考

| 命令 | 功能 |
|---|---|
| `sync --remote <path>` | 手动同步两个数据库 |
| `sync --remote <path> --resolve <strategy>` | 同步后自动解决冲突（keep-local/keep-remote/create-copy） |
| `snapshot <db> --create` | 创建数据库快照 |
| `snapshot <db> --list` | 列出所有快照 |
| `snapshot <db> --delete <id>` | 删除指定快照 |
| `snapshot <db> --restore <id>` | 恢复指定快照（自动创建保护快照） |
| `snapshot <db> --protect <id>` | 标记快照为受保护 |
| `snapshot <db> --unprotect <id>` | 取消快照保护 |
| `db-cleanup <db> [--max-snapshots N] [--max-days N] [--entry-history-limit N] [--tombstone-cleanup]` | 清理快照/历史/墓碑 |
