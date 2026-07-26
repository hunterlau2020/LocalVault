# LocalVault 用户手册

## 1. 配置文件

所有配置以 **INI 纯文本文件** 存储，不使用注册表。

### 1.1 文件位置

| 模式 | 主配置文件 | 本地状态文件 |
|---|---|---|
| **正常模式** | `%APPDATA%\KeePassXC\keepassxc.ini` | `%LOCALAPPDATA%\KeePassXC\keepassxc.ini` |
| **便携模式** | `<exe目录>\keepassxc.ini` | `<exe目录>\keepassxc_local.ini` |
| **Debug 构建** | `keepassxc_debug.ini` | `keepassxc_debug.ini` |

正常模式下完整路径示例：
- `C:\Users\<用户名>\AppData\Roaming\KeePassXC\keepassxc.ini` — 主配置
- `C:\Users\<用户名>\AppData\Local\KeePassXC\keepassxc.ini` — 本地状态

### 1.2 两个文件的区别

| 文件 | 内容 |
|---|---|
| `keepassxc.ini` | 主配置：语言、主题、加密设置、快捷键、自动锁定策略等 |
| `keepassxc_local.ini` | 本地状态：窗口位置/大小、最近打开的文件列表、本设备 ID 等 |

### 1.3 便携模式

在 exe 同目录创建名为 **`.portable`** 的空文件（注意有前导点，非 `.portable.txt`）：

```cmd
cd /d <KeePassXC目录>
type nul > .portable
```

之后所有配置文件和数据将存储在 exe 所在目录，无需写入 `%APPDATA%`，适合 U 盘携带。

### 1.4 环境变量覆盖

| 变量 | 作用 |
|---|---|
| `KPXC_CONFIG` | 指定主配置文件路径（覆盖默认位置） |
| `KPXC_CONFIG_LOCAL` | 指定本地状态文件路径 |

可通过 `--config` 和 `--localconfig` 命令行参数覆盖。

---

## 2. 构建类型

### 2.1 动态构建（默认）

依赖 Qt 运行时 DLL，由 `windeployqt` 自动部署到 exe 同目录。适合开发和调试。

### 2.2 静态构建

KeePassXC.exe（~37MB）和 keepassxc-cli.exe（~19MB）完全独立，**无需安装任何 Qt 或 VC++ 运行时**，可直接复制到任意 Windows 10+ 机器运行。

---

## 3. GUI 命令行参数

| 参数 | 功能 |
|---|---|
| `[filename(s)]` | 启动时打开指定 .kdbx 数据库 |
| `--config <path>` | 指定自定义主配置文件 |
| `--localconfig <path>` | 指定自定义本地状态文件 |
| `--keyfile <path>` | 指定密钥文件 |
| `--pw-stdin` | 从标准输入读取数据库密码 |
| `--lock` | 锁定所有已打开的数据库 |
| `--minimized` | 启动后最小化到系统托盘 |
| `--allow-screencapture` | 允许屏幕截图和录制（Windows/macOS） |
| `--debug-info` | 显示调试信息并退出 |
| `--version` | 显示版本信息并退出 |
| `--help` | 显示帮助信息并退出 |

---

## 4. CLI 命令参考

所有 CLI 功能集成在单一可执行文件 **`keepassxc-cli.exe`** 中。`sync`、`snapshot`、`db-cleanup` 都是它的子命令，不存在独立的 `sync.exe` 或 `snapshot.exe`。

```cmd
keepassxc-cli.exe <子命令> [参数]
keepassxc-cli.exe --help    # 列出所有可用子命令
```

### 4.1 数据库操作

| 命令 | 功能 |
|---|---|
| `add` | 添加新条目 |
| `analyze` | 分析密码强度和问题 |
| `clip` | 复制条目属性到剪贴板 |
| `close` | 关闭数据库 |
| `create` | 创建新数据库 |
| `diceware` | 生成 Diceware 密码短语 |
| `edit` | 编辑条目 |
| `estimate` | 估算密码强度 |
| `export` | 导出数据库 |
| `generate` | 生成随机密码 |
| `import` | 导入数据库 |
| `info` | 显示数据库信息 |
| `list` | 列出数据库条目 |
| `locate` | 查找条目 |
| `merge` | 合并两个数据库 |
| `move` | 移动条目到其他组 |
| `open` | 打开数据库 |
| `rm` | 删除条目 |
| `show` | 显示条目详情 |

### 4.2 附件操作

| 命令 | 功能 |
|---|---|
| `attachment-export` | 导出附件 |
| `attachment-import` | 导入附件 |
| `attachment-rm` | 删除附件 |

### 4.3 LocalVault 独有命令

| 命令 | 功能 |
|---|---|
| `sync --remote <path>` | 手动同步两个数据库（条目级差异分析 + 自动合并） |
| `sync --remote <path> --resolve <strategy>` | 同步后自动解决冲突，strategy 可选：`keep-local` / `keep-remote` / `create-copy` |
| `snapshot <db> --create` | 创建数据库快照 |
| `snapshot <db> --list` | 列出所有快照 |
| `snapshot <db> --delete <id>` | 删除指定快照 |
| `snapshot <db> --restore <id>` | 恢复指定快照（恢复前自动创建保护快照） |
| `snapshot <db> --protect <id>` | 标记快照为受保护（不被自动清理） |
| `snapshot <db> --unprotect <id>` | 取消快照保护 |
| `db-cleanup <db> [--max-snapshots N] [--max-days N] [--entry-history-limit N] [--tombstone-cleanup]` | 按策略清理快照、条目历史、墓碑 |

### 4.4 通用 CLI 选项

| 选项 | 功能 |
|---|---|
| `-q`, `--quiet` | 静默模式，仅输出错误 |
| `--key-file <path>` | 指定密钥文件 |
| `--no-password` | 不使用密码 |
| `-y`, `--yubikey` | 使用 YubiKey |

---

## 5. 同步概念

### 5.1 版本向量

LocalVault 使用**版本向量**（Vector Clock）而非时间戳判定并发修改。每个设备维护自己的版本计数器，同步时通过比较版本向量判断条目是否需要合并。

### 5.2 冲突处理

当同一字段在两个设备上被并发修改时，该字段进入冲突状态。支持 4 种解决策略：
- **保留本地** — 维持本地值不变，忽略远端修改
- **保留远端** — 以远端值覆盖本地
- **手动合并** — 按字段逐项选择保留哪一侧
- **生成副本** — 将远端条目克隆到同组，两个版本均保留

### 5.3 墓碑机制

条目的删除操作会被保留为**墓碑**记录，同步时传播到远端。墓碑在指定清理周期前不会被物理删除，确保删除操作可被同步。

### 5.4 快照

每次同步操作前自动创建数据库级快照。快照文件存储在本地专用目录，KDBX 内仅保存索引元数据。可通过 `snapshot --restore` 回退到任意快照。
