# Architecture.md

## 项目名称
**本地密码管理器**

## 文档信息
- 文档类型：Technical Architecture Specification
- 版本：v1.1
- 日期：2026-05-11
- 关联文档：
  - SRS v1.3
  - FSD v1.1
  - 研发任务拆解版 FSD v1.0

---

# 1. 文档目的

本文档定义系统的整体技术架构，包括：

- 架构目标
- 模块分层
- 模块职责边界
- 数据存储模型
- 同步与冲突处理模型
- 状态流转
- 安全边界
- 生命周期管理机制
- 异常恢复机制
- 技术选型建议

本文档用于：
- 架构评审
- 模块分工
- 联调设计
- 安全审计
- 性能与可维护性评估

---

# 2. 架构目标

系统架构需满足以下目标：

1. **本地优先**
   - 数据库是本地主存储
   - 服务端仅负责存储密文和同步载体

2. **安全优先**
   - 明文仅在本地短暂存在
   - 服务端不具备解密能力
   - 敏感信息在内存中受控

3. **条目级本地同步合并**
   - 同步最小粒度为条目
   - 合并发生在客户端本地
   - 不采用整库覆盖作为默认同步策略

4. **一致性优先于便利性**
   - 冲突必须可见
   - 同字段冲突不得自动覆盖
   - 删除必须采用墓碑机制

5. **可恢复性**
   - 同步前数据库级快照
   - 条目级历史恢复
   - 外部修改检测与修复模式
   - FIDO2 恢复密钥包

6. **可持续运行**
   - 快照、历史、墓碑可清理
   - 数据库膨胀可压缩
   - 生命周期可管理

7. **平台可演进**
   - V1 面向 Windows
   - 为未来 macOS / Linux 扩展预留接口抽象
   - FIDO2、远程存储、生命周期策略可替换实现

---

# 3. 总体架构概览

系统采用**单机富客户端 + 远程被动存储**架构。

## 3.1 逻辑组成
- 本地客户端应用
- 本地加密数据库（KDBX）
- 本地 UI / 业务逻辑 / 安全逻辑 / 同步逻辑
- 远程存储适配层
- 被动远程存储端（SFTP / WebDAV / Cloud / Mail）

## 3.2 关键原则
- **所有合并、冲突判定、恢复决策都在本地完成**
- **远端不参与协调**
- **同步元数据内嵌在 KDBX 内部**
- **快照是数据库级，历史恢复是条目级**
- **恢复密钥包不参与数据库主密钥派生**
- **字段级变更检测采用整条目版本快照与结构化 diff**

---

# 4. 分层架构

系统建议采用 5 层结构。

## 4.1 表现层（Presentation Layer）
职责：
- Qt UI
- 页面状态管理
- 用户输入收集
- 用户提示和冲突展示
- 同步进度与生命周期进度展示

主要组件：
- MainWindow
- EntryListView
- EntryEditorView
- SyncDialog
- ConflictDialog
- SnapshotView
- Fido2Dialog
- RecoveryFlowView
- LifecycleSettingsView

---

## 4.2 应用服务层（Application Layer）
职责：
- 用例编排
- 调用领域服务
- 驱动基础设施层
- 控制事务边界

主要服务：
- DatabaseAppService
- EntryAppService
- SyncAppService
- SecurityAppService
- LifecycleAppService

---

## 4.3 领域服务层（Domain Layer）
职责：
- ���载业务规则
- 封装同步、冲突、历史、生命周期等核心模型
- 不直接依赖 UI

主要服务：
- EntryMergeService
- ConflictResolutionService
- SnapshotService
- MetadataConsistencyService
- ExternalChangeService
- RecoveryKeyService
- LifecyclePolicyService
- EntryDiffService

---

## 4.4 存储与适配层（Infrastructure Layer）
职责：
- KDBX 读写
- KDBX Custom Data 读写
- FIDO2 对接
- SFTP / WebDAV 对接
- 云盘 / 邮件 API 对接
- 安全内存工具
- 审计日志
- 大文件传输控制

