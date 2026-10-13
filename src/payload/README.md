# src/payload

任务栏负载 DLL（`deskfeed-payload.dll`）的源码。它被注入 `explorer.exe`，只做一件事：读主进程发布的
**最新显示帧**，把通用读数渲染进任务栏托盘的 XAML 树，并在主进程退出或崩溃时把一切拆干净。

**两个产物、一份源**：生产 `deskfeed-payload.dll` 与自测 `deskfeed-payload-test.dll` 用同一份源列表、
同一清单、同一 CFG/MAP，唯一差异是后者多一个 `DESKFEED_DLL_TEST_API`（见 `payload_flavor.h`）。
探针 ABI 与全部对抗开关只在自测构建里编译；生产 DLL 的导出表只有 `DeskFeedStart` / `DeskFeedStop` /
`DeskFeedGetStatus` 与两个 COM 入口，**没有**任何 `DeskFeedProbe*` / `DeskFeedTest*`。这条由
`deskfeed-native-tests --audit <dll>` 两侧各一条 CTest 断言。

硬边界（验收判据见 [docs/native-safety.md](../../docs/native-safety.md) 与
[docs/taskbar-injection.md](../../docs/taskbar-injection.md)）：

- **不解析任何协议、不发任何网络请求、不判断业务语义**。唯一输入是 `display/mailbox.h` 的
  `DisplayFrameConsumer` 读到的一帧；渲染口径来自 `display/reading.h` 的纯格式化器。
- 映射文件（`deskfeed-payload.map`）与导入表是这条边界的机器可读证据：模块里没有 WinHTTP、没有
  DbgHelp、没有 JSON/快照/协议校验器，`core/` 里只有 `utf8_convert.cpp`（纯转换）被拉进来。

