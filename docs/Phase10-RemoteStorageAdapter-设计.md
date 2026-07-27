# Phase 10 设计文档 — Remote Storage Adapter（远端存储适配器，方案 B）

> 本文档为 Phase 10 实现设计，供专家评审。评审通过后再实现。

## Context（为什么做）

Phase 8（外部变更检测）已完成「本地可信」这道关。但当前同步仍是**本地↔本地**（`sync --remote <本地路径>`）。Phase 10 要接上「远端通道」：让同步能从任意远端（SFTP / WebDAV / 云盘 / 邮件）拉取/回推加密库，使条目级同步真正跨设备可用。

**已锁定方案 B**：`IRemoteStorageAdapter` 接口 + **命令委托适配器**（复用 QProcess 跑外部命令的模式），**零新依赖**。命令委托天然 cloud-agnostic（rclone 对接 Dropbox/GDrive/OneDrive/S3；BaiduPCS-Go 对接百度网盘等），取代 FSD「预留」的 CloudApiAdapter。

### 方案 B 的前提与权衡
- **本地需安装对应网盘 CLI**：云盘需 `rclone`/`BaiduPCS-Go`；SFTP 用 `scp`/`sftp`（系统通常自带）；WebDAV 用 `curl`（通常自带）。LocalVault 只存命令串，不内嵌网盘协议。
- **凭证由外部工具管**（rclone 的 OAuth token、scp 的密钥），LocalVault 不存网盘密码。
- **对比方案 A（原生 libssh2/libcurl）**：A 免装外部工具，但要 vcpkg 静态重编 4-8h，且每个云盘后端需写原生 adapter。B 用一个命令委托适配器覆盖所有网盘。

### 关键利好（探索确认）
- **SyncEngine 完全内存化**（`analyzeDiffs`/`applyMerges` 只吃 `QSharedPointer<Database>`，不碰路径）→ 传输纯粹是 CLI 层的事，引擎零改动。
- **`KPXC_REMOTE_SYNC_SETTINGS`** CustomData key 已存在且 **Protected（落盘自动加密）**（`CustomData.cpp:29,199-203`）→ 配置/凭证存储零成本。
- **`AsyncTask::runThenCallback` / `runAndWaitForFuture`** 现成（Phase 8 FileWatcher 已用）→ 异步不阻塞 + 同步等待直接复用。
- 上游 `src/gui/remote/RemoteHandler` + `RemoteProcess`（QProcess + `{TEMP_DATABASE}` 占位 + 超时 + kill）是参考实现（GUI 耦合，CLI 侧写精简核心层版本，不引 gui 依赖）。

---

## 设计

### 接口与适配器（src/core/，非 QObject、全局命名空间，对齐 SnapshotService 风格）

**`IRemoteStorageAdapter`**（抽象接口，FSD §12.3 的 V1 子集）：

> **评审 #1 对齐**：`RemoteStorageConfig` 的 JSON schema **与现有 `gui/remote/RemoteSettings` 完全一致**（字段名、msec 单位、顶层 JSON 数组），使 GUI 与 CLI 共享同一个 `KPXC_REMOTE_SYNC_SETTINGS` blob，互不覆盖。

```cpp
struct RemoteStorageConfig {
    QString name;                   // 远端配置名（如 "my-dropbox"）
    QString downloadCommand;        // 下载命令，{FILE}/{TEMP_DATABASE} 占位符
    QString downloadCommandInput;   // 写入进程 stdin 的内容（评审 #2：sftp 批量/密码 piping 需要）
    int downloadTimeoutMsec = 30000;// 默认 30s（评审 #1：单位 msec，对齐 RemoteSettings）
    QString uploadCommand;
    QString uploadCommandInput;
    int uploadTimeoutMsec = 30000;
    QJsonObject toJson() const;     // 顶层 JSON 数组的一项（非 {remotes:[]} 对象）
    static RemoteStorageConfig fromJson(const QJsonObject&);
};
```

```cpp
class IRemoteStorageAdapter {
public:
    virtual ~IRemoteStorageAdapter() = default;
    virtual bool testConnection(QString* error = nullptr) = 0;
    virtual bool fetch(const QString& localTempPath, QString* error = nullptr) = 0;  // 远端 → 本地临时文件
    virtual bool upload(const QString& localPath, QString* error = nullptr) = 0;     // 本地 → 远端
    virtual void cancel() = 0;                                                        // 取消传输
};
```

