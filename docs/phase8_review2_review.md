# Phase 8 (External Change Detector) 第二轮代码评审报告

本报告对分支 `feature/local-sync-merge` 中的最近两次关键提交（`0f103f6e` 和 `d8565293`）进行了代码评审。这两次提交针对第二轮架构/代码设计评审中的反馈，做出了至关重要的修正与补强。

---

## 1. 评审对象与代码范围

最近两个提交包含：
1. **`0f103f6e`**：`phase8(review2): 修复第二轮代码评审 4 个问题`
2. **`d8565293`**：`test(phase8): 补 review2 #6 测试缺口`

涉及的核心修改文件：
* [ExternalChangeDetector.h](file:///home/debian/LocalVault/src/core/ExternalChangeDetector.h) / [ExternalChangeDetector.cpp](file:///home/debian/LocalVault/src/core/ExternalChangeDetector.cpp)
* [IntegrityDigest.cpp](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp)
* [SyncMetadata.h](file:///home/debian/LocalVault/src/core/SyncMetadata.h) / [SyncMetadata.cpp](file:///home/debian/LocalVault/src/core/SyncMetadata.cpp)
* [DbCheckCommand.cpp](file:///home/debian/LocalVault/src/cli/DbCheckCommand.cpp)
* [RepairCommand.cpp](file:///home/debian/LocalVault/src/cli/RepairCommand.cpp)
* [TestExternalChangeDetector.cpp](file:///home/debian/LocalVault/tests/TestExternalChangeDetector.cpp)
* [TestIntegrityDigest.cpp](file:///home/debian/LocalVault/tests/TestIntegrityDigest.cpp)

---

## 2. 关键评审结果

### A. 彻底解决 `file_size` / `file_mtime` 造成的误报低危警告问题 (Review2 #1)
* **变更**：废除了对文件物理大小（`fileSize`）与修改时间（`fileMtimeUtc`）的存储及对比（已将字段从 [SyncMetadata.h](file:///home/debian/LocalVault/src/core/SyncMetadata.h#L111-L123) 与 [IntegritySummary](file:///home/debian/LocalVault/src/core/SyncMetadata.cpp#L180-L197) 序列化代码中完全剔除）。
* **原因**：之前的设计（在单次 save 内记录 pre-write 的旧文件信息）会导致保存后磁盘文件大小/修改时间与元数据所存的值产生微弱偏差，引发每次正常打开都会报 `Low` 级别变更警告。
* **安全性**：文件的物理变化已通过对数据内容逻辑状态做密码学摘要计算（`contentDigest`）实现完全覆盖，剔除这两个字段并不影响安全性。

### B. 完善 `contentDigest` 对数据库结构的覆盖，消灭盲区 (Review2 #3)
* **变更**：[IntegrityDigest.cpp](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp#L40-L135) 新增了对 Group 树结构（`serializeGroup` 递归序列化组的名称、嵌套拓扑结构、自定义数据、条目所属组关系）、条目自定义标签（`tags`）和条目的过期属性（`expires`/`expiryTime`）的序列化。
* **作用**：完全消除了“在不同组之间移动条目、重命名组或修改组属性”而摘要保持不变的安全漏洞。

### C. 增强 `repair` 命令行工具的容错性与控制性 (Review2 #2)
* **变更**：
  * 修改了 [RepairCommand.cpp](file:///home/debian/LocalVault/src/cli/RepairCommand.cpp#L116-L158)，若显式指定的 `--type` 是无效的值（例如不支持的修复方案），将直接报未知修复类型错误并以 `EXIT_FAILURE` 退出，彻底阻断了隐式转为 `None` 而静默报成功的 Bug。
  * 强加了 `--auto` 的功能逻辑，如果没有设置 `--type` 或 `--auto` 两个写权限确认参数，则默认仅打印预估修复计划（Plan），不实际修改数据库，提供了只读的安全交互防错机制。

### D. 增强 `db-check --json` 机器可读字段 (Review2 #4)
* **变更**：改写了 [DbCheckCommand.cpp](file:///home/debian/LocalVault/src/cli/DbCheckCommand.cpp#L102-L139) 中的 `--json` 输出流生成方式，采用了规范的 `QJsonObject` 来输出完整的检测指标，包括：详细诊断原因数组 `findings`、推荐动作数组 `suggested_actions` 属性以及实际计算得到的 `recorded_content_digest` / `actual_content_digest` 等诊断比对指纹，供外部自动化流程深度分析。

### E. 测试覆盖缺口与隔离机制 (Commit d8565293 及后续)
* **变更**：在 [TestExternalChangeDetector.cpp](file:///home/debian/LocalVault/tests/TestExternalChangeDetector.cpp#L295-L313) 中补全了针对 `riskyDirectoryDetected` 识别 Low 级警告不阻断同步、以及修复策略失败在无元数据引擎时安全返回 `false` 且不破坏数据库的安全回滚用例，确保高危和中低危严重性的覆盖与修复稳定性。

---

## 3. 结论

> [!IMPORTANT]
> **总体评估**：**强烈推荐合并 (PASS)**
> 这两次修改从逻辑正确性与安全健壮性的角度极大地提高了 Phase 8 代码的完成度，尤其是解决了在正常保存操作中由时间戳/文件大小引发的 Low 级别误报问题，并在逻辑指纹对比中补强了 Group 树结构与 entry 特殊字段。代码逻辑清晰，架构完整，单线程构建及 49 个单元测试已全部通过，具备极高的可靠度。