| 文件 | 作用 |
| --- | --- |
| `dllmain.cpp` | 入口与导出。`DllMain` 只记模块句柄、`DisableThreadLibraryCalls`、建清理事件、拿进程守卫对象的引用；DETACH 里按 `payload_guard_release` → `payload_release_owned_handles` → `hook_post_release_owned_handles` 的顺序归还本模块自己创建的 4 个内核句柄（后两个只在钩子门完全退休时才关）。`DeskFeedStart(void* scope)` / `DeskFeedStop(void* timeout_ms)` 都是 `DWORD WINAPI f(void*)`（给 `CreateRemoteThread` 用），外加 COM 的 `DllGetClassObject` / `DllCanUnloadNow`；`DeskFeedProbe*` 一组与两个 `DeskFeedTest*` 只在自测/测试 DLL 构建里存在。 |
| `payload_flavor.h` | **唯一**构建风味开关：`DESKFEED_DLL_TEST_API \|\| DESKFEED_NATIVE_TESTS` ⇒ `DESKFEED_PAYLOAD_PROBE_BUILD`。生产两个都不定义，所以探针入口与全部对抗开关**根本不编译**（不是"启动时复位成正常"）。 |
| `guard_record.h` | 进程守卫的**唯一**共同定义：40 字节记录布局、状态值、对象名格式、FNV-1a、`guard_header_ok` / `guard_snapshot_ok` / `guard_classify` / `guard_recovery` 等纯函数。工具侧（`src/inject/guard.*`）只 `include` 它并用别名，**不复制**布局或常量。 |
| `guard_keeper.h` / `guard_keeper.cpp` | Native 侧持有那三个命名内核对象（租约 mutex / 粘性 event / 记录 mapping）的引用：`DllMain` 里一次性 prepare（只 kernel32 + 栈；只有创建者才做一次 init 后降级只读视图）、`DeskFeedStop` 的第一道 fail-closed 门、DETACH 时只归还自己的引用。 |
| `retire_gates.h` | 回收门位与 `unload_safe` 的**唯一**推导处（纯逻辑，负载与离线用例共用同一份）。 |
| `status_pod.h` | 状态 ABI（定长 POD，无指针跨进程）：`thread_alive` / `unload_safe` / `dangling` / `mailbox_attached` / `producer_*` / `scope` 等。 |
| `probe_api.h` | 自测宿主用的探针 ABI（POD 定义）。只在自测构建里被 include。 |
| `host.h` / `host.cpp` | 负载生命周期：日志、DPI 感知、**先**建调度队列**再**初始化 WinRT 单元、起动作工作线程、挂邮箱、把第一帧渲染进任务栏、消息循环（等通知事件与生产者进程句柄）、按 `feed` 的策略更新或摘块、以及带门禁的拆除顺序。另外提供 DETACH 用的 `payload_release_owned_handles`（归还命名清理事件与负载线程句柄；**不**在 `DeskFeedStop` 里提前关）。 |
| `feed.h` / `feed.cpp` | 显示帧的**消费策略**（纯逻辑，可离线测）：正常收尾与生产者死亡都摘块、生产者活着但帧坏了才沿用上一帧且有界。 |
| `hook_post.h` / `hook_post.cpp` | 把动作编组到任务栏线程并给这条路径一个可证明的收尾：原子取走的单次卸钩、装钩前的非 PIN 自引用、"完成事件 + 非排队消息"两道证明、以及"超时不卸钩、不还引用、不报告可卸载"。DETACH 用的 `hook_post_release_owned_handles` 只在门已完全退休（完成事件 + 同 T 的 `WM_NULL` ack 走过）时才归还那两条无名事件，否则 fail closed。协议说明见 docs/native-safety.md。 |
| `xaml_tap.h` / `xaml_tap.cpp` | 用公开的 XAML 诊断接口（`xamlom.h`）连接进程里的每个 XAML 核心、走查一次任务栏的树并把目标元素解析出来；同时充当 TAP（`DllGetClassObject` + `IObjectWithSite::SetSite`）。模块引用按**实际交出的站点数**归还。四个 COM 入口都有 C++/SEH 双边界。 |
| `reg_hook.h` / `reg_hook.cpp` | 按导入名改写各模块的 IAT，让 XAML 的 composition 走查器读到 `DisableCompositionDiag=1`。走查结束立刻恢复；恢复失败会让 `DeskFeedStop` 返回"未退休"。替身应答 `RegOpenKeyExW` 时交给调用方的是**独立的一份** HKEY（本模块独占自己那一份）：调用方按契约会 `RegCloseKey` 它，共享同一份就会让本模块在恢复阶段去关一个已被回收、可能被复用的句柄值。`DeskFeedProbeHook(1)` 确认的是**安装**（返回 `reg_hook_installed()`），不声称负载自己那次 HKLM 读取被拦截——本模块自己的导入表按纪律从不被补。已接受的残余窗口见 docs/native-safety.md。 |
| `taskbar_xaml.h` / `taskbar_xaml.cpp` | 真正的插入与刷新：把格式化后的读数放进 `SystemTrayFrameGrid`（`Grid` 插一列 `Auto` 并把既有子元素右移，`StackPanel` 插到索引 0）、按托盘给的高度选字号、最多三行、运行期只改已持有 `Run` 的文字与前景色；悬停提示（`TOOLTIPS_CLASS`，最大宽度 420，`TOOLINFOW::cbSize` 按进程内 comctl32 版本候选重试）；右键菜单与双击走动作工作线程发给主进程。托盘目标（T）在生产里恒为 `FindWindow(Shell_TrayWnd)`；自测构建里 `DeskFeedProbeSetTrayWindow` 可以用宿主自己的岛顶替它，**顶替期间任何不可证都直接失败、绝不回退真实托盘**（见 docs/native-safety.md）。 |
| `action_worker.h` / `action_worker.cpp` | 合并（coalesced）、有界的动作工作线程：任务栏线程只置标志，真正的控制通道发送在这里做；收尾时等**线程句柄**触发才算退休。 |
| `control_upstream.h` / `control_upstream.cpp` | 负载 → 主进程的单向客户端（`ControlOp::Refresh` / `ShowWidget`），复用 Phase 1 的控制窗口与线格式，只链编码那一半。 |
| `deskfeed-payload.manifest` | 声明 comctl32 6.0 依赖（悬停提示与任务栏自己的一致）。作为**源文件**加入目标，由 CMake 与它生成的清单合并。 |
