# Phase 8 (External Change Detector) 第三轮代码评审报告

本报告对分支 `feature/local-sync-merge` 中的最近两个提交（`c1659a4c` 和 `6d8ff13a`）进行了评审。这两个提交专注于解决第三轮评审所提出的 **3 项 🔴 健壮性建议** 以及 **2 项 🟡 可选改进**。

---

## 1. 评审对象与代码范围

最近两个提交包含：
1. **`c1659a4c`**：`phase8(review3): 处理第三轮评审 3 个 🔴 健壮性建议`
2. **`6d8ff13a`**：`phase8(v1.1): 处理第三轮评审 🟡 可选改进 #6 #7`

涉及的核心修改文件：
* [IntegrityDigest.cpp](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp)
* [RepairCommand.cpp](file:///home/debian/LocalVault/src/cli/RepairCommand.cpp)
* [DbCheckCommand.h](file:///home/debian/LocalVault/src/cli/DbCheckCommand.h) / [DbCheckCommand.cpp](file:///home/debian/LocalVault/src/cli/DbCheckCommand.cpp)
* [TestExternalChangeDetector.cpp](file:///home/debian/LocalVault/tests/TestExternalChangeDetector.cpp)
* [TestIntegrityDigest.cpp](file:///home/debian/LocalVault/tests/TestIntegrityDigest.cpp)
* [TestCli.h](file:///home/debian/LocalVault/tests/TestCli.h) / [TestCli.cpp](file:///home/debian/LocalVault/tests/TestCli.cpp)

---

## 2. 关键评审结果

### A. 解决的 3 项 🔴 健壮性建议 (c1659a4c)

#### 1. 递归深度保护防御栈溢出
* **实现**：[IntegrityDigest.cpp:L46-52](file:///home/debian/LocalVault/src/core/IntegrityDigest.cpp#L46-52) 中的递归函数 `serializeGroup` 引入了 `depth` 参数限制。当递归嵌套层数超过 **256** 层时，函数立即截断返回 `{"_truncated": true}`，阻断无限递归。
* **验证**：[TestIntegrityDigest.cpp:L165-177](file:///home/debian/LocalVault/tests/TestIntegrityDigest.cpp#L165-177) 构造了一个包含 **300** 层嵌套的测试组树，成功验证了该防御截断逻辑，保障在极端/恶意构造数据输入下的安全性。

#### 2. 参数化覆盖 Risky Directory 检测
* **实现**：重构了 [TestExternalChangeDetector.cpp:L123-145](file:///home/debian/LocalVault/tests/TestExternalChangeDetector.cpp#L123-145) 的测试用例，采用 Qt 参数化机制对全部 5 种云同步目录（`onedrive`、`dropbox`、`google drive`、`googledrive`、`icloud`）以及 1 个安全负例进行了一次性全量覆盖测试，保证了子串模糊匹配及大小写转换的稳定性。

#### 3. 参数校验前移（Fail-Fast）
* **实现**：[RepairCommand.cpp:L99-106](file:///home/debian/LocalVault/src/cli/RepairCommand.cpp#L99-106) 将 `--type` 修复类型的合法性校验前移至最开头，在触发 `unlockDatabase` 解锁数据库前进行校验。
* **效果**：用户输入无效类型时会立刻 Fail-Fast 并报错退出，无需再被询问/输入主密码。[TestCli.cpp:L2569-2580](file:///home/debian/LocalVault/tests/TestCli.cpp#L2569-2580) 新增了 `testRepairInvalidType` 用例，断言了该阻断报错以及非 0 退出状态码。

---

### B. 解决的 2 项 🟡 可选改进 (6d8ff13a)

#### 4. `db-check` 命令行支持 `--pretty` 缩进输出
* **实现**：[DbCheckCommand.cpp:L130-136](file:///home/debian/LocalVault/src/cli/DbCheckCommand.cpp#L130-136) 配合 `--json` 使用时，支持输出缩进美化后的 JSON（`QJsonDocument::Indented`），默认保持单行紧凑模式（`QJsonDocument::Compact`），方便开发调试。

#### 5. 重构 `RepairCommand` 内部显式授权逻辑
* **实现**：移除了绕弯的覆盖变量设计，采用 `const bool shouldExecute = parser->isSet(TypeOption) || parser->isSet(AutoOption)` 单次判定条件，代码逻辑更为简洁明了。

---

## 3. 结论

> [!IMPORTANT]
> **总体评估**：**强烈推荐合并 (PASS)**
> 最近的 2 个 commit 完成了非常高质量的加固，尤其是防栈溢出的深度截断逻辑，以及免密检验参数的 Fail-Fast 机制，极大地增强了系统的边界抗冲击能力 and CLI 的交互友好度。目前 49 个单元测试在 Linux 环境离屏模式下全部通过，可以安全合入主分支。
