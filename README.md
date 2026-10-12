# DeskFeed

DeskFeed 是一个通用的 Windows 桌面信息显示工具：它从**一个**只读 HTTP/HTTPS 端点轮询 JSON 快照，
把其中的条目按生产端给定的顺序与状态显示出来。桌面悬浮窗与任务栏真嵌入组件是它的两个显示面。

DeskFeed 不解释业务数值。温度、百分比、币种、剩余量都只是生产端写好的文本，桌面端只负责
布局、主题、字号与失效呈现。桌面端只认协议里的 `id` / `label` / `value` / `state` / `detail` /
`expiresAtMs`；运行期的取数只由主进程负责。

## 运行前提

- Windows 10/11 **x64**（任务栏负载要与 64 位 `explorer.exe` 同位数）。
- 一个返回快照 JSON 的 HTTP/HTTPS 端点。
- 不需要安装 VC 运行库（静态 CRT），也不需要任何外部客户端或框架陪着跑：`--pid` 是**可选**的，
  只有想把自己的生命周期绑到某个生产端进程上时才用它。

## 快速开始

交付物是三个文件，放在同一个目录（例如 `%LOCALAPPDATA%\DeskFeed\`）：

```
deskfeed.exe            主进程：取数、状态机、命令通道、两个显示面
deskfeed-payload.dll    任务栏负载（被注入 explorer.exe）
deskfeed-inject.exe     注入/卸载工具（主进程按需调用它）
```

第一次启动时指定数据源地址：

```
deskfeed.exe --url "https://example.com/snapshot"
```

数据源只需返回一份合法快照。最小正文是 `{"version":1,"items":[]}`（合法的“空内容”，不是失败）；
一条带绝对到期时间的例子：

```json
{
  "version": 1,
  "expiresAtMs": 1791825470339,
  "items": [
    {
      "id": "cpu",
      "label": "CPU",
      "value": "54℃",
      "state": "ok",
      "detail": "最近一次采样结果"
    }
  ]
}
```

`expiresAtMs` 可选，是**绝对** Unix 毫秒时刻（不是相对时长）；省略时只按通信与校验的健康状态
判断失效。字段、容量边界与失效规则见 [docs/protocol.md](<docs/protocol.md>)。

查看运行状态、正常退出：

```
deskfeed.exe --inspect
deskfeed.exe --quit
```

调度默认值是 30 s 轮询、5 s 失败重试、8 s 请求超时；可用 `--poll-ms` / `--retry-ms` /
`--timeout-ms` 随 `--url` 一起提交，越界值夹到边界（范围与语义见 [docs/http.md](<docs/http.md>)）。

想把生命周期绑到生产端进程上时（可选，不带 `--pid` 就是独立运行）：

```
deskfeed.exe --url "https://example.com/snapshot" --pid 1234
```

`--quit` 默认只等到 IPC 确认，**确认不等于实例已经退出**；`--quit --wait-exit-ms 5000` 才等到
实例进程真的退出，拿不到这个证据时返回非 0，并且**不补杀、不重开、不另起一个实例**
（见 [docs/lifecycle.md](<docs/lifecycle.md>)）。

### 从源码构建

工具链要求与构建命令见 [docs/building.md](<docs/building.md>)；构建产物在 `.agent/build/debug/bin`。
只想离线校验一份快照、或量一次取数：

```
deskfeed-check.exe --file snapshot.json --report json
deskfeed-check.exe --url "https://example.com/snapshot" --report json
```

## 默认行为与验证现状

- **默认启动就会建立两个显示面**：桌面悬浮窗，以及任务栏组件。`[taskbar] component` 默认为
  `true`，主进程侧的任务栏编排因此会被武装，并按需调用 `deskfeed-inject.exe` 向当前任务栏属主
  进程装载负载。注入不是“需要额外打开”的能力，而是默认行为。
- 不要这两个显示面时：`--headless`（本次运行不显示悬浮窗、也不碰任务栏）、`--no-inject` /
  `--no-taskbar`（不碰任务栏）、`--hidden`（悬浮窗保持隐藏，取数与任务栏组件照旧运行）；
  想持久关掉任务栏组件就写 `[taskbar] component=false`。命令行开关只对本次运行有效，只有
  `[taskbar] component` 会落盘（见 [docs/configuration.md](<docs/configuration.md>)）。
- **自测与验收必须用隔离实例**：`--scope <名字>`（或 `--test-instance`，或环境变量
  `DESKFEED_SCOPE`）会隔开互斥体、控制窗口类名、设置文件与日志；再加 `--headless` 保证不碰界面
  与任务栏，生产实例与测试实例互不干扰。
- 已实现并有离线用例的：协议与严格校验、健康/缓存/到期状态机、单主进程与命令事务、显示帧与
  显示邮箱、负载 DLL（生产与同源自测两侧）、注入/卸载工具、自测宿主、桌面悬浮窗。
- 已独立验证的：**HTTPS 真实取数路径**（真实证书端点上的完整取数，含证书错误拒绝与跨跳重定向）。
  这是某个检查点上的结论，不是干净 checkout 上的发布验收。
- **尚未验收**的：真机 `explorer.exe` 接受、帧生产/消费路径在真实目标进程上的验收、干净 checkout
  上的 Release 构建与部署。检查点与证据记在 [docs/testing.md](<docs/testing.md>)，本文不复述。

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/protocol.md](<docs/protocol.md>) | 快照协议 v1：字段、容量边界、严格校验、完整替换、失效与到期 |
| [docs/lifecycle.md](<docs/lifecycle.md>) | 命令行、单实例、控制通道、配置事务、绑定与代次、退出码、`--inspect` |
| [docs/http.md](<docs/http.md>) | 取数规则：墙钟预算、有界重定向、200 才接纳、正文上限、错误分类 |
| [docs/configuration.md](<docs/configuration.md>) | 设置文件、键与默认值、脱敏策略、隔离开关 |
| [docs/testing.md](<docs/testing.md>) | 构建与测试、离线检查器、**安全验收流程**与检查点证据 |
| [docs/building.md](<docs/building.md>) | 工具链要求、构建命令、产物与交付边界 |
| [docs/architecture.md](<docs/architecture.md>) | 四条 seam、模块划分、阶段路线 |
| [docs/taskbar-injection.md](<docs/taskbar-injection.md>) | 任务栏真嵌入与显示邮箱：实现状态、硬性安全约束、已知风险 |
| [docs/native-safety.md](<docs/native-safety.md>) | 原生侧安全：收尾门禁、进程守卫、`DeskFeedStop` 返回值、已接受的残余窗口 |
| [schema/snapshot-v1.schema.json](<schema/snapshot-v1.schema.json>) | 机器可读的协议约束 |
| [examples/producer/](<examples/producer/README.md>) | 不依赖任何私有生产端的最小生产端示例 |

## 安全边界

- 任务栏组件会**向当前任务栏属主进程（`explorer.exe`）注入代码**，风险与限制见
  [docs/taskbar-injection.md](<docs/taskbar-injection.md>) 与 [docs/native-safety.md](<docs/native-safety.md>)。
  如上一节所述，**默认运行就会武装这条路径**；只有 `--headless` / `--no-inject`（或持久写
  `[taskbar] component=false`）才会让它一次动作都不发。不要在未授权的情况下运行默认实例。
- 负载 DLL 只做显示：它只读主进程发布的显示帧，**不做任何 HTTP、不解释业务语义、不自己判断
  健康与到期**。运行期取数只有主进程一条路径（`deskfeed-check.exe` 是离线量具，不是常驻组件）。
- 命令确认不等于取数成功；IPC 超时只说明没收到确认，**不能**据此认为命令没有执行
  （见 [docs/lifecycle.md](<docs/lifecycle.md>) 的“返回值与不确定性”）。
- 地址里可能带上游密钥：日志与 `--inspect` 输出一律去掉 userinfo、查询串与片段；
  实际保存与请求的地址保持原样（见 [docs/configuration.md](<docs/configuration.md>)）。

## 许可

MIT，见 [LICENSE](LICENSE)。
