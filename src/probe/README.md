# src/probe

自测宿主 `deskfeed-probe.exe` 的源码：在**自己拥有的 XAML 岛**里跑与注入路径完全相同的代码，
不必把负载装进 `explorer.exe`。这条路线上最容易出事的一半（走查任务栏的 XAML 树、往里插元素、
把动作编组到任务栏线程）一旦写错就会把 explorer 带崩，所以它先在普通进程里跑通。

宿主窗口**从不 `ShowWindow`**：所有自测都在隐藏窗口里完成，不移动真实光标、不新建 GUI 会话、
不在桌面上留任何可见痕迹。可观测的判据都走"读自己那棵 XAML 树 / 读探针自己的观测计数"，不靠截图。

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 命令行：三种离线量具（`--reading` / `--reading-rows` / `--frame-reading`）与 `--mock` / `--feed` 两种宿主模式、以及各对抗开关。离线量具**先于**任何窗口/线程/XAML/DLL 返回。 |
| `offline.h` / `offline.cpp` | 离线量具本体：严格快照 v1 走 `snapshot_validate` + health 状态机（含**真实 Unix 毫秒**的 `expiresAtMs` 到期判定），诊断帧 JSON 只用于把 Waiting/Error/整体 alarm 这些显示分支画出来；三路共用 `display/reading.h` 的同一批纯函数。 |
| `host.h` / `host.cpp` | 宿主：调度队列 → 单元 → 隐藏窗口 → XAML 岛（与 shell 同形状的 `SystemTrayFrameGrid` 树）→ `--cores N` 个核心 → 工作线程装载负载并做**唯一一次**走查 → UI 线程解析并应用 → 对抗序列 → 拆除 → 归还引用并打印 `payload module references at the end: 1`。退出码见下。收尾顺序是硬的：**工作线程与额外 XAML 核心都拿到真实 signal** 之后才谈归还引用；任何一项退休事实不成立就**不**归还、**不** `FreeLibrary`，模块保持映射到本进程结束。 |
| `host_gate.h` | 两条**纯决策**：① feature gate —— 命令行开了哪些功能/对抗，就必须在负载里解析到哪些导出，缺任何一条 → `BLOCKED`（exit 2），一条也不跑；**不允许**"日志里说一句就跳过"再以 0 退出。② **卸载门禁** `unload_allowed()` —— 宿主唯一一处 `FreeLibrary` 循环前面的判据（工作线程真退出、停止入口真跑过、悬挂判据为 0，feed 还要状态 POD 报 `thread_alive=0 / unload_safe=1 / dangling=0`），默认全不成立 ⇒ fail closed。 |
| `host_wait.h` | 收尾等待的公共形状 `wait_thread_exit_pumping()`：等到**自己起的**线程真实 signal，期间继续泵消息（死等会把"线程互相推进"变成假死）。"已经 post 过 `WM_CLOSE`"不是证据，所以它只以真实 signal 为准。 |
| `mock_producer.h` / `mock_producer.cpp` | 隔离 scope 的 mock 显示帧生产者：负载**不做外部 HTTP、不解析 JSON**，只从这块共享内存读最新完整帧。 |
| `probe_hooks.h` / `probe_hooks.cpp` | 第三方 `WH_GETMESSAGE` 钩子与宿主侧 ack 观测，用来咬住收尾顺序（见下）。钩子只在**从负载拉到的阶段快照**显示"负载正处在自己的 `CallNext` 帧"时才泵消息。 |
| `observer_pod.h` | 转发包含负载侧 common ABI 头 `payload/probe_api.h` 并起别名（R2 状态 POD、R3 阶段快照）：探针**不自带**第二份布局，头不在包含路径上就直接编译失败。 |
| `reading-cases/` | 离线量具的 fixture 与说明，见 [reading-cases/README.md](reading-cases/README.md)。 |
| `deskfeed-probe.manifest` | 声明支持的 Windows 版本；作为**源文件**交给 CMake 与它生成的清单合并（单独 `/MANIFESTINPUT` 会被链接器拒绝）。 |

## 退出码

| 码 | 含义 |
| ---: | --- |
| `0` | 跑了，且所有观测量都符合预期（对抗用例里"观察到对抗症状"也算符合预期）。 |
| `1` | 跑了，但至少一条断言不成立，或运行期发现某个必需入口不见了。 |
| `2` | **BLOCKED**：命令行要跑的功能缺少必需的负载导出（逐条打印 `BLOCKED: <功能> needs export <符号>`），或参数组合无效——**一条也没跑**。 |

