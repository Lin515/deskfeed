# 最小生产端示例

`minimal-producer.mjs` 是只依赖 Node.js 内置 `http` 的示例生产端：对 `GET /snapshot`
返回一份合法的 v1 快照。它可以用来验证 DeskFeed 的行为，也可以作为写自己生产端的起点。

```
node minimal-producer.mjs --port 8099
```

默认监听 `127.0.0.1:8099`，路径固定为 `/snapshot`。

## 开关（查询参数）

| 参数 | 行为 |
| --- | --- |
| `?clear=1` | 返回 `{"version":1,"items":[]}`：合法清空，不是失败。 |
| `?expired=1` | 带条目，但 `expiresAtMs` 已经是过去时间（验证“首份即过期也接纳内容并染红”）。 |
| `?no-expiry=1` | 不带 `expiresAtMs`（验证“不从快照年龄推断失效”）。 |
| `?ttl-ms=<n>` | 本次快照的存活时间，默认 60000 ms。 |
| `?bad=1` | 返回 `version: 2` 的非法正文（应当整份拒绝）。 |
| `?status=<code>` | 直接返回该状态码（例如 `?status=204`），不带快照正文。 |
| `?slow-headers=<ms>` | 延迟写响应头（慢头部）。 |
| `?slow-chunk=<ms>` | 分块发送正文，每块之间等待（慢滴流）。 |

`slow-*` 两个开关专门给“整次取数的墙钟预算”做压力验证。

## 配合 DeskFeed 使用

```powershell
# 只量一次 HTTP 取数（不建互斥体、不开窗口、不注入）
.\build\bin\deskfeed-check.exe --url "http://127.0.0.1:8099/snapshot" --report json

# 用隔离实例启动主进程（不碰生产实例、不碰任务栏）
.\build\bin\deskfeed.exe --scope demo --headless --url "http://127.0.0.1:8099/snapshot"
.\build\bin\deskfeed.exe --scope demo --inspect
.\build\bin\deskfeed.exe --scope demo --quit
```

`--inspect` 需要 `Start-Process -Wait` 才拿得到真实退出码与输出，见
[docs/testing.md](../../docs/testing.md)。
