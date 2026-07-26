# pwd_merge — 旧式密码数据合并 & KDBX 转换工具

## 文档信息

- 文档类型：模块设计文档
- 版本：v1.1
- 日期：2026-05-29
- 关联模块：pwd_merge/

---

## 1. 目的

将不同时期、不同设备上生成的旧式 IDEA 加密密码数据文件（.dat）合并转换为 LocalVault 可识别的 KDBX 4.x 数据库。

**输入**：一个或多个 `.dat` 密码数据文件（IDEA-ECB 加密）
**输出**：单个 `.kdbx` 数据库文件（AES-256 / Argon2id）

---

## 2. 架构概览

```
pass.json (配置)
     │
     ▼
main.cpp ── CLI 入口 (QCoreApplication)
     │
     ├── crypt_file_reader  ← 读取 .dat → IDEA 解密 → RAWNODESTRUCT 记录
     │        ├── ideaplus   (IDEA 64-bit block cipher)
     │        ├── md5a       (MD5 hash → 128-bit key → IDEA subkeys)
     │        └── crypt_block (记录包装器)
     │
     └── merge_engine       ← 合并 + 去重 + 历史链
              │
              └── keepassxc_core  ← KDBX 4.x 写入
                   ├── Database / Entry / Group / TimeInfo
                   ├── CompositeKey / PasswordKey
                   └── Argon2id KDF
```

---

## 3. 旧式 .dat 文件格式

### 3.1 DBHEADER（32 字节，明文）

| 偏移 | 大小 | 字段 | 字节序 |
|------|------|------|--------|
| 0 | 2 | MAGICKEY (0xc1f5) | Big-endian |
| 2 | 2 | VERSION (2) | Native |
| 4 | 4 | TOTALCOUNT | Big-endian |
| 8 | 4 | USECOUNT | Big-endian |
| 12 | 4 | PASSHASH (reserved) | Big-endian |
| 16 | 4 | TIMESTAMP | Big-endian |
| 20 | 4 | BODYLENGTH | Big-endian |
| 24 | 8 | RESERVER | — |

文件总大小 = 32 + BODYLENGTH

### 3.2 RAWNODESTRUCT（256 字节，加密体解密后）

| 偏移 | 大小 | 字段 |
|------|------|------|
| 0 | 20 | szUserName (null-terminated) |
| 20 | 20 | szPassWord (null-terminated) |
| 40 | 200 | szComment (null-terminated) |
| 240 | 4 | szNodeID |
| 244 | 1 | cMagicChar (必须 = 'h', 解密验证哨兵) |
| 245 | 1 | cUsed (0=空槽, 非0=有效记录) |
| 246 | 4 | sAddTime (Big-endian time32_t) |
| 250 | 6 | PAD |

`#pragma pack(1)` 确保结构体对齐正确。

### 3.3 加密方式

- **算法**：IDEA (64-bit block cipher, 128-bit key, 8 rounds)
- **模式**：ECB（8 字节块独立加密，无 IV，无链接）
- **密钥派生**：Password → MD5 → 16 字节 → 配对为 8 个 16-bit 小端字 → IDEA key

---

## 4. 密钥派生 Bug（关键实现细节）

原始加密程序的密钥派生存在一个 C++ 运算符优先级 Bug：

```c
// 实际执行的代码（来自 crypt_file_manager.cpp:119）
shValue = szPasswd[i * 2] + szPasswd[i * 2 + 1] << 8;
```

在 C++ 中 `+` 优先级高于 `<<`，实际计算为：

```
shValue = (szPasswd[i*2] + szPasswd[i*2+1]) << 8
```

正确的 little-endian 拼装应为：

```c
shValue = szPasswd[i*2] | (szPasswd[i*2+1] << 8);
```

**解密时必须精确复制此 Bug**，否则生成的 IDEA 密钥不匹配。`crypt_file_reader.cpp` 的 `initCryptKey()` 中保留了带注释的 Bug 版本。

> 注意：`pwd_merge/crypt.cpp:109` 中包含正确的括号版本，不适合解密实际 .dat 文件。

---

## 5. 合并算法

### 5.1 Entry 身份判定

Entry 身份由复合键 `(userName, comment)` 唯一确定，**与 password 字段无关**。

两个记录属于同一个 Entry 当且仅当：
- `userName` 字符串完全相同，且
- `comment` 字符串完全相同