"没跑"绝不允许以 0 返回：验收脚本只能把 0 当"真的跑过并通过"。

## feature gate

每一种模式都有一份必需的导出清单，缺一条即 BLOCKED（清单落在 `host_gate.h`，由离线用例逐条钉住）：

- `--mock` 基线：`DeskFeedProbeStart` / `Sites` / `Watch` / `Run` / `SetHost` / `Stop`；
  `--slow-apply-ms` 另需 `DeskFeedProbeSetSlowApply`。
- `--teardown`：另需 `DeskFeedProbeTeardown` / `DeskFeedProbeDangling`。
- `--hook`：另需 `DeskFeedProbeHook`（自检完必须能还原，否则算失败）。
- `--third-party-hook-pump` / `--install-race` / `--mutation-early-done`：另需
  `DeskFeedProbePostApply` / `Retire` / `SetObserver` / `ObserverStage` / `HookStatus` / `Dangling`；
  `--install-race` 再加 `SetInstallDelay`，`--mutation-early-done` 再加 `SetCompletionMutation`。
- `--hang-ui-ms` / `--slow-apply-ms`（结构自测）：另需 `PostApply` / `Retire` / `Dangling`。
- `--feed`：另需 `DeskFeedProbeSetTrayWindow`（返回 0 视为失败）/ `DeskFeedStart` / `DeskFeedStop` /
  **`DeskFeedGetStatus`（144 字节状态 POD）** / `DeskFeedProbeDangling`。

`--feed` 与结构自测、对抗开关**互斥**（命令组合无效时同样按 BLOCKED 处理）。


## 两种宿主模式（互斥）

- `--mock`（默认，**结构自测**）：`DeskFeedProbeStart` → 等齐本进程的核心 → `DeskFeedProbeWatch`
  （一次走查，**不重试**）→ UI 线程 `DeskFeedProbeSetHost` + `DeskFeedProbeRun`（inline 解析并
  应用）→ `DeskFeedProbeTeardown` → `DeskFeedProbeStop` → 引用计数。
- `--feed`（**生产入口**）：`DeskFeedProbeSetTrayWindow(<宿主窗口>)` 把宿主的岛登记成托盘目标，
  再调**生产入口** `DeskFeedStart(scope)`：负载自己 attach mock 邮箱、自己经真实 `WH_GETMESSAGE`
  编组把帧渲染进宿主的岛，收尾走真实的 `DeskFeedStop`（返回 1 才算可以卸载）。
  这里**不做** inline 应用：XAML 元素有线程亲和性，内容必须由负载编组到 UI 线程。

## 收尾顺序为什么这样测

正确的顺序是：

```
负载回调：执行体 → 析构 → CallNextHookEx（第三方钩子在这里跑，可能泵消息）
          → 负载最后一步 SetEvent(done) → 裸返回
收尾线程：等 done → 向 T 的窗口发一次跨线程 WM_NULL → 只有它成功返回才证明钩子帧弹掉 → 才归还引用
```

所以 **第三方钩子在 `CallNext` 里记下的 sentinel 必须早于 `done`**；而 ack（宿主窗口收到那次
`WM_NULL`）必须**晚于** `done`，并且**不能**在第三方钩子还处于 `CallNext` 帧里时被处理。
第三方钩子因此**先装**（负载的钩子后装 → 负载的 `CallNextHookEx` 会到达它），并且只在
"负载正处在自己的 `CallNext`"时才泵消息。

"负载正处在自己的 `CallNext`"这个判断走 common ABI 的 **R3 拉模型**：负载侧
`DeskFeedProbeSetObserver(1)` 打开阶段记录，宿主把 `DeskFeedProbeObserverStage` 的函数指针交给
第三方钩子，钩子在**自己的回调帧里、同一条线程上**取一次阶段快照（`stage == CallNextEntered`
且 `complete == 0` 且在观测中）。common ABI **刻意没有**任何"回调已返回"的字段：`done` 之后负载
只剩一次裸返回，不许再写字段、调 API、跑析构；"帧真的弹掉了"由宿主自己的 ack 观测证明。

