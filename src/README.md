# src

按层分目录，每层一个静态库目标，依赖方向自上而下：`master`/`tools`/`inject`/`probe` → `ipc`、`net` → `core`、`platform`。

| 目录 | 目标 | 内容 |
| --- | --- | --- |
| `core/` | `deskfeed_core`、`deskfeed_core_text` | 纯逻辑：严格 JSON、协议模型与校验、健康/缓存/到期状态机、调度、UTF-8 判据。`core_text` 只放 UTF-8/UTF-16 转换（负载只链它）。 |
| `platform/` | `deskfeed_platform`、`deskfeed_symbols` | 日志、Win32 帮手、用户范围路径与安全描述符、控制台输出；符号解析（注入器/命令行工具用，负载一行都不碰）。 |
| `net/` | `deskfeed_net` | 唯一的取数入口与地址处理。 |
| `ipc/` | `deskfeed_ipc`、`deskfeed_control_wire`、`deskfeed_control_wire_send` | 命令行、控制通道（线格式拆成"只编码"与"编码 + 解码"）、设置落盘、进程绑定。 |
| `display/` | `deskfeed_display` | 显示帧的线格式、显示邮箱（共享内存 + 通知事件）与纯读数格式化器；主进程写、任务栏负载 DLL 只读。 |
| `payload/` | `deskfeed_payload`、`deskfeed_payload_test` | 注入 `explorer.exe` 的任务栏负载：只读最新显示帧，把读数渲染进任务栏 XAML 树，并带可证明的拆除门禁与进程守卫。两个产物同源，只差一个 `DESKFEED_DLL_TEST_API`。 |
| `master/` | `deskfeed` | 主进程入口与生命周期（含任务栏组件的编排）。 |
| `tools/` | `deskfeed-check` | 离线检查器与量具。 |
| `inject/` | `deskfeed_inject_support`、`deskfeed-inject` | 注入/卸载工具：目标解析、远程调用、PE 导出解析、动作与状态探询，以及 per-target 租约（`guard.*`，布局的唯一出处仍是 `payload/guard_record.h`）。 |
| `probe/` | `deskfeed-probe` | 自测宿主：自带 Win32 窗口与系统 XAML 岛，走真实负载探针入口，**绝不注入 explorer**。 |
| `widget/` | `deskfeed_widget` | 桌面悬浮窗（窗口、主题、文本度量、界面偏好、全屏判定）。 |

各目录的文件清单见各自的 `README.md`。