**`CommandDelegatingAdapter`**（方案 B 的唯一具体实现）：
- 持有 `RemoteStorageConfig`。
- `fetch`/`upload`：把 `{FILE}`/`{TEMP_DATABASE}` 替换为目标路径；**评审 #2**：`start()` 后若 `*CommandInput` 非空则 `write(input+"\n")` + `closeWriteChannel()`（镜像 `RemoteHandler.cpp:63-67`）。
- **评审 #3（安全）**：用 `QProcess::startCommand(command)`（Qt6，内部按空白拆分 program+args，**不经过 shell**，避免 shell 注入）；`{FILE}` 路径用 `QProcess::splitCommand` 安全引用。代码注释写明「命令经 QProcess 非 shell 执行，但仍信任用户自配命令本身」。`waitForFinished(timeoutMsec)` 控超时，`kill()` 实现 cancel。
- `testConnection`（**评审 #4**）：跑 downloadCommand 到临时文件，成功判定 = `exitCode==0` **且** `QFileInfo(tempPath).size() > 0`（防 rclone 网络断开 exit 0 但空文件误判）。

### 配置存储（复用 Protected CustomData）

**`RemoteConfigService`**（核心层，镜像 `RemoteSettings` 但去 gui）：
- 读写 `KPXC_REMOTE_SYNC_SETTINGS`（CustomData key，已 Protected）。
- JSON：`{ "remotes": [ RemoteStorageConfig, ... ] }`（命名列表，支持多个远端）。
- API：`load(db) → QList<RemoteStorageConfig>`、`save(db, list)`、`get(db, name)`、`upsert(db, config)`、`remove(db, name)`。

### CLI 集成

**新增 `remote` 命令**（`src/cli/RemoteCommand.h/.cpp`，对齐 DbCheckCommand/RepairCommand 骨架）：
- `remote add <db> --name X --download-cmd "..." --upload-cmd "..." [--timeout 30]`：配置/更新一个远端（存入 CustomData）。
- `remote list <db>`：列出已配置远端。
- `remote test <db> --name X`：testConnection（跑下载到临时文件，报成功/失败 + stderr）。
- `remote remove <db> --name X`：删除一个远端配置。

**SyncCommand 扩展**（`src/cli/SyncCommand.cpp`）：
- 新增 `--remote-storage <name>` 选项：从 CustomData 取该远端配置 → `CommandDelegatingAdapter::fetch` 下载到临时 `.kdbx` → 用临时路径走**既有** `unlockDatabase` + `analyzeDiffs` + `applyMerges` 流程 → 本地 `save` 后 `CommandDelegatingAdapter::upload` 回推。
- 保留 `--remote <本地路径>`（向后兼容本地↔本地同步）。
- **评审 #5**：`--remote-storage` 与 `--remote` **互斥**——同时传报错退出（`return EXIT_FAILURE`）。
- **评审 #6**：upload 回推必须在 `localDb->save()` 成功**之后**（代码加注释 `// upload must happen after save()`）。
- 临时文件用 `QDir::temp()` + uuid（参考 `RemoteHandler::getTempFileLocation`），用完清理。

### SyncBaseline 激活（可选 follow-up）

`SyncMetadataEngine::updateSyncBaseline(remoteId, cursor)` 已建模+序列化但**生产里从未调用**（vestigial）。V1 可在 sync 成功后调用它记录 `remoteId`(配置名哈希) + cursor(远端文件 mtime/etag)，为未来增量同步铺路。**本次可先跳过**，标注为 follow-up。

---

## 文件清单

### 新增
| 文件 | 内容 |
|---|---|
| `src/core/RemoteStorageAdapter.h` | `RemoteStorageConfig` struct + `IRemoteStorageAdapter` 抽象接口 + `CommandDelegatingAdapter` 类（声明）。 |
| `src/core/RemoteStorageAdapter.cpp` | `CommandDelegatingAdapter` 实现（QProcess + `{FILE}` 替换 + 超时 + kill）+ `RemoteStorageConfig::toJson/fromJson`。 |
| `src/core/RemoteConfigService.h/.cpp` | 读写 `KPXC_REMOTE_SYNC_SETTINGS` CustomData（命名远端列表）。 |
| `src/cli/RemoteCommand.h/.cpp` | `remote add/list/test/remove` 命令。 |

### 修改
| 文件 | 改动 | 锚点 |
|---|---|---|
| `src/cli/SyncCommand.cpp` | 加 `--remote-storage <name>`；命中则 fetch→临时文件→既有 sync→upload 回推 | `--remote` 解析后（:71）/ save 后（:183） |
| `src/cli/Command.cpp` | 注册 `remote` 命令 | :46-48, :193-195 |
| `src/CMakeLists.txt` | `core_SOURCES` 字母序加 `core/RemoteStorageAdapter.cpp`、`core/RemoteConfigService.cpp` | core_SOURCES |
| `src/cli/CMakeLists.txt` | `cli_SOURCES` 加 `RemoteCommand.cpp` | cli_SOURCES |
| `tests/CMakeLists.txt` | 加 `testremotestorage` 测试目标 | :114 后 |
| `tests/TestCli.cpp` | 命令计数 +1（remote）→ 断言更新 | :251, :283（32→33） |

