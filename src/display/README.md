# src/display

主进程 → 显示面 的那条 seam：主进程是唯一生产者，任务栏负载 DLL 是只读消费者。

| 文件 | 作用 |
| --- | --- |
| `frame.h` / `frame.cpp` | 显示帧的**有界二进制线格式**与编解码：`kind` / `alarm` / `failure` / `reason` 加每条 item 的 `state` / `id` / `label` / `value` / `detail`。载荷上限由 `core/snapshot.h` 的协议容量边界算出；解码逐字段做边界检查，越界整帧拒绝。纯逻辑，不碰 Windows，可离线测。 |
| `mailbox.h` / `mailbox.cpp` | **显示邮箱**：命名共享内存 + 通知事件。生产者写最新完整帧（跨进程 seqlock 提交，先帧体、再帧头与代次、最后 `SetEvent`），消费者只读映射、校验序号/长度/条目数/校验和后解码。帧头带生产者 PID 与进程创建时间，消费者用活动句柄核对以防 PID 复用，并可用该句柄等主进程退出。 |

## 为什么单独成层

- 负载 DLL 要被注入 `explorer.exe`，所以它只能链这一层与平台层：`src/core/` 的 JSON 解析器与
  协议校验器留在主进程，不进 explorer。
- 这条通道是“最新一帧”语义，不排队积压：新帧覆盖旧帧，DLL 重新加载时直接读当前帧。
- 显示帧只承载通用字段，不含任何业务语义；负载 DLL 不解释取值，也不自己做健康/到期判断。

线格式与生命周期的硬约束见 [docs/taskbar-injection.md](../../docs/taskbar-injection.md) 的
“显示邮箱”，以及 [docs/architecture.md](../../docs/architecture.md) 的 seam 表。