password 只用于判断该 Entry 在历史上是否发生过凭证变更（见 5.3），不作为身份判定依据。

> **字符串预处理**：所有字段在比较/合并前均进行首尾空白字符裁剪（ASCII 码 ≤ 0x20，含空格、`\t`、`\r`、`\n`）。
> 因为旧式 `.dat` 文件的字符串存储在固定长度 char 数组中（szUserName[20], szComment[200]），
> 不同文件生成时 null terminator 之后的残留字节可能不同，导致同一 Entry 的 comment 出现尾随空格/换行差异。

### 5.2 分组

以 `(userName, comment)` 为复合键，将所有 .dat 文件中的记录分组。不同组的记录互不影响。

### 5.3 密码比较规则

密码比较为**首尾裁剪后精确匹配**（trim whitespace → string equality，区分大小写），不做语义分析。

裁剪规则：去除首尾所有 ASCII 码 ≤ 0x20 的字符（空格、`\t`、`\r`、`\n`）。

两个密码视为「相同」当且仅当裁剪后字符串逐字节相等，例如：
- `"Abc@123"` 与 `"Abc@123"` → 相同
- `"Abc@123"` 与 `"abc@123"` → 不同（大小写不同）
- `"pass "` 与 `"pass"` → **相同**（尾随空格被裁剪）
- `"pass\n"` 与 `"pass"` → **相同**（尾随换行被裁剪）
- 空字符串 `""` 与 `""` → 相同

### 5.4 组内处理

同组记录（即同一个 Entry 的历史）按 `addTimeStamp` 升序排列后，相邻且密码相同的记录视为同一凭证的重复保存，仅保留时间最新的一份。

```
1. Sort by addTimeStamp (oldest → newest)
2. Collapse consecutive equal passwords → keep newest only
3. After collapse, count remaining entries:

   = 1: 只有一个有效凭证 → 生成单个 Entry，无历史

   > 1: 存在历史上使用过的不同密码
        → 最新一条 → live Entry（当前密码）
        → 其余按时间 oldest-first → addHistoryItem() chain
```

**示例**：某组记录按时间排序后的密码序列为：

```
A → A → B → A → C
```

执行步骤 2（collapse consecutive equal passwords）后：

```
A(最新) → B → A(老旧) → C
```

最终：C 为 live Entry，A → B → A 按 oldest-first 依次进入历史链。

### 5.5 Entry 字段映射

| KDBX 字段 | 来源 |
|-----------|------|
| Title | userName（首尾裁剪后）|
| Username | userName（首尾裁剪后）|
| Password | password（首尾裁剪后）|
| URL | 从 comment 提取（见 5.6）|
| Notes | 完整 comment（首尾裁剪后）|
| CreationTime | sAddTime (Big-endian → Unix timestamp) |
| LastModificationTime | 同上 |

### 5.6 URL 提取规则

从 comment 文本中匹配以下三种 URL 模式（优先级从高到低，首次匹配即返回）：

1. **协议前缀 URL**：`http://`、`https://`、`ftp://` 开头
2. **www 前缀 URL**：`www.` 开头
3. **裸域名**：含有效 TLD 的域名，可选 `:端口` 和 `/路径?查询#片段`

匹配时排除以下字符（防止从中文文本中贪婪截取）：
- ASCII 空白符（`\s`）
- 括号、引号、尖括号（`()[]"'<>`）
- 全角中文标点：`，。、：；！？（）`

匹配后裁剪尾随的 ASCII 和全角标点（`.` `,` `;` `)` `，` `。` `）`）。

示例：

| comment | 提取结果 |
|---------|---------|
| `http://www.cartoon-sky.com/` | `http://www.cartoon-sky.com/` |
| `www.qwerks.com` | `www.qwerks.com` |
| `csdn.net` | `csdn.net` |
| `example.com:8080/path?a=1` | `example.com:8080/path?a=1` |
| `访问网站 csdn.net 注册账号` | `csdn.net` |
| `网址csdn.net，注册账号` | `csdn.net`（全角逗号被排除+裁剪）|

> **实现注意**：必须使用普通字符串字面量（非 raw string），确保 `\uXXXX` Unicode 转义序列被 C++ 编译器正确编译为全角标点字符。PCRE2 引擎在字符类中遇到 `\u` 会将其视为字面量 `u`，不会自行解析 Unicode 转义。

### 5.7 统计项

### 5.7 统计项

