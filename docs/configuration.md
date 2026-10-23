# 配置与文件位置

## 位置

| 内容 | 位置 |
| --- | --- |
| 设置（漫游，用户级） | `%APPDATA%\DeskFeed\settings.ini` |
| 设置（隔离实例） | `%APPDATA%\DeskFeed\test-<scope>\settings.ini` |
| 日志 | `%LOCALAPPDATA%\DeskFeed\deskfeed.log` |
| 日志（隔离实例） | `%LOCALAPPDATA%\DeskFeed\test-<scope>\deskfeed.log` |
| `--inspect` 转交文件 | `%LOCALAPPDATA%\DeskFeed[\test-<scope>]\inspect-<发送方 PID>.json`（读完即删） |
| 建议部署位置 | `%LOCALAPPDATA%\DeskFeed\`（主进程、负载 DLL 与注入器同目录；见 [building.md](building.md)） |

`--state-dir <目录>` **只**覆盖设置文件所在目录（文件名仍是 `settings.ini`）。日志与
`--inspect` 转交文件不受它影响，仍在上面 `%LOCALAPPDATA%\DeskFeed[\test-<scope>]` 的隔离目录下。

## 键

```ini
[source]
url=http://127.0.0.1:8099/snapshot
poll_ms=30000
retry_ms=5000
timeout_ms=8000

[window]
; right / bottom 是窗口右下角的屏幕坐标锚点，可以省略；省略时按下面表格的缺省落位
expanded=false
topmost=true
hidden=false

[taskbar]
component=true
; 启用托盘显示的屏幕标识（分号分隔）；省略整个键 = 只在主屏显示
; 显式写成空值 = 不在任何任务栏显示
screens=\\?\DISPLAY#PHLC226#5&81de98e&0&UID8451#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}
```

`[source]`：

- `url` 是唯一的数据源地址。没有它时按“未配置 URL”处理（红色 `ERROR`），不使用任何默认地址。
- `poll_ms` / `retry_ms` / `timeout_ms`：0 或缺省表示用默认值；越界值夹到边界。
  上下限见 [http.md](http.md) 的“调度”。
- **地址的精确性**：通过命令（`--url`）提交并被接纳的地址，保存与请求都保持**精确原样**，不做
  去空白、不做大小写归一。手写 INI 则按当前 parser 处理**整行**：整行两端的外沿空白（空格/制表符）
  会先被去掉，`=` 之后的前导空白**保留**在值里。地址里出现空白或控制字符会被当成非法地址**报错**，
  而不是悄悄改写（需要空格请写 `%20`）。没有可用的地址时按“未配置 URL”处理（红色 `ERROR`）。
- URL 里的查询串会原样保存与请求；只有片段（`#...`）不会发给服务器。
- 数值键允许首尾空白（人写的文件里常见）。

`[window]` 是悬浮窗的界面偏好，`[taskbar] component` 是任务栏组件开关，`[taskbar] screens` 是
托盘显示在哪些屏幕上。**这几组键当前版本就在读写**，随界面动作即时落盘（语义见
[taskbar-injection.md](taskbar-injection.md) 与 [src/widget/README.md](../src/widget/README.md)）：

| 键 | 含义 | 缺省 | 读不出来时 |
| --- | --- | --- | --- |
| `[window] right` / `bottom` | 窗口**右下角**的屏幕坐标锚点 | 主屏可用区右下角向内 10 px | 两个键缺一个、不是整数、或放不进 `LONG`，就按缺省落位（不会把窗口丢到 `(0,0)`）；负值合法（左侧/上方显示器） |
| `[window] expanded` | 展开详情 | `false` | 按缺省 |
| `[window] topmost` | 置顶 | `true` | 按缺省 |
| `[window] hidden` | 用户把悬浮窗藏起来（跨启动保持） | `false`（显示） | 按缺省 |
| `[taskbar] component` | 任务栏组件的**持久意图** | `true`（开） | 按缺省 |
| `[taskbar] screens` | 托盘显示在哪些屏幕上：分号分隔的屏幕标识 | 键不存在 = 仅主屏 | 整行解析不出合法标识时按“仅主屏”处理 |

- 布尔键只认 `true` / `false`（大小写不敏感，允许首尾空白），写坏的值按缺省处理，
  不因为一行坏值改变界面行为。
