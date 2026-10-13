# 原生侧安全约束（任务栏注入）

本文记录任务栏负载 `deskfeed-payload.dll` 与注入器必须遵守的收尾与安全规则。它解释**为什么**这么
做，以及哪些结论是证明、哪些只是减险。行为口径（命令行、邮箱、显示规则）见
[taskbar-injection.md](taskbar-injection.md)。

## 任务栏线程编组（线程级 `WH_GETMESSAGE`）

XAML 元素有线程亲和性：只有拥有任务栏那棵树的那条线程（`Shell_TrayWnd` 的线程，下面记作 T）能碰
元素。负载把一个动作编组到 T 上的方式，是临时装一个 `WH_GETMESSAGE` 线程钩子、把 T 唤醒、让钩子过程
在 T 上跑，然后**在钩子过程内部自摘**。

为什么是这样而不是别的：

- `SetWindowSubclass` 拒绝跨线程调用；替换 `Shell_TrayWnd` 的窗口过程实测会让 explorer 在大约十秒后
  崩溃。这两条都不采用。
- `UnhookWindowsHookEx` 的文档说明：**从别的线程卸钩，返回之后本次调用仍可能在跑；并发时连"已移除"
  都不保证**。而在钩子过程里自摘属于"未被并发调用"那一支：移除发生在回调返回之前。所以卸钩只有
  一个合法执行者——钩子过程自己。

### 收尾门禁

只有当下面每一条都成立，负载才认为"这次编组已经退休"，才允许注入器卸载模块：

1. **完成事件**——一个手动复位事件，只有"摘掉记录的那次回调"在**最后一步**置位。收尾必须等它本身。
   "钩子句柄已经为空"与"在途计数为零"都**不能**代替它：句柄是在回调开头被取走的，那时执行体、
   `CallNextHookEx` 与它们的析构都还没跑完。
2. **记录确实被摘掉**——回调内的 `UnhookWindowsHookEx` 成功。失败会标记 `still installed`，本次
   绝不声明退休。
3. **在途为零**——完成事件之后不应还有回调在途。
4. **帧已弹掉**——向 T 拥有的窗口发一次跨线程的非排队消息（`SendMessageTimeout(WM_NULL)`）。它只有在
   目标线程回到消息检索路径、执行完一次窗口过程之后才可能返回，而钩子过程是
   `GetMessage`/`PeekMessage` 调用栈里的内层帧。发 ping 前后都核对确认窗口的属主线程与属主进程没有变。

另外两条实现纪律：

- 装钩**之前**先 `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)`（**不是** `PIN`）取一次本
  模块的加载器引用，保证"user32 已经决定调用、但还没进入"的那一瞬间模块仍然映射着；四条门禁都拿到
  之后才归还它。装钩失败也**立刻归还**。因为它不是 PIN，验收判据
  `payload module references at the end: 1` 在成功路径上保持不变。
- 同进程的线程钩子必须 `hMod = NULL`；传模块句柄是未定义行为。
- 安装方把句柄存进原子变量之前，T 就可能进入回调，所以安装完成后置一个"已发布"事件，回调取不到
  句柄时**有界地**等它一次；仍取不到就什么都不做，记录留在链上等 T 下一次取消息。

**超时不是失败，是"这次不退休"**：不卸钩、不归还引用、不报告可卸载。记录留在 T 的链上，T 下一次
取消息时会自己跑完并自摘；稍后的第二次 `--stop` 是幂等的。最坏情况是模块活到 explorer 退出——
不存在"活着但收不回来的引用"，也没有 PIN。

### 不做什么

- 不替换任何窗口过程（尤其不碰 `Shell_TrayWnd`）。
- 不调用 `DispatcherQueueController::ShutdownQueueAsync()`：它的完成回调会回到本模块，实测直接把
  explorer 打崩。丢掉控制器就够了，队列随线程消失。
- 不按镜像名杀进程，不强行卸载。卸载只有一条路：`DeskFeedStop` 返回"可以卸载"，而且调用它的远程
  线程已经退出。

### 自测宿主的托盘目标（只存在于自测构建）

