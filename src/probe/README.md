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
| `host.h` / `host.cpp` | 宿主：调度队列 → 单元 → 隐藏窗口 → XAML 岛（与 shell 同形状的 `SystemTrayFrameGrid` 树）→ `--cores N` 个核心 → 工作线程装载负载并做**唯一一次**走查 → UI 线程解析并应用 → 对抗序列 → 拆除 → 归还引用并打印 `payload module references at the end: 1`。 |
| `mock_producer.h` / `mock_producer.cpp` | 隔离 scope 的 mock 显示帧生产者：负载**不做外部 HTTP、不解析 JSON**，只从这块共享内存读最新完整帧。 |
| `probe_hooks.h` / `probe_hooks.cpp` | 第三方 `WH_GETMESSAGE` 钩子与宿主侧 ack 观测，用来咬住收尾顺序（见下）。 |
| `observer_pod.h` | 负载 `hook_post` 的阶段计数 POD（提议版，等负载作者在 `src/payload/` 冻结同一布局）。只含 **`done` 之前**写下的计数。 |
| `deskfeed-probe.manifest` | 声明支持的 Windows 版本；作为**源文件**交给 CMake 与它生成的清单合并（单独 `/MANIFESTINPUT` 会被链接器拒绝）。 |

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
"负载正处在自己的 `CallNext`"时才泵消息（用阶段 POD 的进入/返回计数判断；否则别的消息检索期间
的泵会把顺序误判成失败）。

- `--third-party-hook-pump`：装第三方钩子并断言 `entries > 0`，且
  `ack.hits_while_third_party_active == 0` 与 `ping_in_frame == 0`。
- `--mutation-early-done`：对抗 failpoint（把 `done` 提前到 `CallNext` 之前）。此时**必须**观察到
  `ack.hits_while_third_party_active > 0` 或 `ping_in_frame > 0` —— 一个硬失败；观察不到就说明
  量具根本没咬住。failpoint 只在探针会话里存在，收尾前强制复位为 0。
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

## 已知前提与未验证项

- 负载侧的 `DeskFeedProbeSetTrayWindow` / `DeskFeedProbeSetInstallDelay` /
  `DeskFeedProbeSetObserver` / `DeskFeedProbeSetCompletionMutation` 与两份 POD
  （`status_pod` / 阶段计数）由负载作者冻结；**缺任何一个时本宿主如实退化为"不跑那一条对抗"并
  在日志里说明**，绝不假装跑过。
- 本宿主**没有**在真实 explorer.exe 上跑过任何一次：真机实验只能在明确授权后按
  "注入 → 观察 → 立刻 stop"逐轮做。
