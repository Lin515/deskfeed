# DeskFeed

DeskFeed 是一个通用的 Windows 桌面信息显示工具：它从**一个**只读 HTTP 端点轮询 JSON 快照，
把其中的条目按生产端给定的顺序与状态显示出来。桌面悬浮窗与任务栏真嵌入组件属于它的显示面。

DeskFeed 不解释业务数值。温度、百分比、币种、剩余量都只是生产端写好的文本，桌面端只负责
布局、主题、字号与失效呈现。

## 当前实现状态

| 能力 | 状态 |
| --- | --- |
| 快照协议 v1、严格校验、容量边界 | 已实现 |
| 健康、缓存、到期状态机 | 已实现 |
| 单主进程 `deskfeed.exe`：单实例、命令通道、URL/PID 事务、生产者绑定、条件退出 | 已实现 |
| 取数：HTTP GET、200 才接纳、重定向与超时规则 | 已实现（**HTTPS 取数没有测试覆盖**） |
| 离线检查器 `deskfeed-check.exe` 与离线用例 `deskfeed-tests.exe` | 已实现 |
| 显示帧与显示邮箱（`src/display/`：主进程 → 显示面的 seam） | 已实现（含 `--frame-mailbox` 隔离验收通道） |
| 任务栏负载：生产 `deskfeed-payload.dll` + 同源自测 `deskfeed-payload-test.dll` | 已实现（Phase 2）；生产导出表只有 `DeskFeedStart/Stop/GetStatus` |
| 进程守卫：租约 + 入口记录 + 粘性不确定事件，`DeskFeedStop` 在不可证时 fail closed | 已实现并有真 DLL 子进程用例；**只对本协议的合作参与者成立**，不宣称"整进程任意线程安全" |
| 注入/卸载工具 `deskfeed-inject.exe`、自测宿主 `deskfeed-probe.exe` | 已实现（工具侧）；**真实注入 explorer 仍需明确授权**，默认不做 |
| 原生侧离线回归 `deskfeed-native-tests.exe`（含成品 DLL 的导入表/导出名审计） | 已实现（生产/测试两侧各一条 CTest） |
| 桌面悬浮窗 `src/widget/` | 已实现并有离线与隐藏窗口用例；**未做真实桌面目视验收** |

主进程目前是无界面后台进程：它取数、校验、维护状态机，并提供命令行控制与状态查询。当前阶段的
验收基线是"核心 + 主进程 + 显示面 + 负载 + 工具"的**离线**用例全绿；真机那条路（真 `explorer.exe`、
真 XAML 走查、`--cores 1/2/3` 矩阵）**尚未**在本轮验收，也**没有**任何默认开关会触发注入。任务栏与
悬浮窗的路线与安全约束见 [任务栏注入计划](<docs/taskbar-injection.md>) 与
[原生侧安全约束](<docs/native-safety.md>)。

## 快速开始

前提：Windows 10/11 x64；生产端提供一个返回快照 JSON 的 HTTP 端点（可先用
[示例生产端](<examples/producer/README.md>)，只需要 Node.js，无第三方依赖）。

构建产物在 `.agent\build\debug\bin`（构建命令见 [docs/building.md](<docs/building.md>)）：

```
:: 1. 启动示例生产端
node examples\producer\minimal-producer.mjs --port 8099

:: 2. 启动主进程（首个实例，指定地址）
.agent\build\debug\bin\deskfeed.exe --url "http://127.0.0.1:8099/snapshot"

:: 3. 另开一个终端查看运行状态
.agent\build\debug\bin\deskfeed.exe --inspect

:: 4. 正常退出
.agent\build\debug\bin\deskfeed.exe --quit
```

离线校验一份快照文件、或直接量一次 HTTP 取数：

```
.agent\build\debug\bin\deskfeed-check.exe --file snapshot.json --report json
.agent\build\debug\bin\deskfeed-check.exe --url "http://127.0.0.1:8099/snapshot" --report json
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [docs/protocol.md](<docs/protocol.md>) | 快照协议 v1：字段、容量边界、严格校验、完整替换、失效与到期 |
| [docs/lifecycle.md](<docs/lifecycle.md>) | 命令行、单实例、控制通道、配置事务、绑定与代次、退出码、`--inspect` |
| [docs/http.md](<docs/http.md>) | 取数规则：墙钟预算、有界重定向、200 才接纳、正文上限、错误分类 |
| [docs/configuration.md](<docs/configuration.md>) | 设置文件、键、默认值与上下限、脱敏策略、隔离开关 |
| [docs/testing.md](<docs/testing.md>) | 构建与测试、离线检查器用法、**安全验收流程**（隔离实例、不碰 explorer） |
| [docs/building.md](<docs/building.md>) | 工具链要求与构建命令 |
| [docs/architecture.md](<docs/architecture.md>) | 四条 seam、模块划分、阶段路线 |
| [docs/taskbar-injection.md](<docs/taskbar-injection.md>) | Phase 2 任务栏真嵌入：当前实现状态、硬性安全约束与已知风险 |
| [docs/native-safety.md](<docs/native-safety.md>) | 原生侧安全：任务栏线程编组的收尾门禁、`DeskFeedStop` 返回值、已接受的残余窗口 |
| [schema/snapshot-v1.schema.json](<schema/snapshot-v1.schema.json>) | 机器可读的协议约束 |
| [examples/producer/](<examples/producer/README.md>) | 不依赖任何私有生产端的最小生产端示例 |

## 安全边界

- Phase 2 的任务栏组件会**向 `explorer.exe` 注入代码**，风险与限制见
  [docs/taskbar-injection.md](<docs/taskbar-injection.md>)。当前版本不做任何注入。
- 命令确认不等于取数成功；IPC 超时只说明没收到确认，**不能**据此认为命令没有执行
  （见 [docs/lifecycle.md](<docs/lifecycle.md) 的“不确定性与返回值”）。
- 地址里可能带上游密钥：日志与 `--inspect` 输出一律去掉 userinfo、查询串与片段；
  实际保存与请求的地址保持原样（见 [docs/configuration.md](<docs/configuration.md>)）。

## 许可

MIT，见 [LICENSE](LICENSE)。