真实注入里 T 恒为 `Shell_TrayWnd` 的线程。**自测宿主**（`deskfeed-probe.exe --feed`）要在自己的进程里
走同一条"投递到 UI 线程"的路，所以 `DeskFeedProbeSetTrayWindow(<宿主自己的岛>)` 会把这个窗口登记成
这一次 attach 的托盘目标，并同时置"已布防"：

- `taskbar_xaml_attach` 在布防状态下**只认**这个窗口：它不在了、不属于本进程、或登记的是空窗口，
  attach 直接失败，**绝不**回退去找真实的 `Shell_TrayWnd`（这条回退正是"往真实任务栏投递"这件事本身，
  而自测宿主的存在意义就是不碰它）；
- `update` 与 `detach` 沿同一次选定的 `g_state.tray` 走，不会中途换目标；
- `DeskFeedProbeSetTrayWindow(nullptr)` 与"没有布防的 `DeskFeedStart`"都会把这个登记清干净，之后
  `attach` 回到与生产完全相同的 `FindWindow` 链；
- 生产 DLL 里这段代码（含登记用的原子变量）**根本不编译**，`attach` 只有 `FindWindow` 那条链。

## `DeskFeedStop` 的返回值与"绝不强卸"

```
DeskFeedStart(void* scope_or_null) -> DWORD   1 已启动 / 2 请求非法 / 3 已在运行 / 4 起线程失败
DeskFeedStop (void* timeout_ms)    -> DWORD   1 可以卸载 / 0 未退休（保留映射）
```

超时值按 `uintptr_t` 编码在参数指针本身里（不解引用），所以远程线程不必在目标进程里另写一个 DWORD。

注入器只在返回 `1` 时 `FreeLibrary`，而且要等调用它的**远程线程退出**之后；返回 `0` 时记一行
`payload stays loaded (not retired: …)`，`--status` 要能表达"已加载但未退休"。这个门禁是硬性的：
先前的实现等一个清理事件、`Sleep` 一小段就无条件卸载，UI 线程一挂住就必然强卸。

## 进程守卫：租约 + 入口记录 + 粘性不确定事件

`DeskFeedStop` 的第一道门不是负载内部的任何标志位，而是**内核对象事实**：注入器（actor）与负载
（Native）共用三个按目标进程命名的对象，名字由唯一的格式化函数生成：

```text
Local\DeskFeed-payload-guard-<pid>-<创建时间 16 位小写 hex>-{mutex,event,record}
```

| 对象 | 用途 |
| --- | --- |
| mutex | 每个动作的**租约**：同一次动作只有一个 actor；崩掉的 actor 让锁变成 abandoned，事实留在内核对象里 |
| event | **手动复位、粘性**的不确定信号：只有 actor 会置，只有能证明干净的 actor 会清；**Native 永不置、永不清** |
| record（4096 字节 mapping） | 40 字节记录：`size/version`（不可变头，一次写入）+ `state/reserved/tid/seq/checksum/reserved2`（actor 快照） |

记录布局、状态值、对象名格式与全部纯函数只有一处定义：`src/payload/guard_record.h`；工具侧只
`include` 它并用别名，不复制布局或常量。两套校验刻意分开：

- **不可变头 `[0,8)`（`size`/`version`）**：Native 每次 `DeskFeedStop` 都只看这一段；
- **actor 快照 `[8,40)`**：整体 FNV-1a + `state` 合法 + `reserved` 归零 + `state` 与 `tid` 自洽，
  由**持租约的 actor** 校验。

Native **不读快照、不读 `checksum`、不要求记录回到 Idle**。理由是自我卸载那条调用：actor 发起
`--stop` 之前按写序已经写下 `PendingNoTid` / `PendingTid{它自己的 Stop 远程线程}`；如果 Native 拿
快照当门，就会把**正在执行的这一次** Stop 判成"有入口在飞"而拒绝——而那正是必须允许走到结论的调用。

`DeskFeedStop` 的门次序（全部在 `payload_request_stop()` **之前**）：