- `[taskbar] component` 缺省为 `true`，所以**没有设置文件的新实例默认就会武装任务栏组件**
  （见 [README.md](../README.md) 的“默认行为”）。要默认关掉它得在设置文件里显式写 `false`；
  `--no-inject` / `--no-taskbar` / `--headless` 只关本次运行。
- `--hidden` 是本次运行的强制隐藏，**不**改写 `[window] hidden`。
- `[taskbar] screens` 的每一项是**显示器接口路径**（`\\?\DISPLAY#...`），不是 `\\.\DISPLAY2`
  这种随拓扑变化的名字。托盘或悬浮窗的右键菜单里勾选屏幕时由程序写入；一项表示该屏幕启用。
  - 键**不存在**：向后兼容的缺省，只在主屏显示。
  - 键存在但值为空（`screens=`）：用户明确选择**不在任何任务栏显示**。这种配置不会因为
    “没有可显示的任务栏”而退化到主屏。
  - 启用了某块屏幕、但该屏幕当前失效（没有任务栏窗口或托盘元素解析不出来），并且启用的屏幕
    **一块都不可用**时，负载只在主屏显示；**退化不写回配置**。启用了多块而只有部分不可用时，
    可用的那几块照常显示，不退化。规则见 [taskbar-injection.md](taskbar-injection.md) 的
    “多屏与退化”。
  - 屏幕拔掉后，它的标识仍留在配置里，菜单会以“已断开的屏幕”列出并允许取消；重新接回同一块
    屏幕（同一接口）时会自动恢复。
- `--inspect` 的 `taskbarScreens` 字段报告这份持久配置（`configured` 与 `enabled`），不是本次
  退化后的实际落点。

## 写入规则

- **只改自己负责的那一行**：写 `[source] url` 时，其它**未修改**行的正文、栏目、键、注释与顺序
  保持不变；被改的那一行按 `key=value` 重写。缺失的键插到对应栏目的末尾（栏目不存在就先补一行
  栏目头）。
- **保存会统一编码与行尾**：整份文件以 **UTF-8 无 BOM** 重写，每行以 **CRLF** 结尾。原文件若是
  LF 行尾或带 BOM，保存后就不再是逐字节相同的文件——上面说的“不动”指**未修改行的正文内容**，
  不是整份文件的字节。
- `[window]` 与 `[taskbar] component` 各由自己的那条路径写：界面偏好写 `[window]`，组件开关写
  `[taskbar] component`，屏幕勾选写 `[taskbar] screens`，互不重复写。
- `[taskbar] screens` 的写入同样是候选克隆 + 原子替换；整行按 `id;id;...` 规范化。空集合写成
  `screens=`（不是删掉这一行），所以“明确关闭”与“从未配置”在磁盘上可以区分。
- **原子替换**：先写同目录临时文件并 `FlushFileBuffers`，再
  `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`。失败时目标文件保持原样。
- 设置文件读不出来（不是合法 UTF-8 等）时，本次运行**拒绝一切写入**并在日志里说明，
  以免把用户文件覆盖掉。

## 绝不落盘的内容

- PID、进程句柄、绑定代次；
- 缓存内容与失效/到期状态（重启后不恢复上次读数）；
- “只在本次运行有效”的开关（`--headless` / `--no-inject` / `--hidden` / `--state-dir` / `--scope`）。

## 脱敏策略

地址里可能带着上游密钥（userinfo 或 `?token=...`）。因此：

- **任何打印出去的地址**（日志、`--inspect` 的 `runtimeUrl` 与 `diskUrl`）都会去掉 userinfo、
  完整查询串与片段，只留 `scheme://host[:port]/path`；
- `runtimeUrlRedacted` 记录是否真的删过东西；
- **实际保存与请求的地址一个字节都不改**。

## 隔离开关

- `--scope <名字>` / `--test-instance <名字>`（或环境变量 `DESKFEED_SCOPE` / `DESKFEED_TEST_INSTANCE`）
  把互斥体、控制窗口类名、设置文件与日志全部换到隔离命名空间。
- 名字只允许 `[A-Za-z0-9_-]`、长度 1..32；其它字符、超长、显式空串都是用法错误。
  超长/非法的环境变量值会被**原样读出并拒绝**，不会悄悄退化成生产实例。
- `--headless`（或 `--no-inject` + `--hidden`）保证本次运行不碰界面与任务栏。
- `--mock` 只是 `--headless` 的别名，**不伪造数据**。