---

## 分步实现（每步可编译+可测收尾）

1. **接口 + 命令委托适配器**：`RemoteStorageAdapter.h/.cpp`。单测用**假命令**（Windows `cmd /c copy`、POSIX `cp`/`echo`）模拟 download/upload/timeout/cancel，**无需真实网络**。
2. **配置存储**：`RemoteConfigService.h/.cpp`。单测 JSON 往返 + 多远端列表。
3. **`remote` CLI 命令**：add/list/test/remove + 注册。单测 add 后 list 含、remove 后消失。
4. **SyncCommand 接入**：`--remote-storage`。单测假命令 + 两本地库端到端。
5. **CMake + TestCli 计数 + 回归**。

---

## 云盘对接示例（命令委托的天然能力）

`sync --remote-storage my-dropbox` 即可跨设备同步（用户自行安装对应 CLI）：
- **Dropbox**（rclone）：download `rclone cat dropbox:vault/db.kdbx > {FILE}` / upload `rclone copy {FILE} dropbox:vault/db.kdbx`
- **百度网盘**（BaiduPCS-Go）：download `BaiduPCS-Go d /vault/db.kdbx --stdout > {FILE}` / upload `BaiduPCS-Go u {FILE} /vault/db.kdbx`
- **SFTP**：download `scp user@host:/vault/db.kdbx {FILE}` / upload `scp {FILE} user@host:/vault/db.kdbx`
- **WebDAV**（curl）：download `curl -sS -u user:pass -o {FILE} https://host/vault/db.kdbx` / upload `curl -sS -u user:pass -T {FILE} https://host/vault/db.kdbx`

> `{FILE}` 在 download 时 = 下载目标临时文件，upload 时 = 本地保存后的库文件。命令执行/凭证由外部工具负责，LocalVault 只存命令串（CustomData Protected 加密落盘）。

---

## 验证

1. **构建**：`cmake --build build --config Release`（单线程），MSVC 无警告。
2. **单元**：`testremotestorage`（假命令模拟）；`testsync*` 无回归。
3. **CLI 手测**：`remote add` → `remote list`/`test`；`sync --remote-storage`（本地文件模拟远端）端到端。
4. **云盘手测（可选，需装 rclone）**：配 Dropbox，跨两台机验证条目合并。

---

## 风险与边界

1. **命令注入**：用户配置的命令经 QProcess 执行——V1 信任用户自配（命令存 Protected CustomData，仅解锁后可见）。文档应警告「仅配置可信命令」。
2. **凭证归属**：命令委托下凭证由外部工具管（rclone token、scp 密钥）。LocalVault 不存网盘密码——符合「服务端只存密文、凭证最小化」。
3. **断点续传（>10MB）**：方案 B 不提供原生断点续传——大文件续传依赖外部工具自身能力（rclone 支持断点）。TC-RMT-008 由外部工具覆盖。若需原生续传，后续走方案 C 的 WebDavAdapter（HTTP Range）。
4. **进度回调**：QProcess 有 stdout/stderr 信号，可桥接（rclone `--progress`）。V1 先做成功/失败 + stderr 透传，精细进度条留 follow-up。
5. **平台**：Windows `cmd /c`、POSIX `sh -c` 执行命令——跨平台注意（V1 文档给两系示例）。

---

## 评审反馈处理（2026-07-27，对应 phase10_design_review.md）

设计方向已获评审 ✅ 通过。6 个反馈点已并入上文（标注「评审 #N」）：

| # | 类别 | 处理 |
|---|---|---|
| 🔴1 | JSON schema 与 RemoteSettings 冲突 | `RemoteStorageConfig` 对齐 RemoteSettings：保留 `commandInput`、msec 单位、顶层 JSON 数组——GUI/CLI 共享同一 `KPXC_REMOTE_SYNC_SETTINGS` blob |
| 🔴2 | 缺 stdin 支持 | 保留 `downloadCommandInput`/`uploadCommandInput`；fetch/upload 里 `write(input+"\n")`+`closeWriteChannel`（镜像 RemoteHandler） |
| 🔴3 | QProcess 调用模式/注入 | 用 `QProcess::startCommand`（Qt6 非 shell 拆分）；代码注释写安全边界；`remote add` 打印警告 |
| 🟡4 | testConnection 判定弱 | 成功 = `exitCode==0` 且 `QFileInfo.size()>0` |
| 🟡5 | --remote-storage/--remote 互斥 | 同时传报错退出 |
| 🟡6 | upload 须在 save 后 | 流程明确 + 代码注释 `// upload must happen after save()` |

