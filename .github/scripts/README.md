# scripts

- `package-release.ps1` — 由 `../workflows/release.yml` 调用：从 Release 构建目录取出三个交付
  二进制，校验 PE 机器类型为 AMD64，压成 `deskfeed-<tag>-x64.zip` 并生成 `SHA256SUMS.txt`。

参数与本地用法见 [docs/building.md](../../docs/building.md) 的“发布（tag）”一节。
