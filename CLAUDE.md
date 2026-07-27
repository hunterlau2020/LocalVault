# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# LocalVault — 本地优先条目级同步密码管理器

## Project Overview

基于 **KeePassXC fork** 二次开发的本地密码管理器。核心定位：
- **本地加密数据库** (KDBX 4.x) 为主存储
- **条目级本地同步合并**，而非整库覆盖
- **服务器仅存密文**，不参与解密/协调
- **版本向量**判定并发修改，**字段级冲突检测**
- **墓碑机制**处理删除同步
- **FIDO2** 增强解锁认证 + **恢复密钥包**
- **数据生命周期管理**（快照/历史/墓碑清理）

当前 Phase 1-7、11 已完成，Phase 8-10 待开发。

### 仓库

- **Fork**: `https://github.com/hunterlau2020/LocalVault`
- **长期分支**: `feature/local-sync-merge`
- **上游**: KeePassXC 官方仓库
- 工作流：子功能分支 → 合并回长期分支 → 定期同步上游

## 文档体系（docs/）

| 文档 | 内容 |
|---|---|
| `SRS.md` | 产品需求规格，含功能/非功能需求、验收标准、安全要求 |
| `FSD拆解版v1.1.md` | 10 个核心模块的详细拆解（子模块、数据结构、接口、任务清单） |
| `Architecturev1.1.md` | 5 层架构、数据模型、同步流程、冲突处理、状态机 |
| `测试用例大纲.md` | 13 个模块的测试点、集成链路、性能/安全测试 |
| `Plan.md` | 启动计划、KeePassXC fork 策略、开发阶段划分 |
| `development-roadmap.md` | 开发阶段详情、交付物、Bug 修复记录、CLI 命令参考 |
| `USER_MANUAL.md` | 用户手册：配置文件位置、命令行参数、CLI 命令、同步概念 |

**阅读顺序**: SRS → Architecture → FSD → Plan → development-roadmap

## 架构分层

1. **表现层** — Qt UI (MainWindow, EntryListView, ConflictDialog 等)
2. **应用服务层** — 用例编排 (DatabaseAppService, SyncAppService 等)
3. **领域服务层** — 业务规则 (EntryMergeService, ConflictResolutionService 等)
4. **基础设施层** — KDBX 读写、FIDO2、SFTP/WebDAV、安全内存、审计日志
5. **外部依赖** — FIDO2 设备、Windows WebAuthn、远端存储

## 关键设计约束

- 同步元数据存储在 **KDBX Custom Data** 内（JSON + schema_version），禁止 sidecar 文件
- 使用**版本向量/向量时钟**判定并发修改，不依赖时间戳
- **同字段并发修改不得自动覆盖**，必须进入冲突处理
- 删除使用**墓碑**标记，不得物理删除
- 同步前自动创建**数据库级快照**
- 单条目默认最多保留 **10 条**历史版本
- FIDO2 采用 **主密码 + FIDO2（AND）** 模式，仅用于解锁
- 恢复密钥包**不参与主密钥派生**，仅作为绕过 FIDO2 的授权令牌
- 主密码 SecureBuffer 在解密完成后**立即执行 zeroing**

## 开发环境

- Windows 11 / Visual Studio 2022 + MSVC / CMake / Git
- **vcpkg**: `C:\mysoft\vcpkg-full`
- **Qt (动态)**: `D:\cloud_data\download\Qt\6.11.0\msvc2022_64`
- **Git**: `C:\mysoft\Git`
- **VC 环境**: `C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Visual Studio 2022\Visual Studio Tools\VC`

## 构建命令

### 动态构建（使用外部 Qt，产生 DLL 依赖）

```bash
cmake -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_PREFIX_PATH="D:/cloud_data/download/Qt/6.11.0/msvc2022_64" \
  -DCMAKE_TOOLCHAIN_FILE="C:/mysoft/vcpkg-full/scripts/buildsystems/vcpkg.cmake" \
  -DKPXC_FEATURE_DOCS=OFF \
  -DWINSDK="C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22000.0/um/x64/WindowsApp.lib" \
  -B build

cmake --build build --config Release
```

### 静态构建（无 Qt/VC++ DLL 依赖，独立运行）

```bash
cmake -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_TOOLCHAIN_FILE="C:/mysoft/vcpkg-full/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=x64-windows-static \
  -DKPXC_STATIC_BUILD=ON \
  -DKPXC_FEATURE_DOCS=OFF \
  -DWINSDK="C:/Program Files (x86)/Windows Kits/10/Lib/10.0.22000.0/um/x64/WindowsApp.lib" \
  -DWITH_TESTS=OFF \
  -DBOTAN_LIBRARY="E:/works/LocalVault/build/vcpkg_installed/x64-windows-static/lib/botan-3.lib" \
  -DBOTAN_LIBRARY_DEBUG="E:/works/LocalVault/build/vcpkg_installed/x64-windows-static/debug/lib/botan-3.lib" \
  -B build

cmake --build build --config Release

# 产物: build/src/Release/KeePassXC.exe (37MB)
#       build/src/cli/Release/keepassxc-cli.exe (19MB)
# 首次构建需 vcpkg 编译 Qt 源码（一次性 4-8 小时），后续增量构建几分钟
```

### 运行

```bash
# GUI
cd build/src/Release && ./KeePassXC.exe

# CLI
cd build/src/Release && ../cli/Release/keepassxc-cli.exe --help
```

### 测试

```bash
# 动态构建
cp build/tests/Release/test*.exe build/src/Release/
cd build/src/Release && ./testxxx.exe

ctest -C Release -R <test_name>
```

## 工作流程

### 修改前
1. 阅读相关文件确认上下文
2. 涉及 >2 文件或 >50 行的改动先给方案概要
3. 需要人工确认时，用 `msg * "xxx"` 弹 Windows 消息提示

### 修改后
1. 运行相关测试
2. **禁止使用并发编译**（如 `-j` 或 `--parallel`），必须使用单线程编译以避免资源消耗过高或编译冲突
3. 确保 MSVC 编译无警告
4. 集成测试: `ctest -C Release`
5. 更新 ctags: `ctags -R .`
6. 变更数据库结构需通知用户

### 提交规范
- 不提交 .md 文档、二进制文件、data/ 目录
- 不提交测试数据、密码账号
- Commit 前测试必须通过
