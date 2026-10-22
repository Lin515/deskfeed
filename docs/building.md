# 构建

## 要求

- Windows 10/11 **x64**（任务栏负载必须与 64 位 `explorer.exe` 同位数）。
- **MSVC**（C++/WinRT 头文件与 Windows SDK 随 Visual Studio 提供），CMake ≥ 3.20，Ninja。
- C++20，静态 CRT（`MultiThreaded$<$<CONFIG:Debug>:Debug>`）。
- CMake 里对非 Windows、非 MSVC、非 x64 都会直接报错退出。

## 配置与构建

Windows 上必须**在同一条命令里**先设置代码页再载入 MSVC 环境（Ninja + MSVC 的头文件依赖记录
依赖这一步，否则会丢依赖、改头文件不重编）。

下面的示例要在 **Visual Studio 的 Developer Command Prompt**（或已经配置好的 Developer PowerShell）
里运行：普通 PowerShell/cmd 里 `vcvarsall.bat` 与 `ninja` 都不在 `PATH` 上。若没有用 Developer shell，
请先定位本机 Visual Studio 安装的 `vcvarsall.bat`，在**同一条 cmd** 里用它的完整路径调用（该路径随
VS 版本与安装位置不同，本文不硬编码）。

```powershell
cmd /c 'chcp 65001 >nul && call vcvarsall.bat x64 && cmake -S . -B .agent/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build .agent/build/debug'
```

日常增量构建：

```powershell
cmd /c 'chcp 65001 >nul && call vcvarsall.bat x64 && cmake --build .agent/build/debug'
```

## 产物与交付边界

构建输出统一在 `.agent/build/debug/bin`：

| 产物 | 用途 | 交付 |
| --- | --- | --- |
| `deskfeed.exe` | 主进程：取数、状态机、命令通道、两个显示面 | **是** |
| `deskfeed-payload.dll` | 任务栏负载（注入 `explorer.exe`；同目录另有 `.map`） | **是** |
| `deskfeed-inject.exe` | 注入/卸载工具：主进程按需调用它，必须与主进程同目录 | **是** |
| `deskfeed-check.exe` | 离线协议检查器 / HTTP 量具 | 按需（诊断） |
| `deskfeed-probe.exe` | 自测宿主（自带窗口与 XAML 岛，**绝不注入 explorer**） | 按需（自测） |
| `deskfeed-payload-test.dll` | 同源自测负载（只多 `DESKFEED_DLL_TEST_API`） | 否 |
| `deskfeed-tests.exe`、`deskfeed-native-tests.exe`、`deskfeed-tools-tests.exe`、`deskfeed-supervisor-tests.exe`、`deskfeed-widget-tests.exe`、`deskfeed-widget-window-tests.exe` | 离线用例 | 否 |
| `deskfeed-reg-hook-caller.dll`、`deskfeed-test-remote-writer.dll`、`deskfeed-test-fixture-payload.dll`、`deskfeed-tools-fixture-host.exe`、`deskfeed-fake-injector.exe` | 只服务用例的夹具 | 否 |
| `.agent/build/debug/lib/` | 静态库 | 否 |

交付物只有前三个，且必须放在**同一个目录**：注入器的默认查找路径是与主进程同目录，负载 DLL 的
默认查找路径是与注入器同目录。部署位置通常取 `%LOCALAPPDATA%\DeskFeed\`
（见 [configuration.md](configuration.md)）。

本文与其它构建/测试文档的示例按上面的构建路径写；[README.md](../README.md) 的快速开始按部署后的
调用形式写（只写可执行文件名）。

## 测试

```powershell
ctest --test-dir .agent/build/debug --output-on-failure
.\.agent\build\debug\bin\deskfeed-tests.exe --filter health
```

## 发布（tag）

发布由 [`.github/workflows/release.yml`](../.github/workflows/release.yml) 负责，只对版本 tag 运行：

- tag 形式：`v<major>.<minor>.<patch>`，可带预发布后缀（如 `v1.2.3-rc.1`）；不带前缀 `v` 也接受。
- 推送 tag 后，工作流在 `windows-2022` 上做一次干净的 x64 Release 构建，跑完整 CTest，再发布
  GitHub Release。预发布 tag 生成的 Release 会标记为 pre-release。

发布资产：

| 资产 | 内容 |
| --- | --- |
| `deskfeed-<tag>-x64.zip` | `deskfeed.exe`、`deskfeed-payload.dll`、`deskfeed-inject.exe`、`README.md`、`LICENSE` |
| `SHA256SUMS.txt` | zip 的 SHA-256 |

打包脚本 [`.github/scripts/package-release.ps1`](../.github/scripts/package-release.ps1) 会拒绝任何
不是 AMD64 的交付二进制，zip 只包含上表列出的文件。已有 Release 时重跑工作流会覆盖资产并同步
标题与预发布标记，不会因为 Release 已存在而失败；也可以在 Actions 页面用 `workflow_dispatch` 填一个
已有 tag 重新发布。

本地复现打包（先做一份 Release 构建）：

```powershell
pwsh -NoProfile -File .github/scripts/package-release.ps1 -Tag v1.2.3 -BuildDir .agent/build/release -DistDir .agent/temp/dist
```

**不发布 x86**：任务栏负载必须与 64 位 `explorer.exe` 同位数，x86 构建没有可注入的目标；注入器与
PE 解析也按 AMD64 校验。发布包只允许 x64。

## 注意

- 构建目录里如果还有 `deskfeed.exe` 在跑（例如验收实例），重新链接会报
  `LNK1168: 无法打开 ... 进行写入`。要让它退出，用 `deskfeed.exe --quit --wait-exit-ms 5000`
  并**确认退出码为 0**——0 才代表实例进程真的退出；拿不到这个证据时返回 5，此时不要强杀。
  或者从一开始就让测试运行**自己复制的一份 `bin`**，不要在原地锁住正在用的产物。
  **不要移动正在使用的产物**：IPC 确认不等于进程已经退出。
- 任务栏负载 DLL 一旦被装进 `explorer.exe` 就无法重新链接，改动它之前必须先卸载。
- 目标平台实际运行通过才算通过：构建成功不等于运行成功。