- **Duplicates removed**：同 (userName, comment, password) 的旧版本被丢弃
- **Unique entries**：合并后的最终 Entry 数
- **With history**：含历史密码链的 Entry 数

---

## 6. 使用方式

### 6.1 配置文件 (pass.json)

```json
{
    "password": "解密密码",
    "inputs": [
        "data/passinfo-sz.dat",
        "data/passinfo_hk.dat"
    ],
    "output": "data/merged.kdbx",
    "output_password": "KDBX主密码"
}
```

### 6.2 运行

```bash
# 纯配置文件模式（密码不在命令行明文出现）
pwd_merge.exe --config pass.json

# CLI 覆盖配置文件中的字段
pwd_merge.exe --config pass.json -i extra.dat -o extra.kdbx
```

### 6.3 命令行参数

| 参数 | 说明 |
|------|------|
| `-c, --config <file>` | JSON 配置文件 |
| `-i, --input <file>` | 输入 .dat 文件（可重复） |
| `-o, --output <file>` | 输出 .kdbx 路径 |
| `-p, --password <pwd>` | .dat 解密密码（覆盖配置文件） |
| `-P, --output-password <pwd>` | .kdbx 主密码（覆盖配置文件） |

**优先级**：命令行参数 > 配置文件 > 交互式提示

---

## 7. 构建集成

### 7.1 CMake

`pwd_merge/CMakeLists.txt` 作为 KeePassXC 子目录，链接 `keepassxc_core`：

```cmake
add_executable(pwd_merge
    crypt_file_reader.cpp crypt_block.cpp merge_engine.cpp
    main.cpp ideaplus.cpp md5a.c utils.cpp
)
target_link_libraries(pwd_merge keepassxc_core Qt6::Core)
```

入口：`src/CMakeLists.txt` 中 `add_subdirectory(../pwd_merge ...)`。

### 7.2 产物

`build/src/pwd_merge/Release/pwd_merge.exe`

---

## 8. 文件清单

```
pwd_merge/
├── CMakeLists.txt          # 构建定义
├── main.cpp                # CLI 入口（QCoreApplication + QCommandLineParser）
├── merge_engine.h/cpp      # 合并引擎 + KDBX 写入
├── crypt_file_reader.h/cpp # .dat 文件读取 + IDEA 解密
├── crypt_block.h/cpp       # RAWNODESTRUCT 记录包装
├── data_def.h              # DBHEADER / RAWNODESTRUCT 结构体定义
├── ideaplus.h/cpp          # IDEA 密码算法（Ascom Systec, Release 2.1）
├── md5a.h/c                # MD5 哈希算法（Public Domain, Colin Plumb 1993）
├── c_ext_fc.h              # ANSI C 原型兼容宏
├── c_fct.h                 # ANSI C 参数声明宏
├── utils.h/cpp             # bin2hex / hex2bin 工具
├── crypt.h/cpp             # 旧版参考代码（不参与编译）
├── mian.cpp                # 旧版 main（不参与编译）
├── pass.json               # 配置文件
└── data/
    ├── passinfo-sz.dat     # 深圳密码数据（323 条）
    ├── passinfo_hk.dat     # 香港密码数据（349 条）
    └── merged.kdbx         # 输出数据库
```

---

## 9. 错误码

| 错误码 | 含义 |
|--------|------|
| -1 | 无法打开文件 |
| -2 | 文件太小，缺少 header |
| -3 | Magic key 不匹配（密码错误？） |
| -4 | Body 长度不一致（文件损坏） |
| -5 | IDEA 解密失败（密码错误？） |
| -6 | 记录 magic 无效（密码错误或文件损坏） |

---

## 10. 外部依赖

| 依赖 | 用途 |
|------|------|
| keepassxc_core | KDBX 读写（Database, Entry, Group, CompositeKey, KDF） |
| Qt6::Core | QString, QJsonDocument, QCommandLineParser, QRegularExpression |
| ws2_32.lib | ntohs/ntohl（Windows） |

---

## 11. 变更记录

| 版本 | 日期 | 变更内容 |
|------|------|---------|
| v1.1 | 2026-05-29 | 新增首尾空白裁剪（5.1/5.3），修复不同文件的 comment 尾随空格差异导致无法合并历史链 |
| v1.1 | 2026-05-29 | 修复 URL 提取正则：裸域名识别、全角中文标点排除、`\uXXXX` Unicode 转义 |
| v1.0 | 2026-05-28 | 初始版本 |