主要适配器：
- KdbxStorageAdapter
- KdbxCustomDataAdapter
- Fido2Adapter
- SftpAdapter
- WebDavAdapter
- CloudApiAdapter
- MailApiAdapter
- SecureBufferAdapter
- AuditLogAdapter
- TransferSessionAdapter

---

## 4.5 外部依赖层（External Systems）
- FIDO2 安全密钥
- Windows Hello / Windows WebAuthn Runtime
- SFTP 服务器
- WebDAV 服务
- 云盘 API
- 邮件 API
- 操作系统安全能力（锁屏、凭证、文件权限）

---

# 5. 核心模块划分

## 5.1 Database Core
职责：
- 数据库创建、打开、保存、锁定、解锁
- 数据库内部元数据根对象管理
- 完整性检查

---

## 5.2 Entry Service
职责：
- 条目 CRUD
- 条目历史版本
- 条目恢复
- 逻辑删除

---

## 5.3 Sync Metadata Engine
职责：
- 设备 ID 管理
- 版本向量管理
- 字段变更指纹
- 冲突标记
- 墓碑索引
- 同步基线

---

## 5.4 Sync Engine
职责：
- 同步编排
- 拉取远端
- 解析变更
- 差异分析
- 自动合并
- 冲突分流
- 结果回写

---

## 5.5 Conflict Resolver
职责：
- 冲突建模
- 冲突详情生成
- 冲突解决结果生成新版本
- 冲突副本生成

---

## 5.6 Snapshot & History Manager
职责：
- 快照创建与恢复
- 条目历史记录
- 条目恢复
- 快照保护与清理辅助

---

## 5.7 External Change Detector
职责：
- 外部修改识别
- 内部元数据一致性检查
- 风险目录识别
- 修复模式入口

---

## 5.8 FIDO2 & Recovery Manager
职责：
- FIDO2 绑定与验证
- 数据库解锁增强认证
- 恢复密钥包生成、校验、恢复

---

## 5.9 Remote Storage Adapter
职责：
- 统一远端接口
- 对接 SFTP / WebDAV / Cloud / Mail
- 管理传输进度、断点续传、超时、取消

---

## 5.10 Lifecycle Manager
职责：
- 快照保留策略
- 历史记录清理
- 墓碑清理
- 数据库压缩/瘦身

---

# 6. 数据存储架构

## 6.1 主数据载体
唯一正式主数据载体：
- **KDBX 数据库文件**

KDBX 内部包含两类内容：
1. 用户可见业务数据
2. 系统内部同步元数据

---

## 6.2 内部元数据存储方式
系统内部同步元数据必须存储在 **KDBX 的 Custom Data 区域**，不得通过普通条目（Entry）伪装存储，也不得使用独立 sidecar 文件作为正式存储方式。

### 6.2.1 设计原因
- 同步元数据不应暴露给用户
- 应随数据库整体加密保护
- 应随数据库一起备份、迁移和恢复
- 避免 UI 中被误编辑或误删除

### 6.2.2 序列化格式
V1 中，`InternalMetadataRoot` 采用 **JSON** 作为序列化格式，并必须包含：

- `schema_version`
- `format_version`
- `device_registry`
- `entry_sync_metadata`
- `sync_baseline`
- `tombstone_index`
- `conflict_index`
- `snapshot_index`
- `lifecycle_policy`
- `integrity_summary`

### 6.2.3 向后兼容要求
- 必须使用 `schema_version` 控制元数据结构演进
- 新版本客户端必须能够识别旧版本元数据
- 发现未知高版本 schema 时，应进入只读保护或修复模式，而非静默写回

---

## 6.3 快照存储位置
快照不存储在 KDBX 内部。

V1 中快照采用：
- **本地专用快照目录** 存储完整数据库副本
- KDBX 内部仅保存 `snapshot_index` 元数据索引

### 6.3.1 设计原因
- 快照是整库级副本，不适合嵌入当前数据库本体
- 避免数据库递归膨胀
- 便于独立清理和压缩管理

---

## 6.4 数据分类

### 6.4.1 业务数据
- 条目基础字段
- 分组/标签
- 用户备注
- 自定义字段

