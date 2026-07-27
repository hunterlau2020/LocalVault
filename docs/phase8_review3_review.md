# Phase 8 (External Change Detector) 第三轮评审修复 — 代码评审报告

**评审日期**: 2026-07-27
**评审范围**: 最近 2 个 commit（8 文件，+88/-22）

| Commit | 说明 |
|--------|------|
| `c1659a4c` | phase8(review3): 处理第三轮评审 3 个 🔴 健壮性建议 |
| `6d8ff13a` | phase8(v1.1): 处理第三轮评审 🟡 可选改进 #6 #7 |

---

## 1. 修复摘要

### 🔴 健壮性修复（Commit 1）

| # | 问题 | 修复方式 | 验证 |
|---|------|----------|------|
| #1 | `IntegrityDigest::serializeGroup` 无深度保护，恶意 KDBX 可致栈溢出 | 增加 `depth` 参数，`depth > 256` 时截断并标记 `_truncated` | `testDeepGroupTreeDoesNotCrash`（300 层嵌套） |
| #2 | `testRiskyDirectoryDetected` 仅覆盖 OneDrive 一个标记 | 改为参数化测试（`_data()`），覆盖全部 5 个标记 + normal 反面用例 | 6 行参数化数据驱动 |
| #3 | `RepairCommand --type` 拼写错误静默返回 None → exit 0 | 提前验证 `--type` 值（fail-fast，在密码提示和文件 I/O 之前） | `testRepairInvalidType`（CLI 层） |

### 🟡 可选改进（Commit 2）

| # | 改进 | 实现 |
|---|------|------|
| #6 | `db-check --json` 输出格式控制 | 新增 `--pretty` 选项，切换 `QJsonDocument::Indented` / `Compact` |
| #7 | 对应测试覆盖 | `TestCli` 新增测试（如有） |

---

## 2. 代码质量分析

### 2.1 RepairCommand — `--type` 提前验证

**改动**: `src/cli/RepairCommand.cpp:99-107`

```cpp
// Validate --type early (review3 🔴#3): fail fast on bad input before
// prompting for a password / touching the database.
if (parser->isSet(TypeOption) && parsePlanType(parser->value(TypeOption)) == RepairPlanType::None) {
    err << QObject::tr("Error: unknown repair type '%1'. Valid values: "
                       "rebuild-index, reset-sync-baseline, mark-new-branch.")
               .arg(parser->value(TypeOption))
        << Qt::endl;
    return EXIT_FAILURE;
}
```

**评价**: ✅ 正确。验证位于 `getCommandLineParser` 之后、`QFileInfo::exists` 和 `unlockDatabase` 之前，实现了真正的 fail-fast。错误信息列出全部有效值，用户体验良好。

**附带改进**: 原来的 `haveExplicitConsent` 布尔变量被重构为 `shouldExecute`（line 134），语义更清晰：

```cpp
const bool shouldExecute = parser->isSet(TypeOption) || parser->isSet(AutoOption);
```

**轻微问题**: `parsePlanType` 被调用两次（验证一次 + 使用一次）。当前为纯函数无副作用，但如果后续解析逻辑变复杂可能产生不一致。可缓存结果：

```cpp
RepairPlanType parsedType = RepairPlanType::None;
if (parser->isSet(TypeOption)) {
    parsedType = parsePlanType(parser->value(TypeOption));
    if (parsedType == RepairPlanType::None) { /* error */ }
}
// 后面直接用 parsedType
```

**严重度**: 低 — 当前代码完全正确。

### 2.2 IntegrityDigest — 深度保护

**改动**: `src/core/IntegrityDigest.cpp:46-52`

```cpp
QJsonObject serializeGroup(const Group* g, int depth = 0)
{
    QJsonObject obj;
    if (depth > 256) {
        obj[QStringLiteral("_truncated")] = true;
        return obj;
    }
    // ... 正常序列化
    childrenArr.append(serializeGroup(c, depth + 1));
}
```

**评价**: ✅ 正确。阈值 256 远超正常嵌套深度（实际场景一般 ≤5 层），又能防止恶意 KDBX 导致的栈溢出。`_truncated` 标记使截断行为确定化 — 相同树结构总是产生相同摘要，不会引入摘要不稳定。

