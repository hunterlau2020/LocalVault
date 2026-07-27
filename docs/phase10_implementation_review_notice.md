# Phase 10 实现评审通知

**评审对象**：`feature/local-sync-merge` 分支，Phase 10 Remote Storage Adapter（远端存储适配器，方案 B）
**Commit 区间**：`e24f0c73..718bbad0`（5 个 commit）
**设计文档**：`docs/Phase10-RemoteStorageAdapter-设计.md`（已并入设计评审 6 个反馈点）

---

## 一、本次实现内容

打通同步主链路的「远端通道」——让 `sync` 能从任意远端（SFTP/WebDAV/云盘）拉取/回推加密库，使条目级同步真正跨设备可用。

| Step | 文件 | 内容 |
|---|---|---|
| 1 | `src/core/RemoteStorageAdapter.h/.cpp` | `IRemoteStorageAdapter` 接口 + `CommandDelegatingAdapter`（QProcess `splitCommand` 非 shell 执行、`{TEMP_DATABASE}` 占位符转原生分隔符、commandInput 写 stdin、超时 kill、testConnection 检 size>0） |
| 2 | `src/core/RemoteConfigService.h/.cpp` | 读写 `KPXC_REMOTE_SYNC_SETTINGS`（Protected CustomData），顶层 JSON 数组，**schema 对齐 `gui/remote/RemoteSettings`**（GUI/CLI 共享同一 blob） |
| 3 | `src/cli/RemoteCommand.h/.cpp` | `remote --add/--list/--test/--remove` 互斥子命令，`--add` 打印命令注入安全警告 |
| 4 | `src/cli/SyncCommand.cpp` | `--remote-storage <name>`（与 `--remote` 互斥）：fetch→临时文件→既有 sync→save→upload，RAII 自动清理临时文件 |
| 5 | `docs/development-roadmap.md` | Phase 8/10 状态标记 → ✅ |

**新增测试**：`tests/TestRemoteStorageAdapter.cpp`（testremotestorage，9 用例：config 往返、fetch/upload、超时、无效命令、stdin、config-service upsert/get/remove、空库）。全量聚焦 ctest **9/9 (100%)**。

---

## 二、关键设计决策

1. **方案 B（命令委托）而非原生**：不引入 libssh2/libcurl（避免 vcpkg 静态重编 4-8h），用 QProcess 跑用户配置的外部命令。**cloud-agnostic**：rclone（Dropbox/GDrive/OneDrive/S3）、BaiduPCS-Go（百度网盘）、scp、curl 皆可。
2. **零新依赖**：仅用 Qt 标准库（QProcess/AsyncTask）+ 已有 CustomData 基础设施。
3. **SyncEngine 零改动**：传输纯粹是 CLI 层（fetch→临时文件→`unlockDatabase(tempPath)`→既有内存合并→save→upload）。

---

## 三、🔴 请重点评审

### 1. 安全：命令执行模式与注入面
`CommandDelegatingAdapter::runCommand` 用 `QProcess::splitCommand` + `start(program, args)`（**非 shell**）。请确认：
- 是否真的无 shell 注入？`splitCommand` 对带空格/特殊字符路径的拆分是否安全？
- 用户配置的命令本身被信任（存 Protected CustomData，仅解锁后可见）——这个信任边界是否可接受？
- `remote --add` 的安全警告文案是否充分？

### 2. Schema 兼容性（GUI/CLI 共享）
`RemoteStorageConfig` 的 JSON schema **刻意对齐** `gui/remote/RemoteSettings`（字段名、msec 单位、顶层 JSON 数组），使 GUI 和 CLI 读写同一个 `KPXC_REMOTE_SYNC_SETTINGS`。请确认：
- 字段是否完全一致？GUI 写入的配置 CLI 能否正确读取（反之亦然）？
- 是否存在 GUI 用了 CLI 未覆盖的字段导致信息丢失？

### 3. SyncCommand 重构（Step 4）
`--remote-storage` 路径重构了 execute：mutex（#5）、fetch→effectiveRemotePath→unlock、RAII `TempFileGuard`、upload-after-save（#6）。请确认：
- RAII guard 是否覆盖所有 return 路径（临时文件不残留）？
- fetch 失败 / unlock 失败 / save 失败 / upload 失败 的错误处理是否健全（不丢数据、不留半同步状态）？
- `--remote-storage` 与 `--remote` 互斥逻辑是否正确？

### 4. 跨平台路径处理
`{TEMP_DATABASE}` 占位符转 `QDir::toNativeSeparators`（Windows cmd 把 `/` 当开关）。请确认这个处理对 POSIX（cp/scp）无害、对 Windows（cmd/copy）正确。

---

## 四、🟡 已知 follow-up（非阻塞）

- **自动化 `--remote-storage` 端到端测试缺失**：手测已验证（fetch→unlock→analyze 通过），但无 TestCli 双库夹具的自动化用例。核心路径由适配器单测 + 手测覆盖，集成回归测试待补。
- **原生断点续传**：方案 B 不提供；>10MB 续传依赖外部工具（rclone 支持）。若需原生续传，后续走方案 C 的 WebDavAdapter（HTTP Range）。
- **SyncBaseline 激活**：`updateSyncBaseline` 仍是 vestigial（未在 sync 成功后调用），V1 手动同步不强需增量，留 follow-up。

---

## 五、如何验证

```bash
# 构建（单线程）
cmake --build build --config Release --target keepassxc-cli testremotestorage

# 单测（假命令模拟，无需真实网络）
./build/tests/Release/testremotestorage.exe   # 9 用例

# CLI 手测（用本地文件模拟远端）
keepassxc-cli remote local.kdbx --add --name fake \
  --download-cmd 'cmd /c copy "remote.kdbx" "{TEMP_DATABASE}"' \
  --upload-cmd 'cmd /c copy "{TEMP_DATABASE}" "remote.kdbx"'
keepassxc-cli sync local.kdbx --remote-storage fake   # fetch→sync→upload

# 真实云盘（需装 rclone）
keepassxc-cli remote local.kdbx --add --name dropbox \
  --download-cmd 'rclone copyto dropbox:vault/db.kdbx {TEMP_DATABASE}' \
  --upload-cmd 'rclone copyto {TEMP_DATABASE} dropbox:vault/db.kdbx'
```

---

**请专家聚焦：① 安全（QProcess 非 shell 是否真无注入）② Schema 兼容（GUI/CLI 共享是否丢字段）③ SyncCommand 重构的边界与错误处理。** 评审意见请记录到 `docs/phase10_*_review.md`。
