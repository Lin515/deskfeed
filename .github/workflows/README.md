# workflows

- `release.yml` — 推送版本 tag 时在 `windows-2022` 上做干净的 x64 Release 构建、跑完整 CTest、
  打包并发布 GitHub Release；支持对已有 tag 手动重跑。

触发规则、发布资产与本地打包方式见 [docs/building.md](../../docs/building.md) 的“发布（tag）”一节。
