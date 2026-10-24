# 架构与阶段路线

## 一个主进程，两个显示面

DeskFeed 只有一个取数与状态决策的地方：主进程 `deskfeed.exe`。它负责 HTTP、解析、严格校验、
缓存、到期与整体失效判断，并把同一个 `View` 交给各个显示面。桌面悬浮窗与任务栏组件因此不可能
对同一份快照给出不同解释。

桌面悬浮窗还挂一个通知区域（系统托盘）图标：右键弹出的是悬浮窗那一份菜单，左键双击把窗口
叫出来并在已显示时抬到最前，左键单击不动作。图标挂在悬浮窗自己的 HWND 上，不新增线程。

## 默认行为与开关

- 主进程**默认就建两个显示面**：桌面悬浮窗，以及任务栏组件。`[taskbar] component` 缺省为
  `true`，所以任务栏编排启动时就是“开”的目标状态。
- 任务栏编排（`src/master/taskbar_supervisor.*`）本身**不注入**：它只按一份有界的命令行契约异步
  调用 `deskfeed-inject.exe`，真正找任务栏属主、装载/卸载负载的能力都在注入器里。
- 本次运行的开关：`--headless`（不显示悬浮窗、也不碰任务栏）、`--no-inject` / `--no-taskbar`
  （不碰任务栏）、`--hidden`（悬浮窗与通知区域图标都不显示，取数与任务栏组件照旧）。持久关闭
  任务栏组件写 `[taskbar] component=false`。这些开关都**不落盘**，只有 `[taskbar] component` 落盘。
- 没有显示邮箱就不武装编排层：邮箱建不起来时一次注入动作都不会发（不“盲装”负载）。
- 自测与验收必须用隔离实例（`--scope` 等），见 [testing.md](testing.md) 的安全验收流程。

## 四条 seam

| seam | 方向 | 定义处 |
| --- | --- | --- |
| 生产者 → 快照 | HTTP 200 + JSON 正文 | `src/core/snapshot.*`、`src/core/json.*` |
| 快照 → 显示模型 | 校验后的快照 + 现在时刻 | `src/core/health.*`（`HealthState::view()`） |
| 命令 → 运行配置 | 原始命令行参数 | `src/ipc/command.*`、`src/ipc/settings.*`、`src/ipc/*` |
| 主进程 → 显示邮箱 | 最新完整显示帧 | `src/display/frame.*`（线格式）、`src/display/mailbox.*`（共享内存 + 事件）；主进程是唯一生产者，任务栏负载 DLL 只读最新一帧 |

四条 seam 都已经落地：主进程发布显示帧、隔离验收通道 `--frame-mailbox` 可用，任务栏负载
（生产 `deskfeed-payload.dll` + 同源自测 `deskfeed-payload-test.dll`）与注入/探针工具都已实现并有
离线用例。**真机那条路**（真 `explorer.exe` 接受、真 XAML 走查、`--cores 1/2/3` 矩阵）**尚未验收**，
注入必须按 [native-safety.md](native-safety.md) 的纪律逐轮做。

## 模块

| 目录 | 目标 | 内容 |
| --- | --- | --- |
| `src/core/` | `deskfeed_core` | 纯逻辑：严格 JSON、协议模型与校验、健康/缓存/到期状态机、调度、UTF-8 判据。不碰窗口与网络。 |
| `src/platform/` | `deskfeed_platform`、`deskfeed_symbols` | 日志、Win32 帮手、用户范围路径与安全描述符、控制台输出；符号解析（注入器与命令行工具用，负载一行都不碰）。 |
| `src/net/` | `deskfeed_net` | 唯一的取数入口 `http_get` 与地址处理（解析、相对解析、脱敏）。 |
| `src/display/` | `deskfeed_display` | 显示帧的有界线格式（`frame.*`）、显示邮箱（`mailbox.*`）与纯读数格式化器（`reading.*`）：主进程是唯一生产者，任务栏负载 DLL 只读最新一帧。这一层是主进程与负载共用的 seam。 |
| `src/ipc/` | `deskfeed_ipc` | 命令行、控制通道（互斥体 + 隐藏窗口 + `WM_COPYDATA`）、设置落盘、进程绑定与代次。 |
| `src/master/` | `deskfeed` | 单实例生命周期、命令事务、取数调度、到期定时器、消息循环、显示面接线（悬浮窗 + 显示邮箱 + 任务栏编排）、把显示模型发布出去。 |
| `src/payload/` | `deskfeed_payload`、`deskfeed_payload_test` | 注入 `explorer.exe` 的任务栏负载：只读最新显示帧、渲染进任务栏 XAML 树、带可证明的拆除门禁与进程守卫（见 [native-safety.md](native-safety.md)）。两个产物同源，只差 `DESKFEED_DLL_TEST_API`。 |
| `src/inject/`、`src/probe/` | `deskfeed_inject_support`、`deskfeed-inject`、`deskfeed-probe` | 注入/卸载工具与自测宿主（探针自带 Win32 窗口与系统 XAML 岛，**绝不注入 explorer**）。 |
| `src/widget/` | `deskfeed_widget` | 桌面悬浮窗（窗口、主题、文本度量、界面偏好、全屏判定）与通知区域图标；托盘右键复用窗口菜单。 |
| `src/tools/check/` | `deskfeed-check` | 离线协议检查器与 HTTP 量具。 |
| `tests/` | `deskfeed-tests` 等 | 离线用例（见 [testing.md](testing.md)）。 |

## 时钟域

- **墙钟 Unix 毫秒**（`FILETIME`）：只用于快照里的绝对时间——`expiresAtMs`、`receivedAtMs`、
  到期判定。
- **单调时钟**（`GetTickCount64`）：只用于调度与超时——轮询/重试到期、整次取数预算、收尾等待。

两者不得混用；到期时间在参与等待计算时会先换算成相对时长。

## 阶段路线

| 阶段 | 内容 | 实现 | 验证 |
| --- | --- | --- | --- |
| Phase 1 | 协议与严格校验、健康/到期状态机、单主进程与命令事务、绑定与代次、取数规则（含整次墙钟取消）、离线检查器与用例、公开文档/Schema/示例 | 已完成 | 离线用例 |
| Phase 2 | 显示邮箱（主进程发布“最新完整帧”，`--frame-mailbox` 隔离验收通道）、负载 DLL（生产 + 同源自测）、注入/卸载工具、自测宿主、进程守卫与收尾门禁、两侧成品 DLL 审计 | 已完成 | 离线与自测宿主范围已验证；**真机注入尚未验收** |
| Phase 3 | 桌面悬浮窗：位置/展开/置顶/隐藏/全屏让位、任务栏组件开关、状态即时落盘 | 已完成 | 离线与隐藏窗口用例已验证；**未做真实桌面目视验收** |

## 验证现状

“已验证/未验证”的结论只在 [testing.md](testing.md) 记检查点（写清依据的 commit 或产物标识），
本节只给方向，不复制会过期的数字：

- 已独立验证：**HTTPS 真实取数路径**（真实证书端点上的完整取数，含证书错误拒绝与跨跳重定向）。
- 尚未验收：真机 `explorer.exe` 接受、帧生产/消费的 R5 目标修正、干净 checkout 上的 Release
  构建与部署。检查点结论不等于正式发布验收。

Phase 2 的硬性约束与安全边界见 [taskbar-injection.md](taskbar-injection.md) 与
[native-safety.md](native-safety.md)。
