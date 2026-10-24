# tests

| 文件 | 作用 |
| --- | --- |
| `test_support.h` | 极简测试框架：`TEST` / `CHECK` / `CHECK_EQ`，失败抛异常、由入口按用例捕获；不引第三方依赖。 |
| `tests.cpp` | 主进程侧全部离线用例与入口（`--filter <子串>` 只跑匹配的用例，末尾打印通过/失败数，失败时返回 1）。 |
| `native_tests.cpp` | 任务栏负载侧的离线用例与**成品 DLL 审计**入口：显示帧消费策略、钩子门收尾协议、C++/SEH 边界、IAT 改写的恢复路径、回收门位、真 DLL 的 `status` / `glue` / `failclosed`、进程守卫（纯函数 + 自起子进程场景，含 `reload` 的句柄零增长与 `hookevents` 的无名钩子事件归还）。 |
| `tools_tests.cpp` | 注入器与探针工具的离线用例：目标解析、严格 PE 解析、远程调用超时与迟到写者、per-target 租约与真实动作路径（后两者用下面三个夹具）。 |
| `master_supervisor_tests.cpp` | 主进程侧任务栏编排的用例（配 `fake_injector.cpp`，只做假注入）。 |
| `widget_tests.cpp` | 桌面悬浮窗的纯逻辑与偏好用例。 |
| `widget_window_tests.cpp` | 悬浮窗的隐藏窗口门禁：真的建窗口，但强制隐藏（`force_hidden`），所以真实窗口不会出现在桌面上。覆盖建窗与偏好回滚、菜单命令、外部销毁、拖动收尾与捕获被抢走、全屏信号口径、字体度量，以及**悬停与双击的真实消息路径**：合成 `WM_MOUSEMOVE` / `WM_MOUSELEAVE` / `WM_LBUTTONDOWN`+`WM_LBUTTONUP`+`WM_LBUTTONDBLCLK` 驱动生产代码，断言每个条目悬停出**自己**的提示（与任务栏同源的 `ReadingRow::tip`）、提示落在显示器工作区内、同一节点内不重发不移动、空内容不留提示，以及详细↔精简 + 悬停 + 定时器的压力循环里句柄不重建、GDI/USER 对象不增长。通知区域图标也在这里验：合成 `kTrayIconMessage` 的单击事件不写偏好不显示，双击走显示请求并清掉持久隐藏标记，`--hidden` 下双击仍然不绕过；用例把 `tray_icon_available` 关掉，**不登记真实托盘图标**。**不移动光标**（合成的消息就是生产路径：命中判定在窗口里，显示走 `TTM_TRACKACTIVATE`），也不注入 explorer、不碰用户实例。另有一条**右键菜单保活**：在**私有桌面**（`CreateDesktop`，不是输入桌面）上建真正显示的生产 `WidgetWindow`，用通知区域图标的右键回调（`kTrayIconMessage` + `WM_CONTEXTMENU`）开**真的** `TrackPopupMenu` 模态菜单，证明托盘右键复用的就是悬浮窗那一份菜单；把 topmost 位剥掉以定死守卫的“置顶位丢了”那条判据；断言守卫 tick 之后菜单仍是它自己区域的最上层（守卫没把窗口抬到自己菜单之上）、取消菜单不隐藏不退出、菜单开着时递进来的重复右键不叠加第二个菜单、菜单结束后下一 tick 置顶按原判据恢复，并用生产日志核对“菜单开着的那一段没有补置顶、关掉之后才补”。整轮不碰用户桌面、不移动真实光标。 |
| `fake_injector.cpp` | **测试专用**假注入器：主进程任务栏编排用例的目标，不注入任何进程。 |
| `reg_hook_caller.cpp` | **测试专用**夹具 DLL：只按名字导入 `Reg*` 三个入口，让 `reg_hook` 真的去补它的导入槽。 |
| `remote_delay_writer.cpp` | **测试专用**夹具模块：一个"先睡、后写"的导出，用来确定性制造"调用已超时、写入还没发生"的窗口。 |
| `fixture_payload.cpp` | **测试专用**夹具负载 DLL：扮演被注入的负载（真实的生产 ABI，行为开关全走环境变量），只被工具用例装进它自己 spawn 的隐藏夹具主机。 |
| `tools_fixture_host.cpp` | **测试专用**夹具主机 EXE：只装载夹具负载然后等在那里，给动作路径一个真实的远程目标；绝不碰真实 `explorer.exe`。 |
| `probe_producer_child.cpp` | **测试专用**夹具生产者 EXE：`deskfeed-probe.exe` 的"独立帧生产者进程"，给"邮箱属主死掉"那条路径一个真实的、由探针自己 spawn 的属主（先抢真实 scope 仲裁名再发帧）。 |
| `probe_retained_fixture.cpp` | **测试专用**夹具 MODULE：扮演"`DeskFeedStop` 报未退休"的负载，用来咬住"探针绝不 `FreeLibrary` 一个未退休的模块"。 |

**当前状态**：`deskfeed-native-tests` 与其余离线用例在 Debug/Release 下都是绿的。工具用例
`deskfeed-tools-tests` 与 `deskfeed-probe.exe` 的真实 `--feed` 路径目前是**部分阻塞**的：探针宿主的
UI 窗口在收尾之前就被销毁（`kRunMessage` 立刻 `DestroyWindow`），负载向它发的非排队 `WM_NULL`
因此拿不到"同一窗口、同一主人"的证明，`DeskFeedStop` 只能如实报"未退休"。这是**探针侧**的窗口
生命周期问题，与负载的回收门无关；负载侧的完成事件与逐轮净零在 `native_tests` 里独立为绿。
详见 [docs/testing.md](../docs/testing.md)。

覆盖范围：严格 JSON 与深度上限、`expiresAtMs` 的数值整数语义、容量边界、单行/空白判据
（含 VT/FF 等 Unicode 硬换行）、完整替换与失效恢复、独立到期与不续期、首份即过期、调度夹取、
命令行与实例名白名单、控制通道线格式、地址解析/脱敏/点段合并、设置文件保留其它键、
进程绑定的“已退出”拒绝，以及**回环 mock 生产端上的 HTTP 行为**：200 才接纳、有界重定向、
正文超限边读边拒、慢头部与慢滴流都在整次预算附近返回 `timeout`、取消及时、连续取消的取数不堆积、
取消落在一次在途读中间时上下文与缓冲区仍然有效。

全部离线：HTTP 用例只连 `127.0.0.1` 上本进程起的一次性 mock 服务（`ws2_32`），
不读凭据、不连外网、不注入 `explorer.exe`、不碰用户正在跑的实例。
构建与运行方式见 [docs/testing.md](../docs/testing.md) 与 [docs/building.md](../docs/building.md)。
