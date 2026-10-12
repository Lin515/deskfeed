# src/ipc

命令行、控制通道、设置落盘与进程绑定。

| 文件 | 作用 |
| --- | --- |
| `command.h` / `command.cpp` | 命令行模型与解析（四种冻结形式 + 实施层附加开关）、帮助文本。“显式空地址/空实例名”在这里就被拒绝。 |
| `control_channel.h` / `control_channel.cpp` | 单实例互斥体、隐藏控制窗口与 `WM_COPYDATA` 收发；载荷编码/解码（版本、操作码、发送方 PID、参数表），发送端用有界 `SendMessageTimeoutW`。 |
| `settings.h` / `settings.cpp` | `settings.ini` 的逐行解析与写入：只改自己那一行、保留其它键与注释、临时文件 + 原子替换。 |
| `binding.h` / `binding.cpp` | 生产者进程绑定的两阶段实现：**准备**等待者（失败不碰旧绑定）→ 提交；带绑定代次，等待线程只投递一条带代次的消息。 |

载荷格式、时序与失败顺序见 [docs/lifecycle.md](../../docs/lifecycle.md)。
