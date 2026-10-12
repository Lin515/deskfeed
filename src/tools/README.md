# src/tools

| 目录 | 目标 | 内容 |
| --- | --- | --- |
| `check/` | `deskfeed-check` | 离线协议检查器与 HTTP 量具：`--file` / `--stdin` / `--url`，`--report json`。 |
| `check/main.cpp` | — | 参数解析、正文来源（文件/标准输入/真实取数）、报告输出与退出码。 |

它调用的是主进程那份 `http_get` 与 `snapshot_validate`，但不建互斥体、不开窗口、不注入任何进程。
用法与退出码见 [docs/testing.md](../../docs/testing.md)。
