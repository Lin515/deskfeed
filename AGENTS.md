# 本仓库文件索引（面向 Agent）

本文件说明仓库根目录与各子目录的用途；子目录内部文件的说明放在各自的 `README.md`。
面向用户的说明在 [README.md](README.md)。

## 根目录

- `CMakeLists.txt` — 全部构建目标：`deskfeed_platform`、`deskfeed_symbols`、`deskfeed_core`、
  `deskfeed_net`、`deskfeed_ipc`、`deskfeed_display`、`deskfeed`（主进程）、`deskfeed-check`、
  `deskfeed-tests`。Windows / MSVC / x64、C++20、静态 CRT；CTest 注册 `deskfeed-tests`。
- `README.md` — 面向用户的说明：当前阶段、快速开始、安全边界、许可。
- `AGENTS.md` — 本文件。
- `LICENSE` — MIT，版权署名 Lin515。
- `.gitignore` — 忽略 `build*/`、`.agent/`、编辑器与调试产物。
- `docs/` — 长期有效的协议、生命周期、配置、测试、构建与架构文档，入口见 `docs/README.md`。
- `schema/` — 协议的 JSON Schema。
- `examples/` — 不依赖任何私有生产端的最小生产端示例。
- `src/` — 全部 C++ 源码，按层分目录，入口见 `src/README.md`。
- `tests/` — 离线用例，入口见 `tests/README.md`。

## 硬性规则

1. **协议是冻结的**：任何字段、容量边界、校验与失效规则的改动都要同时更新
   `docs/protocol.md`、`schema/snapshot-v1.schema.json` 与 `src/core/snapshot.h`（容量常量只有这一处）。
2. **公开工程里不得出现私有生产端的名称、凭据、来源语义或领域字段**。桌面端只认协议里的
   `id` / `label` / `value` / `state` / `detail` / `expiresAtMs`。
3. **PID、进程句柄、绑定代次与缓存内容一律不落盘**；设置文件里只放数据源 URL、调度配置，
   以及明确授权的界面偏好（悬浮窗位置/展开/置顶/隐藏、任务栏组件开关）。
4. **任何打印出去的地址都必须先脱敏**（`url_redact`）：去掉 userinfo、查询串与片段。
   实际保存与请求的地址保持原样。
5. **验收与测试必须用隔离实例**：`--scope <名字>`（或 `--test-instance`，或环境变量
   `DESKFEED_SCOPE`）会隔开互斥体、控制窗口类名、设置文件与日志；再加 `--headless`
   保证不碰界面与任务栏。生产实例与测试实例互不干扰。
6. **不要在没有明确授权时注入 `explorer.exe`**。Phase 2 之前该能力根本不存在。
7. 健康/到期用**墙钟 Unix 毫秒**（快照里的绝对时间），调度与超时用**单调时钟**，两者不得混用。

## 目录

- `docs/` — 见 `docs/README.md`。
- `examples/` — 见 `examples/README.md`。
- `schema/` — `snapshot-v1.schema.json`，协议约束的机器可读版本。
- `src/` — 见 `src/README.md`（在 `src/` 下按 `core` / `platform` / `net` / `ipc` / `display` / `master` / `tools` 分层）。
- `tests/` — 见 `tests/README.md`。