### 6.4.2 内部元数据
- 设备注册表
- 条目版本信息
- 版本向量
- 字段版本信息
- 同步基线
- 墓碑索引
- 冲突索引
- 快照索引
- 生命周期策略
- 完整性摘要

---

## 6.5 存储原则
- 不使用 sidecar 文件作为正式同步元数据存储
- 内部元数据必须随数据库一起加密
- 所有恢复、迁移、备份都以 KDBX ��中心
- 快照文件作为数据库级恢复载体，位于受控本地目录

---

# 7. 核心数据模型

## 7.1 条目模型

### Entry
- entry_id
- title
- username
- password
- url
- notes
- custom_fields
- tags
- deleted_flag

---

## 7.2 条目版本模型

### EntryVersion
- version_id
- entry_id
- parent_version_ids
- source_device_id
- version_vector
- changed_fields
- payload_hash
- created_at_display
- source_type
- conflict_origin_ids

说明：
- `created_at_display` 仅用于显示，不参与冲突判定。

---

## 7.3 条目同步元数据模型

### EntrySyncMetadata
- entry_id
- current_version_id
- version_vector
- field_version_map
- deleted_flag
- tombstone_version_id
- conflict_state
- last_sync_base

### field_version_map 结构
定义为：

- `map<field_name, version_id>`

示例字段名：
- `title`
- `username`
- `password`
- `url`
- `notes`
- `custom_fields.<key>`

说明：
- 该结构记录“当前字段最后一次由哪个版本修改”
- 不表示字段值的增量存储链

---

## 7.4 冲突模型

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

---

## 7.5 快照模型

### SnapshotRecord
- snapshot_id
- created_at
- reason
- protected_flag
- referenced_state
- size_bytes
- local_path

---

## 7.6 FIDO2 绑定模型

### Fido2BindingInfo
- credential_id
- device_label
- bound_at
- last_verified_at
- enabled
- platform_binding_type

说明：
- Windows 平台下，`platform_binding_type` 可标识为：
  - `windows_webauthn`
  - `external_usb_key`

---

## 7.7 恢复密钥包模型

### RecoveryKeyEnvelope
- recovery_key_id
- mnemonic_word_count
- created_at
- consumed_flag
- recovery_mode_version
- verification_hash

约束：
- `mnemonic_word_count ∈ {12, 24}`
- V1 默认使用 `12`

---

# 8. 同步架构设计

## 8.1 同步原则
- 同步最小单位是条目
- 合并在本地完成
- 使用版本向量 / 等价机制判定因果关系
- 不以系统时间戳作为冲突判定依据

---

## 8.2 同步主流程

### 同步高层流程
1. 预检查
2. 外部修改检测
3. 创建数据库级快照
4. 拉取远端变更
5. 本地解析远端变更
6. 构建差异分析结果
7. 执行自动合并
8. 生成冲突列表
9. 写回本地数据库
10. 更新内部元数据
11. 返回同步结果

---

## 8.3 字段级变更检测策略
系统不采用字段级增量事件持久化作为主存储模型。  
V1 中字段级变更检测采用 **“整条目快照 + 事后 diff”** 模式。

具体规则：
1. 每次条目修改后，保存完整条目版本；
2. 比较旧版本与新版本时，使用结构化 diff 计算 `changed_fields`；
3. 合并与冲突判定依赖 `changed_fields` 集合；
4. 不依赖增量 patch 链重建完整条目。

优点：
- 实现简单
- 合并稳定
- 恢复容易
- 更适配 KDBX 结构

---

## 8.4 差异分析逻辑
对每个条目判断四类关系：

1. **本地无变化，远端有新版本**
   - 直接应用远端版本

2. **本地与远端为因果关系**
   - 以较新因果版本为准

3. **本地与远端并发修改，修改字段无交集**
   - 自动 merge

4. **本地与远端并发修改，修改字段有交集**
   - 生成冲突

---

## 8.5 删除同步逻辑
删除不是物理删除，而是墓碑版本。

删除场景分为：
1. 本地删除，远端未改
2. 远端删除，本地未改
3. 删除与修改并发冲突
4. 双端同时删除

