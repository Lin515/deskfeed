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
陈旧帧、消费者始终绑定原生产者身份）、托盘屏幕配置的解析/规范化与退化规则、主进程屏幕配置的
落盘与配置变化后的重发帧、检查器输出的严格 JSON 与 `--stdin` 的管道输入、
以及条件退出与 `--wait-exit-ms` 的“等到实例真的退出 / 拿不到证据就返回 5 且不发 IPC”两条路径。
全部离线：不读凭据、不连外网、不注入、不碰用户正在跑的实例（回环用例只连 `127.0.0.1`）。

CTest 注册 8 条：

| 用例 | 内容 |
| --- | --- |
| `deskfeed-tests` | 协议/状态机/帧/邮箱/检查器/主进程事务与条件退出 |
| `deskfeed-native-tests` | 负载侧：显示帧消费策略、钩子门收尾协议、C++/SEH 边界、导入表改写、回收门位、真 DLL 的 `status` / `glue` / `failclosed` / 进程守卫 |
| `deskfeed-supervisor-tests` | 主进程侧任务栏编排，配 `deskfeed-fake-injector`（**不注入任何进程**的假注入器） |
| `deskfeed-tools-tests` | 注入器工具：目标解析、远程调用超时与迟到写者、per-target 租约与真实动作路径（配 `deskfeed-test-remote-writer`、`deskfeed-test-fixture-payload`、`deskfeed-tools-fixture-host`、`deskfeed-probe-producer-child` 与 `deskfeed-test-retained-fixture` 五个**测试专用**夹具） |
| `deskfeed-payload-audit` | 成品**生产** DLL 的导入表 / 导出名 / map 审计 |
| `deskfeed-payload-test-audit` | 成品**自测** DLL 的同款审计（探针入口必须仍在） |
| `deskfeed-widget-tests` | 悬浮窗纯逻辑与偏好 |
| `deskfeed-widget-window-tests` | 悬浮窗隐藏窗口门禁（真的建窗口，但强制隐藏） |

一律带 `--no-tests=error`，避免“没有注册用例”被当成绿。

`deskfeed-native-tests` 的过滤器：`--filter feed` / `seh` / `hook` / `hookseq` / `reghook` / `retire` /
`status` / `glue` / `failclosed` / `guard`；`--audit <dll>` 只跑成品审计。`--filter guard` 里的十三条
进程守卫场景各自在**新子进程**里跑
（父进程用 `CreateProcessW` 起自己，`--guard-child <场景>`，断言退出码）：守卫对象名按目标
pid + 创建时间命名、`DllMain` 每个进程只 attach 一次，所以“Native 自己 init / actor 先 init /
粘性事件已置位 / 坏 header / 错类型对象 / 只读零 header / abandoned 恢复 / 引用跨 FreeLibrary /
每轮 reload 不泄漏（含**逐轮净零**、命名清理事件在 `FreeLibrary` 后必须消失）/
无名钩子事件在 DETACH 归还 / R5 登记的托盘目标真的是这次 attach 的 T（`probtray`）以及登记之后它
消失时绝不回退真实 `Shell_TrayWnd`（`probtraydead`）”必须分别是新进程。这些用例只碰**用例自己的
进程**：`LoadLibrary` 成品 DLL、建自己的命名对象、线程、一条**隐藏**的合成 UI 线程（`hookevents`、
`hookseq` 与 `probtray*` 用它把真实的 `WH_GETMESSAGE` 线程钩子装到自己的线程上）与一个自己的显示
邮箱生产者（`probtray*` 用它让负载真的走到 attach）；窗口都是不可见的 STATIC，不注入 explorer、
不起可见 GUI、不碰真实任务栏与光标。

### 句柄判据为什么是“逐轮净零”，不是“装载后 = 稳态 + N”

`reload` 场景曾经断言“一次装载恰好比上一轮稳态多 4 个句柄”（本模块自有的清理事件 + 守卫
mutex/event/mapping）。这条**绝对**断言是环境依赖的错误：Debug 与 Release、甚至同一构建的两次独立
运行里，`LoadLibrary` 那一刻的进程句柄数可以相差十几个（user32/COM/WinRT 的进程级缓存何时长出来、
由哪条线程长出来都不确定）。

现在的判据是**相对**的：首轮之后的每一轮，本轮结尾必须回到上一轮的收尾值，本轮开头也必须回到它。
它对“少一次 `CloseHandle`”仍然是确定性的——删掉任一归还，本轮结尾就比参考高。另外两条“内核对象
消失”断言（命名清理事件消失、三个守卫对象消失）从内核对象一侧独立咬一次。

