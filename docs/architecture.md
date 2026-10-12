# 架构与阶段路线

## 一个主进程，两个显示面

DeskFeed 只有一个取数与状态决策的地方：主进程 `deskfeed.exe`。它负责 HTTP、解析、严格校验、
缓存、到期与整体失效判断，并把同一个 `View` 交给各个显示面。桌面悬浮窗与任务栏组件因此不可能
对同一份快照给出不同解释。

## 四条 seam

| seam | 方向 | 定义处 |
| --- | --- | --- |
| 生产者 → 快照 | HTTP 200 + JSON 正文 | `src/core/snapshot.*`、`src/core/json.*` |
| 快照 → 显示模型 | 校验后的快照 + 现在时刻 | `src/core/health.*`（`HealthState::view()`） |
| 命令 → 运行配置 | 原始命令行参数 | `src/ipc/command.*`、`src/ipc/settings.*`、`src/ipc/*` |
| 主进程 → 显示邮箱 | 最新完整显示帧 | `src/display/frame.*`（线格式）、`src/display/mailbox.*`（共享内存 + 事件）；负载 DLL 尚未实现 |

前三条已经落地；第四条只有计划。

## 模块

| 目录 | 目标 | 内容 |
| --- | --- | --- |
| `src/core/` | `deskfeed_core` | 纯逻辑：严格 JSON、协议模型与校验、健康/缓存/到期状态机、调度、UTF-8 判据。不碰窗口与网络。 |
| `src/platform/` | `deskfeed_platform`、`deskfeed_symbols` | 日志、Win32 帮手、用户范围路径与安全描述符、控制台输出；符号解析是 Phase 2 的平台知识。 |
| `src/net/` | `deskfeed_net` | 唯一的取数入口 `http_get` 与地址处理（解析、相对解析、脱敏）。 |
| `src/ipc/` | `deskfeed_ipc` | 命令行、控制通道（互斥体 + 隐藏窗口 + `WM_COPYDATA`）、设置落盘、进程绑定与代次。 |
| `src/master/` | `deskfeed` | 单实例生命周期、命令事务、取数调度、到期定时器、消息循环。 |
| `src/tools/check/` | `deskfeed-check` | 离线协议检查器与 HTTP 量具。 |
| `tests/` | `deskfeed-tests` | 全部离线用例。 |

## 时钟域

- **墙钟 Unix 毫秒**（`FILETIME`）：只用于快照里的绝对时间——`expiresAtMs`、`receivedAtMs`、
  到期判定。
- **单调时钟**（`GetTickCount64`）：只用于调度与超时——轮询/重试到期、整次取数预算、收尾等待。

两者不得混用；到期时间在参与等待计算时会先换算成相对时长。

## 阶段路线

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| Phase 1 | 协议与严格校验、健康/到期状态机、单主进程与命令事务、绑定与代次、取数规则（含整次墙钟取消）、离线检查器与用例、公开文档/Schema/示例 | 已完成 |
| Phase 2 | 显示邮箱：共享内存 + 通知事件承载“最新完整帧”，负载 DLL `deskfeed-payload.dll` 真嵌入任务栏 XAML，主进程退出/崩溃时自清理；`deskfeed-inject.exe`、`deskfeed-probe.exe` | 进行中 |
| Phase 3 | 桌面悬浮窗：位置/展开/置顶/隐藏/全屏让位、任务栏组件开关、状态即时落盘 | 未开始 |

Phase 2 的硬性约束与安全边界见 [taskbar-injection.md](taskbar-injection.md)。
