# Phase 8 (External Change Detector) 代码评审报告

本报告对分支 `feature/local-sync-merge` 中关于 **Phase 8 (External Change Detector)** 的最新代码实现（以提交 `bab3a19a` 为基础）进行了深度评审。

---

## 1. 评审对象与代码范围

评审涉及的主要变更集及文件包括：

### 核心实现文件
* [ExternalChangeDetector.h](file:///home/debian/LocalVault/src/core/ExternalChangeDetector.h) 与 [ExternalChangeDetector.cpp](file:///home/debian/LocalVault/src/core/ExternalChangeDetector.cpp)：外部变更检测器类 `ExternalChangeDetector`，定义了只读检测逻辑、危害等级评估、修复计划构建与执行流程。
* [IntegrityDigest.h](file:///home/debian/LocalVault/src/core/IntegrityDigest.h) 与 [IntegrityDigest.cpp](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp#L1)：哈希签名计算逻辑命名空间 `IntegrityDigest`，包含元数据根节点摘要及数据内容的规范化摘要计算。

### 命令行工具扩展
* [DbCheckCommand.h](file:///home/debian/LocalVault/src/cli/DbCheckCommand.h) 与 [DbCheckCommand.cpp](file:///home/debian/LocalVault/src/cli/DbCheckCommand.cpp)：新增的只读检测 CLI 命令 `db-check`。
* [RepairCommand.h](file:///home/debian/LocalVault/src/cli/RepairCommand.h) 与 [RepairCommand.cpp](file:///home/debian/LocalVault/src/cli/RepairCommand.cpp)：新增的自动/手动修复 CLI 命令 `repair`。
* [Command.cpp](file:///home/debian/LocalVault/src/cli/Command.cpp) 与 [DbCleanupCommand.cpp](file:///home/debian/LocalVault/src/cli/DbCleanupCommand.cpp)：在核心 CLI 流程和数据库清理流程中接入检测与阻断机制。

### 测试用例
* [TestExternalChangeDetector.cpp](file:///home/debian/LocalVault/tests/TestExternalChangeDetector.cpp)：检测器与修复器完整单元测试。
* [TestIntegrityDigest.cpp](file:///home/debian/LocalVault/tests/TestIntegrityDigest.cpp)：摘要计算器的多项回归测试与边界测试。
* [TestCli.cpp](file:///home/debian/LocalVault/tests/TestCli.cpp)：集成命令行检测数量的更新测试。

---

## 2. 关键设计实现与验证结果

### A. 解决哈希“自引用震荡”循环 (Decision A)
在 [IntegrityDigest.cpp](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp#L42-L45) 中，`metadataRootDigest` 计算逻辑是对包含 `device_registry`、`sync_baselines`、`tombstones` 等信息的 JSON 进行摘要，该计算依赖的数据集本身**不包含 `integrity_summary` 自身**。
* **验证结果**：[TestIntegrityDigest.cpp:L129-143](file:///home/debian/LocalVault/tests/TestIntegrityDigest.cpp#L129-143) 对此进行了明确测试，修改 `integrity_summary` 不会影响元数据哈希结果。逻辑完全自洽。

### B. 解决 4 项设计评审关注的待明确点 (Review #1–#4)

#### 1. 扩展 `contentDigest` 覆盖范围，消除附件内容被替换等篡改盲区 (Review #1)
* **实现**：[IntegrityDigest.cpp:L81-89](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp#L81-89) 对附件列表进行了 SHA-256 哈希比对；[IntegrityDigest.cpp:L54-57](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp#L54-57) 纳入了数据库级元数据（`name` 和 `recycleBinEnabled`）。
* **验证结果**：[TestIntegrityDigest.cpp:L150-175](file:///home/debian/LocalVault/tests/TestIntegrityDigest.cpp#L150-175) 新增了 `testAttachmentContentChangeDetected` 和 `testDbMetadataChangeDetected` 单元测试，确保附件内容发生改变和数据库改名能够被成功抓取。

#### 2. 安全与防篡改的边界澄清 (Review #2)
* **说明**：[IntegrityDigest.h](file:///home/debian/LocalVault/src/core/IntegrityDigest.h#L33-L42) 的代码注释中显式澄清了机制 of 局限性——摘要为无密钥 SHA-256，与数据保存在一起。它仅用于防止“非 LocalVault 外部同步工具混用”导致的无意修改（符合 SRS 6.1 场景），不防御存心伪造数据的恶意攻击。避免了 UI 层过度承诺。

#### 3. 检测触发点扩展 (Review #3)
* **实现**：
  * **同步前**：在 [SyncEngine.cpp:L322-340](file:///home/debian/LocalVault/src/core/SyncEngine.cpp#L322-340) 的 `SyncEngine::analyzeDiffs` 开头插入了 `runPreSyncCheck`。
  * **压缩前**：在 [DbCleanupCommand.cpp:L118-133](file:///home/debian/LocalVault/src/cli/DbCleanupCommand.cpp#L118-133) 清理任务开始前增加阻断校验。
  * **打开数据库后**：注释中指明该阶段通用触发点有意延后到 GUI Database 打开流程中，避免 CLI 多个只读流程重复阻断。

#### 4. `schema_version` 升级规则收紧 (Review #4)
* **实现**：修改了 [SyncMetadata.cpp:L293-298](file:///home/debian/LocalVault/src/core/SyncMetadata.cpp#L293-298)，仅当 `integrity_summary` 且非空时，才会升级 `schema_version` 至 2。
* **验证结果**：[TestSyncMetadata.cpp:L305-329](file:///home/debian/LocalVault/tests/TestSyncMetadata.cpp#L305-329) 中的 `testSchemaMigrationV1ToV2` 验证了该行为，避免空库或旧客户端无意中被升级标记。

---

## 3. 测试覆盖率与构建状态

* **构建验证**：项目于 Linux 平台 (Qt 6.4.2) 下顺利编译成功（无任何 Warning/Error）。
* **单元测试**：
  * `testintegritydigest` (摘要计算逻辑)：全绿通过。
  * `testexternalchangedetector` (只读检测、修复策略、基线回写)：全绿通过。
  * `testsyncmetadata`、`testsyncengine`：全绿通过。
  * **测试结论**：本次 Phase 8 新增的测试完美覆盖了设计文档中所规划的测试场景，无任何回归错误。

---

## 4. 结论与总结

> [!NOTE]
> **总体评估**：**优秀 (PASS)**
> Phase 8 的 C++ 核心代码架构清晰（采用了与既有 SnapshotService 统一的风格），对先前设计评审中遗留的盲区和缺陷（如附件内容哈希篡改、schema_version 滥升级、清理指令漏检测等）做到了完美修复。代码已具备合入主干（`develop`）的质量标准。