其中：
- 删除与修改并发冲突必须进入冲突处理流程

---

# 9. 冲突处理架构

## 9.1 冲突判定规则
系统必须使用版本向量或等价机制判断：

- 因果先后
- 并发修改

冲突仅在“并发 + 同字段修改”或“删除与修改并发”时成立。

---

## 9.2 冲突处理模式
用户可选择：
- 保留本地
- 保留远端
- 手动合并
- 生成副本
- 稍后处理

---

## 9.3 冲突解决后版本图
每次冲突解决都必须生成一个**新版本节点**。

规则：
- 新版本成为当前版本
- 未采纳版本进入历史链
- 若生成副本，则副本获得新 `entry_id`
- 原冲突记录保留为审计痕迹

---

# 10. 条目历史与保留策略

## 10.1 历史记录职责
条目历史用于：
- 单条目恢复
- 冲突前版本追踪
- 用户误编辑恢复
- 审计性追溯

---

## 10.2 默认保留上限
V1 中，单个条目历史版本默认最多保留 **10 条**。

达到上限后：
- 优���清理最早的**非冲突版本**
- 冲突源版本优先保留
- 最近版本不得被清理至 0
- 生命周期管理允许用户全局调整策略

---

## 10.3 恢复策略
- 快照恢复：恢复整个数据库状态
- 历史恢复：以历史版本内容生成新的当前条目版本

---

## 10.4 恢复保护
- 所有恢复前应再次创建保护快照
- 恢复失败不得损坏当前数据库

---

# 11. 外部修改检测架构

## 11.1 检测触发点
- 打开数据库后
- 执行同步前
- 执行修复前
- 执行数据库压缩前

---

## 11.2 分层检测策略
外部修改检测采用两层机制：

### 第一层：快速检测
- 文件哈希
- 文件大小
- 文件修改时间
- 内部元数据根摘要

### 第二层：深度检测
在快速检测失败或用户手动触发时执行：
- 条目级状态快照比对
- 元数据与条目引用完整性检查
- 冲突索引、墓碑索引一致性检查

### 增量校验优化
系统可记录上一次校验时的条目状态摘要；下一次深度检测时，仅优先校验已变化条目，以降低性能开销。

---

## 11.3 修复模式
修复模式是受控的恢复流程，支持：
- 重建内部索引
- 重置同步基线
- 标记为新分支
- 重新绑定同步关系

---

# 12. FIDO2 架构设计

## 12.1 设计边界
V1 中 FIDO2 仅用于数据库解锁增强认证，不用于：
- 高风险操作确认
- 服务端认证
- 条目级权限控制

---

## 12.2 平台兼容性说明
Windows 平台中，FIDO2/WebAuthn 支持通过 **`webauthn.dll` 原生 API** 实现。

V1 技术实现：
- Windows：使用系统原生 WebAuthn API
- 设备形态：
  - Windows Hello 平台认证器
  - 独立 USB / NFC / BLE FIDO2 安全密钥

未来跨平台扩展时：
- 需抽象 `IFido2Adapter`
- macOS / Linux 使用独立平台实现，不影响上层业务逻辑

---

## 12.3 FIDO2 解锁协议流程
Windows 平台下，FIDO2 验证通过 `webauthn.dll` 原生 API 实现，采用基于 **CTAP2 / WebAuthn 挑战-应答模型** 的本地验证流程。

解锁流程：
1. 客户端生成本次解锁挑战值；
2. 通过系统 API 请求 FIDO2 设备完成签名；
3. 客户端验证签名结果与绑定凭据；
4. 验证通过后，确认第二因子满足；
5. 用户主密码通过本地 KDF 派生后，用于正常解密数据库。

说明：
- FIDO2 验证成功不等于数据库已解密
- 主密码仍是数据库主密钥派生的核心输入

---

## 12.4 恢复密钥包的技术边界
恢复密钥包**不参与数据库主密钥派生**，也不是数据库解密密钥。

其作用是：
- 在 FIDO2 缺失时，作为恢复授权令牌
- 用于允许系统进入“绕过 FIDO2 校验”的恢复路径