1. `DllMain` 那次拿到的三个内核对象有任何一个没建起来、类型不对、或视图不可读 ⇒ 粘性未知 ⇒ 返回 0；
2. 粘性事件**已置位** ⇒ 返回 0（Native 永不替它清）；
3. 不可变头不认（`size != 40 || version != 1`）⇒ 返回 0；
4. 之后才走原有的门禁（真实线程句柄 + 回收门位）。

"事件没有置位"必须显式区分 `WAIT_TIMEOUT`：`WAIT_FAILED` 是"问不出来"，同样按不可证返回 0。

### Native 只做一次性初始化，之后结构上不是写者

`DllMain(DLL_PROCESS_ATTACH)` 跑在加载器锁里，只允许 kernel32 与栈上定长缓冲：取 `pid` 与进程创建
时间，用唯一的 `guard_format_object_name` 在栈上拼三个名字，然后 `CreateMutexW`（不 own、不 Wait）、
`CreateEventW(TRUE, FALSE)`（不置、不清）、`CreateFileMappingW(PAGE_READWRITE, 4096)`。

- **只有** `CreateFileMappingW` 没有返回 `ERROR_ALREADY_EXISTS`（确实由本进程创建）时才做一次
  `guard_init_new_record`，随后立刻 `UnmapViewOfFile` + 重新 `MapViewOfFile(FILE_MAP_READ)` ——
  之后写它就是访问违例，所以 Native **结构上**不再是记录的写者；
- 对象已存在（协议要求 actor 先 create + init 再 `LoadLibraryW`）时直接拿只读视图；
- 任何 `Create*`/`Map*` 失败、或视图上不可变头不认（含全零）⇒ 粘性未知，**绝不覆盖/复置**那份记录；
  `DllMain` 仍返回 TRUE（不改变加载语义）。零 header 的自愈路径是"最后一个持有者关掉引用之后，
  下一位正常 init 的 actor 重新创建它"。

`DLL_PROCESS_DETACH` 只关本模块自己那 3 个句柄与只读视图，**不等待、不分配、不碰文件/COM/user32**。
这不是"引用泄漏"：唯一合法的卸载路径是 `DeskFeedStop` 返回 1 之后的 `FreeLibrary`，而健康的 actor
自己持着同样的命名对象句柄与视图跨过 `FreeLibrary`，所以记录不会因为本模块归还引用而消失；actor 在
`conclude_clean` 后成对关掉，对象随之消失。

### DETACH 归还本模块自己创建的 4 个内核句柄

除了上面那 3 个守卫引用，负载还自己创建 4 个句柄；它们**都由 `DllMain(DLL_PROCESS_DETACH)` 归还**
（`payload_release_owned_handles` / `hook_post_release_owned_handles`），否则每次
`LoadLibrary`/`FreeLibrary` 都会留下永久句柄：

| 句柄 | 创建处 | DETACH 的归还条件 |
| --- | --- | --- |
| 命名清理事件 `Local\DeskFeed-payload-cleanup-<pid>` | `payload_prepare` | 无条件（本模块是唯一创建者；它只是观测信号，不是卸载门） |
| 负载线程句柄 | `payload_publish_thread`（`DeskFeedStart` 登记；`payload_run` 的兜底 dup） | 无条件 |
| 钩子完成事件 `done` | `hook_post` 的 `ensure_events` | **门已完全退休**才关，否则一个都不关 |
| 钩子发布事件 `published` | 同上 | 同上 |

两条前者的安全前提是同一条合作者纪律：唯一合法的卸载路径是 `DeskFeedStop` 返回 1 **并且**那次远程
调用已经退出（每次 API 都可能是另一次 `CreateRemoteThread`；关键是同一个**串行化** actor 的租约与
退出证明排除了此后与卸载并发的 `Stop`/`GetStatus`）。所以 DETACH 时刻没有调用方在等这两个句柄，
"关掉一个正被并发等待的句柄"结构上不成立。**`DeskFeedStop` 自己绝不提前关**：那次调用返回之后
actor 还会再查一次状态，提前关就是拿一个已关闭、可能被复用的句柄值去做等待。

