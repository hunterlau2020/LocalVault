# Phase 10 (Remote Storage Adapter) 代码实现评审报告

本报告根据 [docs/phase10_implementation_review_notice.md](file:///home/debian/LocalVault/docs/phase10_implementation_review_notice.md) 中提出的重点评审要求，对分支 `feature/local-sync-merge` 中 Phase 10 的代码实现（Commit `e24f0c73..d35172cb`）进行了全面评审。

---

## 一、 核心关注点评审结果

### 1. 安全性：命令执行模式与注入防护
* **分析**：在 [RemoteStorageAdapter.cpp:L87-95](file:///home/debian/LocalVault/src/core/RemoteStorageAdapter.cpp#L87-L95) 中，`CommandDelegatingAdapter::runCommand` 使用 `QProcess::splitCommand(cmd)` 将命令字符串拆分为可执行程序（program）和独立参数列表（args），并通过 `m_process->start(program, parts)` 直接调用底层 `execve` / `CreateProcess`。
* **结论**：**绝无默认 Shell 注入风险**。该机制不经过 `/bin/sh` 或 `cmd.exe`，管道、重定向或 `;` 等 Shell 特殊字符不会被解析执行。若用户需使用 Shell 特有特性，须显式配置 `sh -c "..."`。
* **警告机制**：[RemoteCommand.cpp:L96-101](file:///home/debian/LocalVault/src/cli/RemoteCommand.cpp#L96-L101) 在执行 `remote --add` 时，明确向控制台输出了命令安全警示，符合凭证最小化与配置信任边界的要求。

---

### 2. Schema 兼容性（GUI / CLI 共享）
* **分析**：[RemoteStorageAdapter.cpp:L30-54](file:///home/debian/LocalVault/src/core/RemoteStorageAdapter.cpp#L30-L54) 和 [RemoteConfigService.cpp](file:///home/debian/LocalVault/src/core/RemoteConfigService.cpp) 精确地对齐了 `gui/remote/RemoteSettings` 的 JSON 结构：
  - 属性名称完全一致（`downloadCommand`, `downloadCommandInput`, `downloadTimeoutMsec`, `uploadCommand`, `uploadCommandInput`, `uploadTimeoutMsec`）；
  - 超时时间维持毫秒（`msec`）单位；
  - 顶层保持为 JSON Array (`QJsonArray`)。
* **结论**：**完全向下双向兼容**。GUI 与 CLI 读写 `KPXC_REMOTE_SYNC_SETTINGS` Protected CustomData blob 时不会造成任何字段丢失或解析冲突。

---

### 3. SyncCommand 重构与错误处理
* **互斥校验**：[SyncCommand.cpp:L86-95](file:///home/debian/LocalVault/src/cli/SyncCommand.cpp#L86-L95) 严格检查了 `--remote` 与 `--remote-storage` 的互斥关系。
* **临时文件泄漏防护**：[SyncCommand.cpp:L105-114](file:///home/debian/LocalVault/src/cli/SyncCommand.cpp#L105-L114) 引入了 RAII 机制 `TempFileGuard`，无论函数在解密失败、合并冲突或保存中断的哪个分支提前 `return`，生成的远端拉取临时文件都会被自动清理，**零残留**。
* **保存与上传顺序**：[SyncCommand.cpp:L247-255](file:///home/debian/LocalVault/src/cli/SyncCommand.cpp#L247-L255) 确保了只有在本地数据库成功完成落盘落库 `localDb->save()` 之后才会触发 `upload` 回推远端，避免了传回未修改或损坏数据库的风险。

---

### 4. 跨平台路径处理
* **分析**：[RemoteStorageAdapter.cpp:L77-80](file:///home/debian/LocalVault/src/core/RemoteStorageAdapter.cpp#L77-L80) 针对 `{TEMP_DATABASE}` / `{FILE}` 占位符使用了 `QDir::toNativeSeparators(destPath)`。在 Windows 环境下强制转化为反斜杠 `\`，防止 Windows `cmd` 命令将正斜杠 `/` 误判为命令行开关参数。

---

## 二、 单元测试与构建验证

1. **单线程构建**：
   - 包含新增目标 `testremotestorage` 在内的工程在 Linux 环境下单线程构建顺利完成（Exit Code 0）。
2. **测试用例**：
   - 运行 `testremotestorage` 套件，包含 JSON 往返、Fetch/Upload 交互、超时 Killing、标准输入 Piping、配置增删改查等 9 个用例 **100% 通过**。
   - 运行全量单元测试套件（50/50），**100% 全部通过 (Pass)**。

---

## 三、 评审结论

> [!IMPORTANT]
> **总体评估**：**同意合并 / 审核通过 (PASS)**
> Phase 10 的代码实现严谨规范，彻底消除了命令注入与 Schema 兼容隐患，资源释放与错误拦截机制完备，构建与 50 项测试套件全部无回归通过。
