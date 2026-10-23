# 本仓库文件索引（面向 Agent）

本文件说明仓库根目录与各子目录的用途；子目录内部文件的说明放在各自的 `README.md`。
面向用户的说明在 [README.md](README.md)。

## 根目录

- `CMakeLists.txt` — 全部构建目标：`deskfeed_base`、`deskfeed_core`（含只做 UTF-8 转换的
  `deskfeed_core_text`）、`deskfeed_platform`、`deskfeed_symbols`、`deskfeed_net`、`deskfeed_ipc`
  （含控制通道线格式的 `deskfeed_control_wire` / `deskfeed_control_wire_send`）、`deskfeed_display`、
  `deskfeed_payload`（`deskfeed-payload.dll`，注入 `explorer.exe`）与 `deskfeed_payload_test`
  （`deskfeed-payload-test.dll`，同源、只多 `DESKFEED_DLL_TEST_API`）、`deskfeed`（主进程）、
  `deskfeed-check`、`deskfeed_inject_support` / `deskfeed-inject`、`deskfeed-probe`、
  `deskfeed_widget`。测试与夹具目标：`deskfeed-tests`、`deskfeed-native-tests`、
  `deskfeed-tools-tests`、`deskfeed-supervisor-tests`、`deskfeed-widget-tests`、
  `deskfeed-widget-window-tests`、`deskfeed-reg-hook-caller`、`deskfeed-test-remote-writer`、
  `deskfeed-test-fixture-payload`、`deskfeed-tools-fixture-host`、`deskfeed-fake-injector`
  （后三者与 remote-writer 一样只服务工具用例：不部署、不安装、不进交付物）。Windows / MSVC /
  x64、C++20、静态 CRT；CTest 注册 8 条用例（含生产/测试两条成品 DLL 审计）。
- `README.md` — 面向用户的说明：运行前提、快速开始、默认行为与验证现状、安全边界、许可。
- `AGENTS.md` — 本文件。
- `LICENSE` — MIT，版权署名 Lin515。
- `.gitignore` — 忽略 `build*/`、`.agent/`、`dist/`、编辑器与调试产物。
- `.github/` — 发布自动化：`workflows/release.yml` 在版本 tag 上构建并发布 x64 Release，
  `scripts/package-release.ps1` 负责打包与 AMD64 校验；两个子目录各有一份 `README.md`。
- `assets/` — 品牌资源（应用图标 SVG/ICO 与 EXE 资源脚本），见 `assets/README.md`。
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
6. **默认运行就会请求注入 `explorer.exe`**：`[taskbar] component` 缺省为 `true`，主进程启动后会
   武装任务栏编排并按需调用 `deskfeed-inject.exe`。所以任何自测/验收都必须带 `--scope <名字>`，
   并带 `--headless`（或 `--no-inject`）——**没有明确授权就不要跑默认实例**。授权之后也必须按
   下面的纪律做：
   - 先用自测宿主（不起窗口、不注入 explorer 的离线用例与 `deskfeed-probe.exe` 的
     `--cores 1/2/3` 矩阵）验，再按“注入 → 观察 → 立刻 stop”逐轮做真机实验；每轮开始前确认上一轮的
     守护进程与后台进程都已结束，日志先复制走（负载日志每注入一次截断一次）。
   - 卸载只有一条合法路径：负载导出的 `DeskFeedStop` 返回“可以卸载”，**并且**调用它的远程线程已经
     退出。返回“未退休”（超时、有悬挂、导入表没恢复）时**绝不** `FreeLibrary`，也不按镜像名杀进程；
     稍后重试即可。门禁与原因见 `docs/native-safety.md`。
   - 不替换任何窗口过程（尤其不碰 `Shell_TrayWnd`），不在拆除时调用
     `DispatcherQueueController::ShutdownQueueAsync()`，不把“清理事件已触发”“`running = false`”
     当成线程已退出。
   - 历史上一例 explorer 在 `Windows.UI.Xaml.dll` 里崩溃时负载并不在那个进程：**既不能归因，也不能
     排除**。不要宣称注入绝对安全；导入表改写那一层还有一个明确接受的残余窗口（见
     `docs/native-safety.md`）。
7. 健康/到期用**墙钟 Unix 毫秒**（快照里的绝对时间），调度与超时用**单调时钟**，两者不得混用。
8. **负载的进程守卫只有一个出处**：`src/payload/guard_record.h`（40 字节记录布局、状态值、对象名
   格式、恢复判定）。工具侧只 `include` 它并用别名，**不得**复制布局或常量。`DeskFeedStop` 返回 0 时
   注入器**必须保留映射**（不得 `FreeLibrary`、不得按镜像名杀进程）；负载侧永不置、永不清那个粘性
   不确定事件，也永不读 actor 快照（`[8,40)`）。这套门只对本协议的合作参与者成立，**不要**据此宣称
   "整进程任意第三方线程都安全"。

## 目录

- `docs/` — 见 `docs/README.md`。
- `examples/` — 见 `examples/README.md`。
- `schema/` — `snapshot-v1.schema.json`，协议约束的机器可读版本。
- `src/` — 见 `src/README.md`（在 `src/` 下按 `core` / `platform` / `net` / `ipc` / `display` /
  `payload` / `master` / `tools` / `inject` / `probe` / `widget` 分层）。
- `tests/` — 见 `tests/README.md`。
