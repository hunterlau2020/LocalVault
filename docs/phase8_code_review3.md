# Phase 8 第三轮代码评审（commit 0f103f6e..d8565293）

**评审日期**: 2026-07-27
**评审范围**: 最近 2 个 commit（10 文件，+177/-58）
- `0f103f6e` phase8(review2): 修复第二轮代码评审 4 个问题
- `d8565293` test(phase8): 补 review2 #6 测试缺口

## 修复摘要

| # | 严重度 | 问题 | 修复 |
|---|---|---|---|
| #1 | P0 | advisory file_size/mtime 基线每次 save 后必过时（pre-write 读旧文件） → 每次正常打开误报 Low | 去掉 advisory，只留密码学摘要（方案 B） |
| #2 | P0 | `--type` 拼写错误静默成功（返回 None → "No repair needed"）+ `--auto` 标志被忽略 | 无效 `--type` 报错 exit failure；`--auto` 赋予实际语义（仅 `--type`/`--auto` 之一才执行） |
| #3 | P1 | contentDigest 漏 Group 树/entry tags/expires | 加 `serializeGroup` + entry `tags`/`expires`/`expiry_time` |
| #4 | P1 | `db-check --json` 输出不全（仅 5 个 boolean） | 用 QJsonObject 输出完整字段（含 findings/suggested_actions/4 个 digest） |

## 优点

- 每个修复都配了测试（新增 6 个测试，覆盖 6 个行为变更）
- advisory 字段移除彻底（struct/toJson/fromJson/detect/recordBaseline/severity/测试/setup/assert 全部清理，无遗留引用）
- `SyncMetadata.h` 文档注释解释了 *为什么* 移除 advisory（单次 save 内无法自洽）
- `serializeGroup` 封装良好（匿名命名空间、递归清晰、字段顺序稳定）
- `RepairCommand` consent 设计正确（`--dry-run` 优先于 consent 检查；`--type` 无效立即 exit failure）
- JSON 输出 schema 完整（severity + 5 boolean + 4 digest + findings 数组 + suggested_actions 数组），自动化脚本可消费

## 🔴 建议处理（3 个，非阻塞可作 follow-up）

### 1. `testRiskyDirectoryDetected` 只覆盖了 1/5 个 risky 标记
启发式有 5 个标记（`onedrive`/`dropbox`/`google drive`/`googledrive`/`icloud`），单标记测试捕获不到拼写错误或大小写回归（代码用 `.toLower()` 后匹配，但 `.contains()` 对子串匹配，`google drive` 和 `googledrive` 是两个独立检查）。
**建议**：用 `QTest::addColumn<QString>("path")` 参数化覆盖全部 5 个 + 一个负例。

### 2. `serializeGroup` 无递归深度保护
恶意构造的 KDBX 可以创建极深 group 嵌套（如 10000 层），触发栈溢出。KeePassXC 实际场景深度通常 <10，但作为检测器应能处理任意输入。
**建议**：加最大深度参数（如 256），超过则截断或返回占位符（如 `{"_truncated": true}`）。V1.1 改进。

### 3. 缺少 `--type invalid` 的 CLI 行为测试
修复 #2 让 `--type rebuild-indx` 报错退出，但没有 `TestCli` 用例验证（`TestCli` 只测命令注册数量，不测单个命令行为）。这是最容易被回归的一条修复。
**建议**：在 `TestCli` 加 `testRepairInvalidType`，断言 exit=1 + stderr 含 "unknown repair type"。

## 🟡 可选改进（4 个，V1.1）

### 4. contentDigest 里 entry UUID 重复出现
每个 entry UUID 同时出现在 `entries[]` 数组（完整对象）和 `groups.children.entries[]` 数组（仅 UUID）。冗余但不影响正确性（digest 是确定性的）。
**建议**：留作 V1.1 性能优化（如果未来 digest 计算成为热点）。

### 5. `testRepairFailureDoesNotCorrupt` 只测了一种失败模式
当前 `executeRepairPlan` 唯一失败路径是 null engine/db。未来若添加新失败路径（如 repair 中途异常），此测试不会捕获。
**建议**：可接受现状；在 `executeRepairPlan` 添加新失败分支时同步加测试。

### 6. JSON 输出 Compact 模式可读性较差
`--json` 用 `QJsonDocument::Compact` 输出单行，对脚本友好但人工查看费劲。
**建议**：加 `--json-pretty` 选项（用 `QJsonDocument::Indented`），或让 `--json` 默认 Indented、脚本用 `jq` 处理。

### 7. `haveExplicitConsent` 变量名略绕
`--type` 设置时覆盖为 `true` 的逻辑可读性一般。
**建议**：改成 `const bool shouldExecute = parser->isSet(TypeOption) || parser->isSet(AutoOption);` 更直白。

## 🟢 无需改动

- advisory 字段移除彻底且正确
- schema 兼容性：老 v2 DB 的 `file_size`/`file_mtime_utc` 字段被 `fromJson` 默认忽略（QJsonObject 默认忽略未知 key），向前兼容
- JSON key 顺序：Qt6 QJsonObject 保持插入顺序，`toJson` 输出稳定
- 安全：`repair` 现在需要显式 consent 才执行，defense-in-depth 加分

## 测试覆盖

- 新增 6 个测试 + 移除 3 处过时断言 = **净增 3 个测试**
- 全量聚焦 ctest **8/8 通过**
- 覆盖矩阵：digest 字段变化（5 个）、JSON 输出（手动验证）、CLI 参数校验（手动验证）、riskyDirectory（1/5 标记）、修复失败（1 种模式）

## 评审结论

**整体：良好，可合入。** 4 个修复都落地且可测，advisory 字段移除彻底。3 个 🔴 建议是合理的健壮性加固，但非阻塞。**当前状态可合入，3 个 🔴 可作为 follow-up 处理。**
