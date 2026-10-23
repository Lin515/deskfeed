# src/platform

平台层：日志、Win32 帮手、用户范围的路径与命名、控制台输出，以及 Phase 2 要用的符号解析。

| 文件 | 作用 |
| --- | --- |
| `log.h` / `log.cpp` | UTF-8 日志文件，逐行刷盘，可镜像到标准错误；`%LOCALAPPDATA%\DeskFeed` 下的默认路径。 |
| `win32_util.h` / `win32_util.cpp` | 错误码/HRESULT 文本、窗口描述、窗口查找、模块路径、路径拼接等小帮手。 |
| `paths.h` / `paths.cpp` | 设置/日志/运行目录、实例名（`--scope` 白名单、用户 SID 短哈希）、整文件读写、目录创建。 |
| `screens.h` / `screens.cpp` | 显示器与任务栏窗口枚举：稳定的显示器接口路径、屏幕标签、主屏判定，以及窗口/矩形到屏幕标识的映射。主进程菜单与负载多屏落点共用这一份。 |
| `user_scope.h` / `user_scope.cpp` | 只允许当前用户访问的安全描述符（SDDL），以及“发送方进程属主是否与自身一致”的检查。 |
| `console.h` / `console.cpp` | 标准输出/错误的写入；GUI 子系统进程在需要打印时挂到父进程控制台。 |
| `pdb.h` / `pdb.cpp` | 从微软符号服务器取 PDB。 |
| `symbols.h` / `symbols.cpp` | 用 DbgHelp 解析模块内部符号（不需要 symsrv.dll）。 |
| `taskbar_symbols.h` / `taskbar_symbols.cpp` | `taskbar.dll` 内部地址的缓存文件与读取。 |

`pdb` / `symbols` / `taskbar_symbols` 是 Phase 2 的平台知识，随平台层一起保留，Phase 1 的运行时
不用它们（`deskfeed_symbols` 目标单独成库）。
