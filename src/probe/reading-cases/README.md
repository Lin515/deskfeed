# reading-cases

`deskfeed-probe.exe` 三种**离线量具**用的 fixture。它们只喂一份正文给纯函数（严格快照 v1 校验 +
健康状态机 + 显示帧解码 + 排版），因此**不建窗口、不起线程、不加载负载 DLL、不碰 explorer**：
在没有稳定负载 DLL、也没有注入授权的时候，这一组是唯一能真实跑起来并核对显示口径的用例。

| 文件 | 喂给谁 | 它在钉住什么 |
| --- | --- | --- |
| `generic-content.json` | `--reading` / `--reading-rows` | 一条合法快照的**顺序**与**状态**：CPU 正常、MEM 警告；3 个来源恰好排出 3 行（`per_line=1`）。 |
| `generic-clear.json` | `--reading-rows` | 合法的 `items: []` **立即清空**，是"收到空内容"而不是"取数失败"：`rows=0`、没有 alarm。 |
| `generic-expired.json` | `--reading-rows` | `expiresAtMs` 用**真实 Unix 毫秒**判定：首份快照即已过期，仍**接纳其内容**并整体染红（`alarm=yes`），不是只显示 `ERROR`。 |
| `frame-alarm.json` | `--frame-reading` | 诊断用的**显示帧** JSON：`kind=content` + `alarm=true` + `failure=transport` + `reason`；量具要证明原因出现在条目前面（tip 以 `connect failed` 开头），且条目值保留。 |

## 怎么跑

```text
deskfeed-probe.exe --reading       src/probe/reading-cases/generic-content.json --refresh-ms 30000
deskfeed-probe.exe --reading-rows  src/probe/reading-cases/generic-content.json --refresh-ms 30000
deskfeed-probe.exe --reading-rows  src/probe/reading-cases/generic-clear.json
deskfeed-probe.exe --reading-rows  src/probe/reading-cases/generic-expired.json --refresh-ms 30000
deskfeed-probe.exe --frame-reading src/probe/reading-cases/frame-alarm.json
```

三种量具都**先于**任何窗口/线程/负载 DLL：命令行解析完就返回，所以它们可以随时跑，不受注入门禁
影响。退出码：`0` = 按预期；`1` = 断言失败；`2` = 参数/文件问题（BLOCKED，不是"跑过了"）。

## 预期输出（实测摘要）

| 命令 | 关键输出 |
| --- | --- |
| `--reading-rows generic-content.json --refresh-ms 30000` | `reading: rows=3 stale=no failed=no refresh_ms=30000`，随后 CPU/MEM/DISK 各一行；`lines: auto (per_line=1, max 3) -> 3 line(s)` |
| `--reading generic-content.json` | 逐条读数 + `--- 展开 ---` + `--- 未显示的来源 ---（无）`（与悬浮窗同一份排版） |
| `--reading-rows generic-clear.json` | `rows=0`；`lines: auto … -> 0 line(s)` |
| `--reading-rows generic-expired.json` | `rows=1 stale=no failed=no … alarm=yes`；`row 0: error=1 prefix=<CPU> value=<54C>` |
| `--frame-reading frame-alarm.json` | 两行读数，tip 以 `connect failed` 开头（原因先于条目） |

## 这一组**不能**证明什么

- 它不加载负载、不跑 XAML、不碰真实任务栏，所以**不能**替代 `--mock --cores 1/2/3`、`--feed`
  与对抗矩阵（那些需要稳定 DLL 与显式注入授权）。
- fixture 里的 `expiresAtMs: 1000` 是**过去**的绝对毫秒；它靠"现在"来判定过期，因此这个用例
  在任何时刻运行都必须保持过期。
