# Phase 10 Remote Storage Adapter 设计评审报告

**评审日期**: 2026-07-27  
**被评审文档**: [docs/Phase10-RemoteStorageAdapter-设计.md](file:///home/debian/LocalVault/docs/Phase10-RemoteStorageAdapter-设计.md)  
**评审结论**: ✅ **设计方向正确，可进入实现阶段——附若干需在实现前对齐的设计细节**

---

## 1. 总体评价

方案 B（命令委托适配器）选型合理。利用现有 `RemoteHandler` / `RemoteProcess` / `RemoteSettings` 作为参考实现，在核心层构建一个精简、无 GUI 依赖的镜像，工程量小、风险低、天然 cloud-agnostic。5 步分层实现计划节奏合理，每步可独立编译测试，符合本项目的增量迭代风格。

---

## 2. 优点

### 2.1 零依赖前提落实彻底
设计明确排除了 libssh2 / libcurl 引入，仅依赖 `QProcess`（Qt 标准库），与现有代码库依赖图完全对齐，无需触碰 CMake 第三方链接配置。

### 2.2 充分利用现有基础设施
| 设计引用 | 对应现有实现 | 复用评估 |
|---|---|---|
| `KPXC_REMOTE_SYNC_SETTINGS` Protected CustomData | [CustomData.cpp:L29,L199-203](file:///home/debian/LocalVault/src/core/CustomData.cpp#L29) | ✅ 字段已存在且已加密保护 |
| `QProcess` + `{FILE}` 占位 + 超时 + kill | [RemoteHandler.cpp](file:///home/debian/LocalVault/src/gui/remote/RemoteHandler.cpp) | ✅ 逻辑完全一致，可直接移植 |
| 临时文件 `QDir::temp() + uuid` | [RemoteHandler.cpp:L28-33](file:///home/debian/LocalVault/src/gui/remote/RemoteHandler.cpp#L28-33) | ✅ 完全复用 |
| `AsyncTask::runAndWaitForFuture` | [RemoteHandler.cpp:L52,L104](file:///home/debian/LocalVault/src/gui/remote/RemoteHandler.cpp#L52) | ✅ 可复用 |
| `SyncEngine` / `analyzeDiffs` / `applyMerges` | [SyncCommand.cpp:L112-179](file:///home/debian/LocalVault/src/cli/SyncCommand.cpp#L112) | ✅ 零改动，只是加一个 fetch 前导 |

### 2.3 CLI 设计语义明确
`remote add/list/test/remove` 子命令划分清晰，与 `db-check` / `repair` 的风格一致，用户认知负担低。

---

## 3. 🔴 必须在实现前对齐的问题（3 个）

### 3.1 JSON Schema 与现有 `RemoteSettings` 不兼容

**现状**：[RemoteSettings.cpp:L84-99](file:///home/debian/LocalVault/src/gui/remote/RemoteSettings.cpp#L84) 使用的现有 JSON schema 为：
```json
[{"name":"...", "downloadCommand":"...", "downloadCommandInput":"...",
  "downloadTimeoutMsec":30000, "uploadCommand":"...",
  "uploadCommandInput":"...", "uploadTimeoutMsec":30000}]
```

**设计文档**中的 `RemoteStorageConfig` 仅包含：
```json
{"name":"...", "downloadCommand":"...", "uploadCommand":"...",
 "downloadTimeoutSec":30, "uploadTimeoutSec":30}
```

**差异点**：
- 现有 schema 有 `downloadCommandInput` / `uploadCommandInput`（用于向进程 stdin 写入数据，如密码），新设计丢失了这两个字段；
- 现有超时单位为毫秒（`downloadTimeoutMsec`），新设计改为秒（`downloadTimeoutSec`），若两侧读写同一个 CustomData key 会产生数量级解析错误；
- 现有 schema 是顶层 JSON Array，文档描述为 `{"remotes": [...]}` 对象，格式不同。

**建议**：`RemoteStorageConfig` 的 JSON schema 必须与 `RemoteSettings::toConfig()` 完全对齐（保留 `commandInput` / `timeoutMsec` 字段），或者明确声明 `RemoteConfigService` 使用不同的 CustomData key，避免 GUI 与 CLI 互相覆盖彼此的配置。

---

### 3.2 `CommandDelegatingAdapter` 实现应支持 stdin 输入

现有 [RemoteHandler.cpp:L63-67](file:///home/debian/LocalVault/src/gui/remote/RemoteHandler.cpp#L63) 在 `start()` 后会调用：
```cpp
remoteProcess->write(params->downloadInput + "\n");
remoteProcess->waitForBytesWritten();
remoteProcess->closeWriteChannel();
```

这个 `commandInput` 字段是某些工具（如 `sftp` 批量脚本）所必需的。`CommandDelegatingAdapter` 若省略 stdin 支持，将导致部分使用场景（如密码 piping）无法正常工作。

**建议**：`RemoteStorageConfig` 应保留 `downloadInput` / `uploadInput` 字段，在 `CommandDelegatingAdapter::fetch/upload` 中同步实现 stdin 写入逻辑。

---

### 3.3 命令注入风险需要在文档中更显眼的警告

文档第 1 个风险点已提及「信任用户自配」，但 `QProcess` 在执行命令时有两种模式：

- `QProcess::start(QString program, QStringList args)` — **安全**，不经过 shell
- `QProcess::startCommand(QString command)` — 内部会拆分参数，**不经过 shell**，但若拼接不当仍有注入风险

现有 [RemoteProcess.cpp](file:///home/debian/LocalVault/src/gui/remote) 使用的是哪种模式，决定了是否存在 shell 注入风险。设计文档只说「QProcess 跑命令」，实现时必须明确：

**建议**：实现时使用 `QProcess::startCommand(const QString &command)` 或 `start(program, args)` 的拆分模式，并在文档和代码注释中显式说明安全边界。同时，`remote add` 命令应在 CLI 输出中打印一条明确警告。

---

## 4. 🟡 建议优化（3 个，不阻塞实现）

### 4.1 `testConnection` 语义偏弱
当前设计：`testConnection` = 执行一次 `downloadCommand` 到临时文件，成功即认为「连通」。

但若命令执行成功但临时文件为空（如 rclone 网络断开时有时返回 exit 0），会误判为连通。参考 [RemoteHandler.cpp:L78-86](file:///home/debian/LocalVault/src/gui/remote/RemoteHandler.cpp#L78) 已包含文件大小检查（`fileInfo.size() == 0` → 失败）。

**建议**：`CommandDelegatingAdapter::testConnection` 应复用与 `fetch` 相同的成功判定逻辑（exit code == 0 **且** 文件大小 > 0）。

### 4.2 `SyncCommand` 的 `--remote-storage` 与 `--remote` 互斥逻辑
文档说保留 `--remote <本地路径>` 向后兼容，但若用户同时传了 `--remote-storage` 和 `--remote`，行为未定义。

**建议**：实现时加互斥检查，报错退出，而非静默忽略其中一个。

### 4.3 上传时机应在 save 成功之后，而非之前
文档描述的流程为：`fetch → unlock → sync → save → upload`，这是正确顺序。但若实现时误将 upload 放在 save 之前，可能上传旧版文件。

**建议**：在 `SyncCommand` 代码中加注释 `// upload must happen after save()` 作为提醒，防止代码审查遗漏。

---

## 5. 🟢 无需改动

- `IRemoteStorageAdapter` 接口设计简洁（4 个方法），足够覆盖 V1 需求；
- `RemoteConfigService` 的 API 设计（`load/save/get/upsert/remove`）与 `RemoteSettings` 的职责划分合理，核心层无 GUI 依赖；
- `SyncBaseline` 标注为 follow-up 是正确决策，V1 不引入增量同步不必要复杂度；
- 分步实现计划（5 步）层次清晰，每步自测完备。

---

## 6. 结论

> [!IMPORTANT]
> **设计方向正确，推荐实现。实现前必须解决 3 个 🔴 问题：**
> 1. **JSON schema 需与现有 `RemoteSettings` 对齐**，或明确使用独立 key 隔离 GUI/CLI 配置；
> 2. **`CommandDelegatingAdapter` 需支持 stdin（`commandInput`）字段**，否则部分命令工具无法正常使用；
> 3. **明确 `QProcess` 的调用模式**（安全/不安全），在代码中写明安全边界。