后两者是**无名事件**，只被钩子回调与收尾线程使用，所以归还条件是"当前状态机证明门已完全退休"：
`completion_expected` 为假只可能来自（a）从来没装成过钩子，或（b）`hook_post_retire` 在等到完成事件、
确认记录已被摘掉、在途为零，并且向**同一条任务栏线程**的窗口发出 `WM_NULL` 并拿到应答、复验过窗口
主人之后才清掉它。这不是拿"钩子句柄为空"或"在途为零"冒充收尾——那两条在回调开头就成立。"完成事件
被提前置位"这种变异会走 `dangling` 分支，一律保留句柄（fail closed）。

进程退出时的 DETACH 走同一段代码：系统本来就回收整张句柄表，那里关掉同样
无害。它不是"平常卸载"的借口，也不改变任何结论——可卸载与否只由 `DeskFeedStop` 给出。

### actor 侧恢复（`WAIT_ABANDONED`）

下一位 actor 拿到 `WAIT_ABANDONED`（上一位带锁退出）时**持锁**读快照，只有能证干净才清事件：

| 快照 | 动作 |
| --- | --- |
| Idle | 清事件 + 重写 Idle(seq+1)，继续 |
| PendingTid{tid}，Toolhelp 确认该 TID 已不在目标进程里 | 清事件 + 重写 Idle，继续 |
| PendingTid{tid}，TID 仍在 / 查询失败 | 保持事件（TID 复用只会 false-retain） |
| PendingNoTid / 头或 checksum 不认 | 保持事件（跨事务的旧 pending 永久保留） |

Toolhelp 只在 actor 侧使用，负载的导入表不新增它。

### 范围限定（不要夸大）

- 本协议只对**本协议版本的 cooperating 参与者**成立：本仓库的 `deskfeed-inject.exe`（含主进程派生）
  与负载 DLL。**不**成立于旧版/第三方工具、调试器、手工 `FreeLibrary`、以及任何不拿租约就卸载的调用方。
- **外部线程入口**（`DllGetClassObject`、XAML tap 回调、探针入口）也不在这套门里：它们不是
  `CreateRemoteThread` 造出来的、起始地址不在本模块。因此**不能**据此宣称"整进程任意第三方线程都安全"。
