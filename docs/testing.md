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
陈旧帧、消费者始终绑定原生产者身份）、检查器输出的严格 JSON 与 `--stdin` 的管道输入、
以及条件退出与 `--wait-exit-ms` 的“等到实例真的退出 / 拿不到证据就返回 5 且不发 IPC”两条路径。
全部离线：不读凭据、不连外网、不注入、不碰用户正在跑的实例（回环用例只连 `127.0.0.1`）。

CTest 注册 8 条：

| 用例 | 内容 |
| --- | --- |
| `deskfeed-tests` | 协议/状态机/帧/邮箱/检查器/主进程事务与条件退出 |
| `deskfeed-native-tests` | 负载侧：显示帧消费策略、钩子门收尾协议、C++/SEH 边界、导入表改写、回收门位、真 DLL 的 `status` / `glue` / `failclosed` / 进程守卫 |
| `deskfeed-supervisor-tests` | 主进程侧任务栏编排，配 `deskfeed-fake-injector`（**不注入任何进程**的假注入器） |
| `deskfeed-tools-tests` | 注入器工具：目标解析、远程调用超时与迟到写者、per-target 租约与真实动作路径（配 `deskfeed-test-remote-writer`、`deskfeed-test-fixture-payload` 与 `deskfeed-tools-fixture-host` 三个**测试专用**夹具） |
| `deskfeed-payload-audit` | 成品**生产** DLL 的导入表 / 导出名 / map 审计 |
| `deskfeed-payload-test-audit` | 成品**自测** DLL 的同款审计（探针入口必须仍在） |
| `deskfeed-widget-tests` | 悬浮窗纯逻辑与偏好 |
| `deskfeed-widget-window-tests` | 悬浮窗隐藏窗口门禁（真的建窗口，但强制隐藏） |

一律带 `--no-tests=error`，避免“没有注册用例”被当成绿。

`deskfeed-native-tests` 的过滤器：`--filter feed` / `seh` / `retire` / `status` / `glue` / `failclosed` /
`guard`；`--audit <dll>` 只跑成品审计。`--filter guard` 里的十三条进程守卫场景各自在**新子进程**里跑
（父进程用 `CreateProcessW` 起自己，`--guard-child <场景>`，断言退出码）：守卫对象名按目标
pid + 创建时间命名、`DllMain` 每个进程只 attach 一次，所以“Native 自己 init / actor 先 init /
粘性事件已置位 / 坏 header / 错类型对象 / 只读零 header / abandoned 恢复 / 引用跨 FreeLibrary /
每轮 reload 不泄漏（含逐轮句柄数回到基线、命名清理事件在 `FreeLibrary` 后必须消失）/
无名钩子事件在 DETACH 归还 / R5 登记的托盘目标真的是这次 attach 的 T（`probtray`）以及登记之后它
消失时绝不回退真实 `Shell_TrayWnd`（`probtraydead`）”必须分别是新进程。这些用例只碰**用例自己的
进程**：`LoadLibrary` 成品 DLL、建自己的命名对象、线程、一条**隐藏**的合成 UI 线程（`hookevents` 与
`probtray*` 用它把真实的 `WH_GETMESSAGE` 线程钩子装到自己的线程上）与一个自己的显示邮箱生产者
（`probtray*` 用它让负载真的走到 attach）；窗口都是不可见的 STATIC，不注入 explorer、不起可见 GUI、
不碰真实任务栏与光标。

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

- 不注入 `explorer.exe`，也不运行 `deskfeed-inject.exe` 的真实注入路径：工具已经构建，但真实注入必须
  由用户明确授权，并按 [native-safety.md](native-safety.md) 的纪律逐轮做。离线用例只在自己的进程里
  `LoadLibrary` 成品 DLL（`deskfeed-native-tests`），不碰别人的进程。
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
| HTTP | **单元层**（CTest 内的 `deskfeed-tests`，只连进程内 `127.0.0.1` 的 mock 生产端）：仅 200 接纳、204/304 不代替清空、有界 redirect、正文超限、慢头部/慢滴流、取消；**真实 HTTPS/TLS** 不在 CTest 里，见下面"独立 HTTPS 检查器验收" |
| 配置事务 | URL 落盘/重启独立、PID 不落盘、无 URL、不同字符串换源清缓存、同 URL 重绑；非法 PID/保存失败整条无副作用 |
| 命令与竞态 | 无参不接管、`--url` 单独解绑、并发短命客户端、就绪/发送超时、迟到旧 HTTP、旧绑定退出不影响新绑定 |
| 显示与任务栏 | 显示帧/邮箱与负载侧收尾协议在**离线与自测宿主**范围内已验证（含进程守卫与两侧 DLL 审计）；真机 `explorer.exe`、真 XAML 走查与 `--cores 1/2/3` 矩阵**尚未**验收 |

### 独立 HTTPS 检查器验收（2026-10-13）

公开 CTest **不含**真实 HTTPS 用例（那需要外部网络与证书链），所以真实 TLS 这一层由一次**独立检查器
验收**覆盖，结论按证据记录、不与单元用例混算：

- 成功接纳 63 字节 JSON 正文，证书链校验按 WinHTTP 默认策略通过（无 `--insecure` 类开关）；
- 故意坏证书组 3 次全部拒绝，`WinHTTP` 错误码 `12175 / 0x2F8F`（证书校验失败）；
- 重定向 5 例通过，其中包含"第二跳试图降级到明文"的那一例（按禁降级拒绝，不是跟随）；
- 语料 148/0 + 4 WARN、HTTP 32/0 + 5 WARN、预算 20 例无违例；全部 WARN 只是 `kind` 取值或报告里
  未承诺字段的缺失，主控已明确批准保留、不掩盖合约。

这一节只说明"独立检查器这一次验收看到了什么"；它不把 CTest 里的回环用例说成 TLS 单元测试，也不构成
对任意第三方服务器的持续可用性承诺。