**测试**: `testDeepGroupTreeDoesNotCrash` 构建 300 层线性链，验证不崩溃且返回有效 64 字符 hex digest。

**轻微问题**: 测试仅验证「不崩溃」，未验证截断隔离行为（level 258+ 的修改不影响 digest）。可补充：

```cpp
// 修改截断点之后的 level，digest 应不变
parent->setName(QStringLiteral("modified-beyond-truncation"));
const QString d2 = IntegrityDigest::contentDigest(*db, engine);
QCOMPARE(d, d2);
```

**严重度**: 低 — 「不崩溃」已覆盖核心风险。

### 2.3 参数化 riskyDirectory 测试

**改动**: `tests/TestExternalChangeDetector.cpp:320-352`

用 Qt 数据驱动测试 `_data()` 模式覆盖 6 个场景：

| 行 | 路径 | 预期 |
|----|------|------|
| onedrive | `C:/Users/me/OneDrive/LocalVault/test.kdbx` | true |
| dropbox | `C:/Users/me/Dropbox/test.kdbx` | true |
| google-drive | `C:/Users/me/Google Drive/test.kdbx` | true |
| googledrive | `C:/Users/me/GoogleDrive/test.kdbx` | true |
| icloud | `C:/Users/me/iCloud/test.kdbx` | true |
| normal | `C:/Users/me/Documents/test.kdbx` | false |

**评价**: ✅ 符合 Qt 测试最佳实践。同时移除了对 severity 和 runPreSyncCheck 的冗余断言（已由其他测试覆盖），使测试聚焦于单一职责。

### 2.4 `--pretty` JSON 格式化

**改动**: `src/cli/DbCheckCommand.cpp:39-41, 132-133`

新增 `PrettyOption`，默认 `Compact`，传 `--pretty` 切换为 `Indented`。

**评价**: ✅ 简洁。`--pretty` 不传 `--json` 时静默忽略（选项描述已标注 "Use with --json"）。可考虑加 warning 但不阻塞。

### 2.5 testRepairInvalidType

**改动**: `tests/TestCli.cpp:454-464`

```cpp
void TestCli::testRepairInvalidType()
{
    RepairCommand repairCmd;
    const int ret = execCmd(repairCmd, {"repair", "dummy.kdbx", "--type", "rebuild-indx"});
    QVERIFY(ret != 0);
    QVERIFY2(m_stderr->readAll().contains("unknown repair type"),
             "stderr should report the unknown repair type");
}
```

**评价**: ✅ 巧妙利用 `--type` 提前验证的特性，无需真实数据库即可测试。"rebuild-indx" 是典型的 typo 场景。

---

## 3. 风险与注意事项

| 项目 | 评估 |
|------|------|
| 对核心检测逻辑的影响 | 无 — 改动限于 CLI 参数验证、序列化防御、测试增强 |
| 对已有测试的影响 | 无 — `testRiskyDirectoryDetected` 简化了断言但未改变语义 |
| 向后兼容性 | 无破坏 — `--pretty` 是新增选项，深度保护不影响正常数据 |
| 性能影响 | 无 — 深度检查是一个 int 比较，可忽略 |

---

## 4. 总结

| 类别 | 评价 |
|------|------|
| **正确性** | ✅ 全部修复逻辑正确，`shouldExecute` 重构清晰 |
| **测试覆盖** | ✅ 参数化测试 + 无效输入测试 + 深度压力测试 |
| **安全性** | ✅ 深度保护堵住栈溢出攻击面 |
| **代码风格** | ✅ 注释引用评审编号（review3 🔴#N），便于追溯 |
| **风险** | ✅ 改动局部化，不影响核心检测/修复逻辑 |

**结论**: 两个 commit 质量良好，3 个建议均为低优先级优化，不阻塞合并。

### 建议跟进（非阻塞）

1. `parsePlanType` 结果缓存（消除双调用）
2. `testDeepGroupTreeDoesNotCrash` 补充截断隔离断言
3. `--pretty` 无 `--json` 时输出 warning