宿主窗口（含作为它子窗口的 ack 窗口）必须**活到这次 ping 之后**：`resolve_ack_window` 只认
"属于同一个 T、同一个进程"的窗口，窗口没了就会回退去 `FindWindow(Shell_TrayWnd)`，被 PID 判据
拒绝，收尾如实报未退休。所以结构自测的 `kRunMessage` **不**结束消息循环——它只跑完"解析 + 应用"；
请工作线程退出、`DestroyWindow` 并让循环结束是工作线程末尾那次 `WM_CLOSE` 的事（它的每一条返回
路径都要发）。同理，`--hang-ui-ms` / `--feed` 的收尾也依赖这个窗口还活着。

## 对抗用例

- `--third-party-hook-pump`：装第三方钩子并断言 `entries > 0`，且
  `ack.hits_while_third_party_active == 0` 与 `ping_in_frame == 0`。第三方钩子的泵**不得吞掉宿主
  自己的退出消息**：`PeekMessage` 取到 `WM_QUIT` 时必须重新入队并停止这一轮泵，否则宿主的消息循环
  会因为队列里再没有退出消息而永久阻塞（这条曾经真的挂死过收尾）。
- `--mutation-early-done`：对抗 failpoint（把 `done` 提前到 `CallNext` 之前）。此时**必须**观察到
  `ack.hits_while_third_party_active > 0` 或 `ping_in_frame > 0` —— 一个硬失败；观察不到就说明
  量具根本没咬住。failpoint 只在探针会话里存在，收尾前强制复位为 0。
  变异路径下负载刻意按"未退休"处理并置 `dangling`，所以这条用例的**成功形态是负向的**：
  退出码非 0、模块保持映射（`retained/unverifiable`），日志里有明确的"咬合"证据；绝不能为了让它
  以 0 退出而把悬挂判据或卸载门禁放宽。
- `--install-race`（默认 300 ms）：把"存好钩子句柄"这一步人为推迟，确定性地制造"回调先于发布
  进入"这个窗口。
- `--hang-ui-ms N` / `--slow-apply-ms N`：UI 线程停止取消息 / 让一次 apply 变慢；此时
  **短超时**的 stop 必须报未退休（0 retained），恢复之后重试必须报可以卸载（1）——
  绝不在这两种情况下强卸模块。

## 引用计数判据

收尾逐个 `FreeLibrary` 直到失败并打印 `payload module references at the end: N`。健康路径
（`--cores 1/2/3`、`--feed`）**都必须是 1**：那一次是探针自己的 `LoadLibrary`，XAML 为每个交出的
site 记的引用已经由负载在解析阶段还清。数到 0 说明多还了一次（模块在它自己的代码还在跑时被抽走），
数到 2 以上说明少还了（模块卸载不掉、下次注入报 already loaded）。

**这一步本身是卸载动作，所以它有前置门禁**（`host_gate.h` 的 `unload_allowed()`，由离线用例逐项
钉住）：只有工作线程**真实退出**、停止入口真的跑过（`--feed` 还要 `DeskFeedStop` 报 1）、悬挂判据报 0、
且 `--feed` 的状态 POD 报 `thread_alive=0 / unload_safe=1 / dangling=0`，才允许数引用。任何一项不
成立一律 **retained/unverifiable**：不归还引用、不 `FreeLibrary`，模块保持映射到本进程结束。
"已经 post 过 `WM_CLOSE`"、"探针的停止入口返回 void"、"引用数看起来是 1"都**不是**退休证明。

`BLOCKED`（exit 2）走另一条更窄的路：那时除了装载与解析导出什么都没跑，所以只归还**本进程自己那
一次** `LoadLibrary` 的引用（打印 `BLOCKED: this process's single load reference -> ...`），**绝不**
循环 `FreeLibrary` 去清"未知的引用"——那会把别的持有者（含第三方钩子）的引用一起抽走。

拆除顺序同样是硬的：额外的 XAML 核心先收到 `WM_QUIT`，再逐个等到**真实** signal，然后才让
`WindowsXamlManager` / `DesktopWindowXamlSource` 在本线程析构（它们的调度队列属于那些线程，线程还
在跑就拆是在别人还在用的对象上动手）。绝不调用 `ShutdownQueueAsync()`（冻结禁用）。

## 已知前提与未验证项