恢复流程：
1. 用户输入主密码；
2. 用户输入恢复密钥包；
3. 系统验证恢复密钥包的合法性（例如校验其哈希/封装）；
4. 验证成功后，允许继续使用主密码正常解密数据库；
5. 恢复成功后，系统状态变为 `rebind_required`。

---

## 12.5 FIDO2 设备丢失后的完整恢复流程
1. 用户在解锁界面选择“使用恢复密钥包”；
2. 输入主密码；
3. 输入恢复密钥包；
4. 系统校验恢复密钥包对应的 `verification_hash`；
5. 校验通过后，允许执行主密码解密流程；
6. 数据库成功解锁后，将安全状态标记为 `rebind_required`；
7. 用户必须在后续安全设置中：
   - 重新绑定新的 FIDO2 设备，或
   - 明确关闭 FIDO2 保护模式。

---

# 13. 生命周期管理架构

## 13.1 生命周期对象
需要管理生命周期的数据包括：
- 快照
- 条目历史记录
- 墓碑
- 冲突记录
- 冗余元数据

---

## 13.2 管理策略

### 快照
- 支持按数量/时间保留
- 保留最近恢复点
- 支持保护标记
- 支持手动删除与自动清理

### 历史记录
- 默认保留
- 单条目默认最多 10 条
- 支持手动清理
- 支持全局清理策略

### 墓碑
- 默认保留
- 初期以手动清理为主
- 清理前必须创建数据库快照

### 数据库瘦身
支持以下主动收缩能力：
- 合并历史版本：保留最近 N 个版本
- 清理孤儿子树：删除没有任何引用的版本对象
- 压缩数据库：触发 KDBX 内部数据紧缩

---

# 14. 关键状态流

## 14.1 数据库状态流
- closed
- opening
- unlocked
- locked
- recovery_mode
- corrupted
- repair_mode

---

## 14.2 同步状态流
- idle
- precheck
- snapshotting
- fetching_remote
- diffing
- merging
- conflict_pending
- finalizing
- completed
- failed

---

## 14.3 FIDO2 状态流
- disabled
- binding
- bound
- verification_required
- unavailable
- recovery_mode
- rebind_required

---

## 14.4 冲突状态流
- detected
- pending_user_action
- resolved_keep_local
- resolved_keep_remote
- resolved_manual_merge
- resolved_copy_created
- archived

---

# 15. 安全边界设计

## 15.1 明文边界
明文仅允许存在于：
- 条目编辑时的本地内存
- 合并时的本地内存
- 解锁后的本地进程内安全缓冲
- 恢复密钥包输入的本地内存

不得存在于：
- 远端存储
- 普通日志
- 调试日志
- 未受控缓存文件

---

## 15.2 敏感对象
需受 Secure Memory 保护的对象：
- 主密码
- 条目密码字段
- 恢复密钥包
- FIDO2 中间态
- 解锁派生中间值

---

## 15.3 解锁时的即时清零要求
在 `unlockDatabase` 流程中，主密码的 `SecureBuffer` 必须在数据库解密完成后**立即执行 zeroing**，不得仅依赖析构函数完成清理。

设计原因：
- 降低进程崩溃或内存转储时残留风险
- 满足高敏感数据最小驻留时间原则

---

## 15.4 审计边界
可记录：
- 操作结果
- 状态变化
- 设备 ID
- 错误码

不可记录：
- 明文密码
- 明文用户名
- 明文 URL
- 恢复密钥包内容

---

# 16. 远程存储架构

## 16.1 远端角色
远端是**被动存储体**，不负责：
- 解密
- 冲突协调
- 合并决策
- 权限判定

---

## 16.2 远端接口抽象
统一抽象为：
- 连接测试
- 变更获取
- 变更上传
- 快照获取
- 快照上传

### 16.2.1 cursor 格式
`fetchChanges(cursor)` 中的 `cursor` 定义为：

```text
cursor = {
  last_sync_timestamp,
  version_vector_digest,
  device_id
}
```

