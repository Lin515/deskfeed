# 构建

## 要求

- Windows 10/11 **x64**（必须与 64 位 `explorer.exe` 同位数，Phase 2 的负载要注入它）。
- **MSVC**（C++/WinRT 头文件与 Windows SDK 随 Visual Studio 提供），CMake ≥ 3.20，Ninja。
- C++20，静态 CRT（`MultiThreaded$<$<CONFIG:Debug>:Debug>`）。
- CMake 里对非 Windows、非 MSVC、非 x64 都会直接报错退出。

## 配置与构建

Windows 上必须**在同一条命令里**先设置代码页再载入 MSVC 环境（Ninja + MSVC 的头文件依赖记录
依赖这一步，否则会丢依赖、改头文件不重编）：

```powershell
cmd /c 'chcp 65001 >nul && call vcvarsall.bat x64 && cmake -S . -B .agent/build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build .agent/build/debug'
```

日常增量构建：

```powershell
cmd /c 'chcp 65001 >nul && call vcvarsall.bat x64 && cmake --build .agent/build/debug'
```

## 产物

```
.agent/build/debug/bin/deskfeed.exe        主进程
.agent/build/debug/bin/deskfeed-check.exe  离线检查器 / HTTP 量具
.agent/build/debug/bin/deskfeed-tests.exe  离线用例
.agent/build/debug/lib/                    静态库
```

文档与 README 里的示例都按这个路径写。部署时通常把 `deskfeed.exe` 放在
`%LOCALAPPDATA%\DeskFeed\`（见 [configuration.md](configuration.md)）。

## 测试

```powershell
ctest --test-dir .agent/build/debug --output-on-failure
.\.agent\build\debug\bin\deskfeed-tests.exe --filter health
```

## 注意

- 构建目录里如果还有 `deskfeed.exe` 在跑（例如验收实例），重新链接会报
  `LNK1168: 无法打开 ... 进行写入`；先让它退出（`--quit`），或把在用的那份产物改名挪开。
- Phase 2 的负载 DLL 一旦被装进 `explorer.exe` 就无法重新链接，改动它之前必须先卸载。
- 目标平台实际运行通过才算通过：构建成功不等于运行成功。
