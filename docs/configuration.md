# 配置与文件位置

## 位置

| 内容 | 位置 |
| --- | --- |
| 设置（漫游，用户级） | `%APPDATA%\DeskFeed\settings.ini` |
| 设置（隔离实例） | `%APPDATA%\DeskFeed\test-<scope>\settings.ini` |
| 日志 | `%LOCALAPPDATA%\DeskFeed\deskfeed.log` |
| 日志（隔离实例） | `%LOCALAPPDATA%\DeskFeed\test-<scope>\deskfeed.log` |
| `--inspect` 转交文件 | `%LOCALAPPDATA%\DeskFeed[\test-<scope>]\inspect-<发送方 PID>.json`（读完即删） |
| 建议部署位置 | `%LOCALAPPDATA%\DeskFeed\deskfeed.exe`（构建产物在 `.agent\build\debug\bin`） |

`--state-dir <目录>` **只**覆盖设置文件所在目录（文件名仍是 `settings.ini`）。日志与
`--inspect` 转交文件不受它影响，仍在上面 `%LOCALAPPDATA%\DeskFeed[\test-<scope>]` 的隔离目录下。

## 键

```ini
[source]
url=http://127.0.0.1:8099/snapshot
poll_ms=30000
retry_ms=5000
timeout_ms=8000
```

以下栏目为**预留、当前版本不读取**（Phase 2/3 使用）：

```ini
[window]
right=2560
bottom=1392
expanded=false
topmost=true
hidden=true

[taskbar]
component=true
```

- `[source] url` 是唯一的数据源地址。没有它时按“未配置 URL”处理（红色 `ERROR`），
  不使用任何默认地址。
- `poll_ms` / `retry_ms` / `timeout_ms`：0 或缺省表示用默认值；越界值夹到边界。
  上下限见 [http.md](http.md) 的“调度”。
- 值**按原样读取与保存**：字符串键不做去空白、不做大小写归一。地址里出现首尾或内部空白、
  控制字符会被当成非法地址拒绝，而不是悄悄改写。
- URL 里的查询串会原样保存与请求；只有片段（`#...`）不会发给服务器。
- 数值键允许首尾空白（人写的文件里常见）。

## 写入规则

- **只改自己负责的那一行**：写 `[source] url` 时，其它栏目、其它键、注释与顺序一个字节都不动；
  缺失的键插到对应栏目的末尾（栏目不存在就先补一行栏目头）。
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
