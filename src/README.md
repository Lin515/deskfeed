# src

按层分目录，每层一个静态库目标，依赖方向自上而下：`master`/`tools` → `ipc`、`net` → `core`、`platform`。

| 目录 | 目标 | 内容 |
| --- | --- | --- |
| `core/` | `deskfeed_core` | 纯逻辑：严格 JSON、协议模型与校验、健康/缓存/到期状态机、调度、UTF-8 判据。不碰窗口与网络，可单独测。 |
| `platform/` | `deskfeed_platform`、`deskfeed_symbols` | 日志、Win32 帮手、用户范围路径与安全描述符、控制台输出；符号解析（Phase 2 用）。 |
| `net/` | `deskfeed_net` | 唯一的取数入口与地址处理。 |
| `ipc/` | `deskfeed_ipc` | 命令行、控制通道、设置落盘、进程绑定。 |
| `display/` | `deskfeed_display` | 显示帧的线格式、显示邮箱（共享内存 + 通知事件）与纯读数格式化器；主进程写、任务栏负载 DLL 只读。 |
| `payload/` | `deskfeed_payload` | 注入 `explorer.exe` 的任务栏负载：只读最新显示帧，把读数渲染进任务栏 XAML 树，并带可证明的拆除门禁。 |
| `master/` | `deskfeed` | 主进程入口与生命周期。 |
| `tools/` | `deskfeed-check` | 离线检查器与量具。 |

各目录的文件清单见各自的 `README.md`。