说明：
- `last_sync_timestamp` 仅用于远端筛选候选变更范围
- 真正的因果/并发判定仍以版本向量为准
- `version_vector_digest` 用于快速比对基线状态

---

## 16.3 支持方式
V1：
- SFTP
- WebDAV

预留：
- Cloud API
- Mail API

---

## 16.4 大文件传输性能要求
对于大于 **10MB** 的快照文件传输，系统必须支持：

1. 断点续传；
2. 进度回调；
3. 默认 30 秒网络超时，支持配置；
4. 非阻塞 UI 更新；
5. 用户取消操作后安全中断传输。

---

# 17. 事务与一致性策略

## 17.1 本地事务边界
一次同步事务建议分为：
1. 预检查事务
2. 快照事务
3. 合并事务
4. 元数据更新事务
5. 最终提交事务

---

## 17.2 一致性原则
- 任一阶段失败，不应损坏原数据库
- 快照成功后才能进入变更写入
- 条目写入与元数据写入需保证逻辑一致
- 若元数据更新失败，应进入修复模式而非静默成功

---

# 18. 异常与恢复架构

## 18.1 关键异常类型
- 数据库损坏
- 解锁失败
- FIDO2 不可用
- 恢复密钥包无效
- 外部修改检测触发
- 同步网络失败
- 远端数据无效
- 快照失败
- 压缩失败

---

## 18.2 恢复策略
- 同步异常：保留原数据库，提示重试
- 冲突处理中断：保留 pending ��态
- 修复失败：保留当前状态并提示人工介入
- 恢复失败：回滚到恢复前快照
- 生命周期清理失败：不影响数据库主流程

---

# 19. 技术选型建议

## 19.1 客户端
- **C++**
- **Qt**
- **CMake**
- **MSVC / Visual Studio 2022**

## 19.2 数据库格式
- **KDBX 4.x**

## 19.3 元数据序列化
- **JSON**
- 使用 `schema_version` 进行版本控制

## 19.4 加密
- **Argon2id**
- **AES-256** 或等价等级方案

## 19.5 安全设备
- Windows WebAuthn / FIDO2 原生 API

## 19.6 网络适配
- SFTP 客户端库
- WebDAV / HTTPS 客户端库

---

# 20. 架构风险与控制点

## 20.1 风险：版本向量复杂度高
控制：
- 先支持条目级版本向量
- 字段级仅记录变更集合与字段版本归属，不做 CRDT 全量实现

## 20.2 风险：KDBX 内嵌元数据兼容性
控制：
- 使用 Custom Data
- 使用明确命名空间
- 做 schema version 管理

## 20.3 风险：数据库膨胀
控制：
- 生命周期管理
- 手动压缩
- 快照清理策略
- 条目历史保留上限

## 20.4 风险：恢复密钥包被误存在线上
控制：
- 强提示
- 不默认导出到同步目录
- 审计记录提示用户行为

## 20.5 风险：用户混用外部同步工具
控制：
- 风险目录检测
- 外部修改检测
- 修复模式阻断

## 20.6 风险：大数据库与大快照性能退化
表现：
- 快照上传/下载耗时长
- 深度校验耗时长
- 生命周期清理阻塞 UI

控制措施：
- 大文件断点续传
- 深度检测按需触发
- 增量校验
- 异步后台任务
- 进度回调与取消能力

---

# 21. 结论

本系统架构的核心是：

> **以 KDBX 为唯一正式主数据载体，在客户端本地完成条目级同步合并、冲突处理、恢复与生命周期管理；远端仅作为密文与同步载体的被动存储。**

其关键架构决策包括：

1. **条目级本地同步合并**
2. **KDBX Custom Data 内嵌同步元数据**
3. **InternalMetadataRoot 使用 JSON + schema_version**
4. **字段级变更检测采用整条目快照 + diff**
5. **版本向量判定并发**
6. **同字段冲突必须人工处理**
7. **快照恢复与条目历史恢复分离**
8. **FIDO2 仅用于数据库解锁增强认证**
9. **恢复密钥包作为 FIDO2 故障恢复授权令牌**
10. **生命周期管理作为长期可用性保障**
11. **大文件传输支持断点续传与进度回调**