- `--feed` 的树判定除了文字与颜色，还读读数块自己的**垂直落位**：`DeskFeedBlock`（Border）与
  `DeskFeedReading`（TextBlock）的 `VerticalAlignment` 必须是 `Center`，任何"块在树上"的帧都查这一条；
  已知短内容（`waiting-first` / `latest` 的短帧，见 `FeedStep::strict_center`）还要求块比托盘那一行
  矮、按内容高度居中之后两侧都还剩得下正的留白。**隐藏窗口里的 XAML 岛不提交 `Arrange`**：岛的内容
  已经 `Loaded`、`Measure` 也真的算出了内容高度，但 `Arrange` 之后 `ActualHeight` 仍是 0（岛只在
  自己的渲染/布局回合里落位，而隐藏窗口没有那个回合，宿主又不许露可见窗口）。所以"实际排出来的
  上下留白"在这个宿主里读不到，落位这一条只在真的排过时（`BlockGeometry::arranged`）才按实际
  `ActualOffset`/`ActualHeight` 判定；真机上的落位由部署后的真实任务栏验收。

- common ABI（`src/payload/probe_api.h` 的 28/40 字节 POD 与 `src/payload/status_pod.h` 的
  144 字节状态 POD）**只有一个出处**：探针侧只做转发包含 + 别名 + `static_assert`，缺头即编译失败。
- 缺必需导出时不是"退化为不跑那一条"，而是 **BLOCKED（exit 2）**：一条也不跑，逐条打印缺哪个符号
  （见上面的 feature gate）。`--feed` 还要求 144 字节状态 POD 的 `DeskFeedGetStatus` 可用。
- 状态 POD 的可信度**不靠**任何共享指针：宿主在本进程内把 POD 拷进自己的栈上缓冲，并逐项校验
  `size`/`abi`/`width`、scope 是否在容量内以 NUL 结尾、是否落在 32 字符白名单内、是否就是本次
  运行的 scope；任何一条不过就 fail closed，绝不把半份 POD 当结论。
- 本宿主**没有**在真实 explorer.exe 上跑过任何一次：真机实验只能在明确授权后按
  "注入 → 观察 → 立刻 stop"逐轮做。
- 用**真实稳定负载 DLL** 跑过的部分（`--scope` 隔离、隐藏窗口、不注入 explorer）：`--mock` 结构化
  路径全绿——`--cores 1/2/3`（各 site 恰好归还、引用数 1、模块完全卸下）、`--teardown`、
  `--balance-check`、`--dump-only`、`--hook`、`--third-party-hook-pump`、`--install-race`、
  `--hang-ui-ms`（短超时 0、恢复后重试 1）；`--feed` 全帧矩阵同样通过（默认帧、`latest` 的
  baseline → 有界突发 → reorder → omit → 空列表清块 → ERROR 不复活旧值、`warn`/`failure`/`alarm`/
  `restore`、`closed`、`empty`、`delete`、`ownerdeath` 的属主死后清块、
  `waiting-first` 的"首次读数就是合法 Waiting/[]、随后正常快照恢复"，每条都以 `DeskFeedStop -> 1`
  与 `refs = 1` 收尾，`--feed --hang-ui-ms` 的短超时 0 / 重试 1 也拿到。
- **一个仍然是红/不稳的点（在探针侧，判据未放宽）**：
  * `--mutation-early-done` 的咬合是**竞态**：收尾那次 ping 必须正好落在负载的 `CallNext` 帧里才会
    被记进 `ack_in_frame` / `ping_in_frame`，所以判据是"多次运行里观察到过"；单次没咬到会如实打印
    "NOT observable"，观察到一次即证明 failpoint 的位置正确、量具真的会咬。
- 曾经红的 `--feed --cores 2/3` 引用数间歇为 2 已在负载侧修掉：走查前先等本轮应答的端口把站点交齐
  （`xaml_tap_wait_for_sites`，上限仍是原来的 3 秒），之后的 Debug/Release `--cores 2/3` 各 20 轮
  90/90 全绿，每轮"应答端口数 = 交出站点数 = 归还引用数"。
- ownerdeath 需要与探针同目录的辅助生产者可执行文件，retained-fixture 门禁用例需要退休门禁夹具
  DLL；两者是独立的构建目标（源码在 `tests/`），缺了就是真的没跑，不软跳过。