- 租约按目标 `pid` + 创建时间命名 ⇒ 跨 explorer 不互斥；`Local\` ⇒ 跨会话天然隔离。
- `DeskFeedStop` 返回 1 仍然只表示"既定回收门禁全部通过"，**不是**形式化安全证明；导入表改写那一层
  的残余窗口（下一节）不受这套守卫影响，也不因为有了守卫就可以淡化。

## 导入表改写（composition 诊断规避）的**已接受残余**

`reg_hook` 通过改写各模块导入地址表，让 XAML 读到 `DisableCompositionDiag=1`，从而不去建那套会在
多核下无锁重建的 composition 走查器（见 [taskbar-injection.md](taskbar-injection.md)）。

它有一个**无法在用户态证明**的窗口：任一线程都可能正好停在 `call [IAT]` 与替身第一条指令之间。
`in_flight` 计数与恢复导入表都消不掉这个单指令窗口。

本项目的处理是：

- **明确接受**这个残余，并把它写在这里，而不是默认它已经被证明安全。理由是保持 XAML 平台兼容、且
  注入只发生在用户明确授权的受控实验里；不引入永久 PIN、不引入 shim/JIT trampoline、也不要求重启
  explorer（那会破坏 `stop → 重建 → 再注入` 的正常回路）。
- **尽力减险**：规避只在一次走查期间需要，所以走查一结束就**立刻恢复**全部导入表槽（不是整个常驻
  生命都拦着）；恢复时先把全部槽写回，再等**观测到的**在途调用归零；原函数地址与替身键一直保留到
  最后一次观测到的回调结束；**恢复失败（还有槽指着本模块）时 `DeskFeedStop` 返回 0**，注入器
  不得卸载。
- **替身键的句柄只由本模块持有**：替身应答 `RegOpenKeyExW` 时，交给调用方的必须是**独立的一份**
  HKEY，绝不能是模块自己那一份。调用方按契约会 `RegCloseKey` 它；交出去的那一份被关掉之后，句柄值
  会被系统回收并复用给本模块随后创建的任何内核对象，恢复阶段再关这个旧值就会关到那个被复用的
  对象（实测会关掉钩子门的完成事件，使收尾门永远拿不到“完成”）。这条不是洁癖：`RegCloseKey` 对
  已复用句柄值的破坏性后果，正是一个“看起来只是少一个 `CloseHandle`”的缺陷能在真机上让整条卸载
  门永久失败的原因。
- 因此 **`DeskFeedStop` 返回 1 表示"既定回收门禁全部通过"，不是"绝对安全"的形式化证明**。

自测宿主会并发压这道实现（并发钩子 / 恢复 / 迟到回调），验收的是"实现没有竞态、且不会把无法证明
的东西当成已证明"，而不是"P2 已被证明"。

## XAML 诊断连接：等齐站点再走查，引用按站点数归还

`InitializeXamlDiagnosticsEx` 只是**发起**连接：每个 XAML 核心在自己的线程上稍后才用
`IObjectWithSite::SetSite` 把它的 `IXamlDiagnostics` 交出来。"端口应答"与"站点交出"是两个时刻，
这条时间差有三条后果，都要按下面处理：

- **走查不能只等"有一个站点"**。`taskbar_xaml_attach` 先用 `xaml_tap_wait_for_sites` 等到站点数追平
  本轮应答的端口数（上限仍是原来的 `kSiteAttempts × kSiteDelayMs`）再走查；到限就用当时的站点
  如实继续，日志里写 `diagnostics ready: N site(s) attached before the walk`，**不**按端口数补引用。
  首站点一到就走查时，其余核心的 `SetSite` 还在路上，而走查一开始诊断就要关闭、
  `xaml_tap_stop` 会置上"正在拆除"标志，晚到的站点只能被拒收——那个端口取走的模块引用就留在
  loader 账上（注入器侧表现为卸载时引用数不为 1）。
- **归还的模块引用数只有一个依据：实际交出的站点数**（`xaml_tap_stop`）。一个站点就是一条活的
  连接、正好持一份引用。按"应答端口数"归还已经在真机上把模块从第三次 `FreeLibrary` 里抽走
  （宣告之后一行都没跑），这条不许再试。
- **一次注入只走查一次**：走查结束立即还回走查上报的句柄 → `xaml_tap_stop` → 关连接 → 恢复导入表。
  连接关掉之后"正在拆除"标志不再清除：迟到的站点一律拒收，本模块在这个进程里也不再应答新端口，
  要再连只能重新加载模块。

残余（如实记录，不用放宽判据去掩盖）：某个端口应答了却在窗口内始终不交站点时，本模块会多留一份
引用，卸载时 loader 账上的引用数就大于 1。探针的 `--feed` 判据是硬判 `refs == 1`：这种情况如实
报红。要注意两种"后果"不是一回事——**保持映射**只发生在**门禁没过**时（`DeskFeedStop` 返回 0/未知、
悬挂判据非 0、宿主工作线程未退出：此时根本不计引用数，模块留到本进程结束）；门禁通过后的引用计数
是逐次 `FreeLibrary` 直到失败来数账，`refs != 1` 报红时映射已经被这个量具还掉了，不能读成"替调用方
保住了模块"。这套量具也只是自测夹具，不是生产注入器的清理策略（生产侧只归还自己那一次引用，不为
未知引用盲 `FreeLibrary`）。

## 其它模块入口

- 每个 COM 入口（`DllGetClassObject` / `IObjectWithSite::SetSite` / `IClassFactory::CreateInstance`
  / `IVisualTreeServiceCallback2::OnVisualTreeChange`）都有 C++ 与 SEH 两道边界，且 SEH 小函数不携带
  需要析构的局部对象。异常码与相对模块地址会记进日志。
- 窗口/菜单/悬停/动作发送都跑在负载自己的线程上，收尾时**等各自的线程句柄真的触发**才算退休；
  `running = false` 这类标志位不能当"线程已退出"。
- 唯一的常驻改动是一条工作线程的消息循环、一个通知邮箱的读句柄、以及插入任务栏布局的那一个
  XAML 元素（走查结束后诊断连接立即关闭，导入表立即恢复）。
