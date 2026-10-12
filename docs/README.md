# docs 索引

长期有效的设计文档。它们描述**目标行为与已实现行为**，并明确标注哪些尚未验证。

| 文档 | 内容 |
| --- | --- |
| [protocol.md](protocol.md) | 快照协议 v1：字段表、容量边界、严格校验、完整替换、失效/恢复/到期、启动行为 |
| [lifecycle.md](lifecycle.md) | 命令行（含 `--frame-mailbox`）、单实例仲裁、控制通道、配置事务的失败顺序、URL 持久化与 PID 不持久化、绑定代次、退出码、`--wait-exit-ms` 与“等到实例真的退出”、`--inspect` |
| [http.md](http.md) | 取数实现：墙钟预算、手工有界重定向、禁降级、200 才接纳、正文上限、错误分类 |
| [configuration.md](configuration.md) | 设置文件位置与键（含 `[window]` 与 `[taskbar]`）、默认值、原子且不丢其它键的写入、脱敏、隔离开关 |
| [testing.md](testing.md) | 构建与测试、CTest 用例、离线检查器、mock 生产端、**安全验收流程**，以及各检查点的验证证据 |
| [building.md](building.md) | 工具链要求、配置/构建/测试命令、产物与交付边界 |
| [architecture.md](architecture.md) | 四条 seam、模块划分、阶段路线与验证现状 |
| [taskbar-injection.md](taskbar-injection.md) | 任务栏真嵌入与显示邮箱：实现状态、默认行为、硬性安全约束、已知风险 |
| [native-safety.md](native-safety.md) | 原生侧安全约束：任务栏线程编组与收尾门禁、进程守卫（租约/入口记录/粘性事件）、`DeskFeedStop` 返回值、导入表改写的已接受残余、范围限定 |

约定：

- 文档与代码里的实现细节（超时归属、上下限、重定向次数等）必须写成“实现选择”，
  不能写成“用户已确认的协议”。
- 改动协议相关行为时，`docs/protocol.md`、`schema/snapshot-v1.schema.json` 与
  `src/core/snapshot.h` 三处必须同时更新。
- **“已验证/未验证”的结论只在 [testing.md](testing.md) 落检查点**（写清依据的 commit 或产物标识），
  其它文档只引用它，不复述会过期的具体数字；检查点结论不等于正式发布验收。
