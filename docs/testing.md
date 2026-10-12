# 测试与安全验收

## 构建与离线用例

```powershell
# 在“先 chcp 65001、再 vcvarsall.bat x64”的同一条 cmd 里执行（见 building.md）
cmake -S . -B .agent/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build .agent/build/debug

ctest --test-dir .agent/build/debug --output-on-failure
.\.agent\build\debug\bin\deskfeed-tests.exe --filter snapshot
```

`deskfeed-tests.exe` 覆盖：JSON 严格解析与深度上限、`expiresAtMs` 的数值整数语义、容量边界、
单行/空白判据、完整替换、失效与恢复、独立到期与不续期、首份即过期、调度夹取、命令行解析、
实例名白名单、控制通道线格式、地址解析/脱敏/点段合并、设置文件保留其它键、进程绑定的“已退出”拒绝、
回环 HTTP 的整次墙钟预算与取消、显示帧编解码与显示邮箱（每次读的帧头校验、生产者退出后不再交出
陈旧帧、消费者始终绑定原生产者身份）、检查器输出的严格 JSON 与 `--stdin` 的管道输入。
全部离线：不读凭据、不连外网、不注入、不碰用户正在跑的实例（回环用例只连 `127.0.0.1`）。

## 离线检查器

产物在 `.agent/build/debug/bin`（见 [building.md](building.md)），下面统一用这个路径：

```powershell
.\.agent\build\debug\bin\deskfeed-check.exe --file snapshot.json --report json
.\.agent\build\debug\bin\deskfeed-check.exe --stdin --report json < snapshot.json
.\.agent\build\debug\bin\deskfeed-check.exe --url "http://127.0.0.1:8099/snapshot" --timeout-ms 8000 --report json
.\.agent\build\debug\bin\deskfeed-check.exe --help
```

`--stdin` 按**字节流**读取，文件重定向（上面的 `< snapshot.json`）与管道
（`... | deskfeed-check --stdin`、或程序化起子进程喂管道）都支持。

退出码：`0` 接受、`2` 拒绝（HTTP 或校验）、`1` 用法/内部错误。`--report json` 的输出形状
——**接受与拒绝两条路径都是一行合法 JSON**，可以直接交给任何严格解析器：

```json
{"accept":true,"kind":"none","message":"","bodyBytes":210,"version":1,"expiresAtMs":1791825470141,
 "items":[{"id":"cpu","label":"CPU","value":"54℃","state":"ok","detail":"最近一次采样结果"}]}
```

`kind` 是校验原因（`too-large` / `encoding` / `parse` / `root` / `version` / `items` / `item-shape` /
`required-field` / `field-type` / `field-limit` / `item-count` / `duplicate-id` / `empty-id` /
`invalid-state` / `blank` / `single-line` / `expiry`）或 HTTP 失败名（`invalid-url` / `connect` /
`timeout` / `send` / `receive` / `redirect` / `http-status` / `too-large` / `empty`）。

`expiry` 表示 **`expiresAtMs` 这个字段本身非法**（类型或取值范围不对），不是“快照已经过期”：
结构合法但时间已过的快照属于**接受**（首份也接纳，只是整体染红），见
[protocol.md](protocol.md) 的失效与到期规则。
没有 `--insecure` 之类的 TLS 绕过开关，这是刻意的。

## mock 生产端

```powershell
node examples\producer\minimal-producer.mjs --port 8099
```

开关见 [examples/producer/README.md](../examples/producer/README.md)：`?clear=1`、`?expired=1`、
`?no-expiry=1`、`?ttl-ms=`、`?bad=1`、`?status=`、`?slow-headers=`、`?slow-chunk=`。

## 安全验收流程（必须照做）

验收脚本只能在**隔离实例**上跑，绝不能碰用户的生产实例，更不能碰 `explorer.exe`：

```powershell
$bin = ".agent\build\debug\bin"
node examples\producer\minimal-producer.mjs --port 8099   # 另开一个窗口

# 用隔离命名空间 + 无界面 + 不注入 启动主实例
Start-Process -FilePath "$bin\deskfeed.exe" -ArgumentList @(
  '--scope','acc1','--headless','--url','http://127.0.0.1:8099/snapshot')

# 查询运行状态（--inspect 不会新建实例）
$p = Start-Process -FilePath "$bin\deskfeed.exe" -ArgumentList @('--scope','acc1','--inspect') `
     -Wait -PassThru -RedirectStandardOutput inspect.json -RedirectStandardError inspect.err
$p.ExitCode      # 0 = 收到应答
Get-Content inspect.json

# 条件退出、切换地址、正常退出
& "$bin\deskfeed.exe" --scope acc1 --quit-if-bound 999999      # 不匹配：不退出
& "$bin\deskfeed.exe" --scope acc1 --url "http://127.0.0.1:8099/snapshot?ttl-ms=120000"
& "$bin\deskfeed.exe" --scope acc1 --quit
```

**重要**：`deskfeed.exe` 是 WIN32 子系统程序，`cmd`/PowerShell 的 `&` **不会**等它结束，
`$LASTEXITCODE` 会停留在上一条命令的值上。要拿真实退出码与输出，必须用
`Start-Process -Wait -PassThru -RedirectStandardOutput/-RedirectStandardError`，
或把命令放进 `cmd /c start /wait`。

## 验收时不得做的事

- 不注入 `explorer.exe`，不调用任何注入/卸载工具（当前版本根本没有这些能力）。
- 不用镜像名批量结束进程（`taskkill /IM`），不因为 IPC 超时就重开或强杀实例。
- 不用默认（生产）命名空间做验收；一律带 `--scope <名字>`。
- 不在验收脚本里改动 `%APPDATA%\DeskFeed\settings.ini`（生产实例的设置）；隔离开关会把它换到
  `test-<scope>` 目录里。
- 不新建 GUI 会话、不发消息、不移动用户光标。

## 覆盖矩阵（对应协议条目）

| 组别 | 用例 |
| --- | --- |
| 完整快照 | 顺序、遗漏删除、空数组立即清空；清空后失败无旧值复活 |
| 校验与容量 | 未知字段忽略；类型/必填/版本/state/id/重复 id 整份拒绝；业务 `error` 不串项；所有容量临界值；UTF-8/BOM/null/单行/多行 |
| 失效与恢复 | 第一次失败无值显示红 `ERROR`；有值立即全红且无保留截止；合法新值恢复各项 `state` |
| 到期 | 未声明不推断年龄；声明后独立生效；反复 200 旧快照不续期；首份过期非空快照显示红值 |
| HTTP | 本地/远程 HTTP(S)、仅 200 接纳、204/304 不代替清空、有界 redirect、禁 TLS 降级、手动刷新不改 URL |
| 配置事务 | URL 落盘/重启独立、PID 不落盘、无 URL、不同字符串换源清缓存、同 URL 重绑；非法 PID/保存失败整条无副作用 |
| 命令与竞态 | 无参不接管、`--url` 单独解绑、并发短命客户端、就绪/发送超时、迟到旧 HTTP、旧绑定退出不影响新绑定 |
| 显示与任务栏 | Phase 2/3：一个主进程、两面同态、邮箱只读最新完整帧、DLL 重新加载、explorer 重启、主进程正常/异常退出清理 |