### 替身键的句柄所有权（一次真实缺陷的回归）

`reg_hook` 通过改写各模块导入表让 XAML 读到 `DisableCompositionDiag=1`。它给调用方的“替身键”
是**本模块自己的** HKEY，而 `RegOpenKeyExW` 的契约是调用方自己 `RegCloseKey`。曾经直接把那一份共享
句柄交出去：调用方一关，这个句柄值就被系统回收，并可能复用给本模块随后创建的任何内核对象；恢复
阶段再 `RegCloseKey` 那个旧值，关掉的就是被复用的对象——实测会关掉钩子门的完成事件，于是之后每一轮
`hook_post_run` 都在入口等待上拿到 `WAIT_FAILED`，永远等不到“完成”，`DeskFeedStop` 只能报未退休。

现在每次应答都给调用方开一份独立句柄，本模块独占自己那一份。`reghook` 用例咬两条：两次打开必须
给出两个不同的句柄；调用方关掉它们之后再创建的内核对象，必须活过 `reg_hook_restore`。

`--mock --hook`（即负载导出的 `DeskFeedProbeHook(1)`）确认的是**安装**：`reg_hook_install` 成功之后
返回 `reg_hook_installed()`。它不声称"负载自己那次 `HKLM\Software\Microsoft\XAML\Debug` 读取被拦截
了"——`reg_hook` 的纪律就是本模块自己的导入表从不被补，所以那次自读在负载进程里永远读不到替身，这
不是功能失败。跨模块的真实拦截由上面 `reghook` 那批经 `deskfeed-reg-hook-caller.dll` 的断言验证。

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
| 显示与任务栏 | 显示帧/邮箱与负载侧收尾协议在**离线与自测宿主**范围内已验证（含进程守卫与两侧 DLL 审计）；托盘屏幕配置的解析、退化规则与主进程落盘/重发帧有离线用例；通知区域图标的单击不动作/双击走显示请求、`--hidden` 不绕过，有隐藏窗口用例（用例里不登记真实托盘图标）；自测宿主的真实 XAML `--feed --cores 1/2/3` 矩阵按"卸载引用必须正好为 1"硬判（见下一节）；**真机多显示器任务栏**已验收（2026-10-24：两块 2560×1440、96 DPI 的显示器上注入真机 explorer，单主屏／仅副屏／双屏／全部关闭四种配置的块落点与增删都正确，屏幕归属靠 `Start` 锚点唯一命中；同一环境还顺带验证了 `--stop` 能真正卸载模块） |

### 负载卸载引用判据（`--feed --cores 2/3`，2026-10-13）

自测宿主的 `--feed` 走的是**生产入口**（`DeskFeedStart` / `DeskFeedStop`），真实 XAML 诊断连接在宿主
自己的进程里跑（连接生命周期见 [native-safety.md](native-safety.md)）。收尾分两步，不要把两步混成
一句：**门禁**（`unload_allowed`）没过时——`DeskFeedStop` 返回 0/未知、悬挂判据非 0、宿主工作线程
没有真的退出——探针**不进**引用计数量具，模块保持映射到本进程结束（`retained/unverifiable`，
不 `FreeLibrary`）；只有门禁全部通过（feed 另要求状态 POD 报 `thread_alive=0 / unload_safe=1 /
dangling=0`）才进 `report_module_references`，它逐次 `FreeLibrary` **直到失败**来数引用，健康路径
必须正好是 1（那一次是探针自己的 `LoadLibrary`），`refs != 1` 就如实报红。注意这套计数量具是在
"数账"、按数到的次数把引用还掉了，所以它的报红**不**表示"替调用方保住了映射"；门禁那道才是。
判据**不放宽**：`refs != 1` 一律红，不按"应答端口数"补还引用。这套量具只是自测夹具，**不是**生产
注入器的清理策略：生产 `deskfeed-inject.exe` 只归还自己那一次 `LoadLibrary` 的引用，绝不为"未知
引用"循环 `FreeLibrary`。

- 绿判据三条同时成立：`DeskFeedStop == 1`、`refs == 1`、模块 `fully unloaded`。
- 证据（2026-10-13；Debug `96B3841D…` 与 Release `64E570E8…` 各自 `--feed --cores 2/3` 各 20 轮）：
  **80/80 绿**，且每轮"应答端口数 = 交出站点数"（`answered on N port(s)` 与 `diagnostics ready:
  N site(s)` 相等），没有一次拒收迟到站点。
- 每轮用独立的 `--scope` 与独立的 `LOCALAPPDATA`：负载日志每注入一次截断，不隔离就会串。

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
