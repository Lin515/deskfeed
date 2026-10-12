# src/inject

注入器 `deskfeed-inject.exe` 的源码：把任务栏负载 `deskfeed-payload.dll` 装进**当前任务栏属主**
explorer.exe、按需撤下来、报告它是否真的在工作。它是主进程侧编排层
（`src/master/taskbar_supervisor.*`）唯一允许"对别的进程动手"的地方。

| 文件 | 作用 |
| --- | --- |
| `main.cpp` | 命令行：`--load` / `--stop` / `--status` / `--watch` 与 `--payload` / `--scope` / `--dry-run` / `--no-unload` / `--interval` / `--once`；日志写 `%LOCALAPPDATA%\DeskFeed\deskfeed-inject.log`（每次截断）。 |
| `actions.h` / `actions.cpp` | 四个动作本身：装载前的状态复查与残留清理、装载并调 `DeskFeedStart`、按门禁撤下、状态查询、watch 的一轮；`unload_decision()` 是卸载门禁的**纯函数**，可以被离线用例直接咬住。 |
| `target.h` / `target.cpp` | 目标解析：任务栏属主（`Shell_TrayWnd`，次选 `Shell_SecondaryTrayWnd`），找不到就拒绝；`--scope` 走实例名白名单。 |
| `remote.h` / `remote.cpp` | 远程原语：进程/模块枚举、`CreateRemoteThread` 的一次调用（返回真实退出码与"是否已退出"）、`LoadLibraryW` 装载、`FreeLibrary` 卸载、以及一块定长缓冲的读写（安全 POD ABI）；未决调用**保留**真实线程句柄，并能在捕获的那个目标进程句柄上 park 到真实结论。 |
| `guard.h` / `guard.cpp` | per-target 租约：命名 mutex + 粘性不确定事件 + 命名记录（布局唯一出处是 `src/payload/guard_record.h`），以及"崩掉的 actor 由下一位解除"的恢复判定。 |
| `pe_export.h` / `pe_export.cpp` | 从**磁盘上的 PE 文件**解析导出函数的 RVA（经节表换算），并**拒绝转发导出**。 |
| `status_pod.h` | 负载生命周期状态的固定 POD（提议版，见下面"状态"）。 |
| `payload_status.h` / `payload_status.cpp` | 把"模块映射着"升级成"可证明在工作"：远程 `DeskFeedGetStatus` + 只读读一遍显示邮箱帧头里的生产者身份。 |

## 命令行与退出码

```text
deskfeed-inject.exe --load   [--payload <DLL>] [--scope <NAME>] [--dry-run]
deskfeed-inject.exe --stop   [--payload <DLL>] [--scope <NAME>] [--no-unload]
deskfeed-inject.exe --status [--payload <DLL>] [--scope <NAME>]
deskfeed-inject.exe --watch  [--scope <NAME>] [--interval <秒>] [--once] [--dry-run]
```

- 目标恒为任务栏属主，**没有**"进程列表里第一个 explorer.exe"这条退路。找不到属主：
  `--load`/`--stop` 退出 1，`--status` 退出 2（"问不出来"）。
- `--payload` 缺省是**与注入器同目录**的 `deskfeed-payload.dll`。
- `--scope` 为空 = 生产实例；非空必须是 1..32 个 `[A-Za-z0-9_-]`。
- `--load`/`--stop`：0 = 成功（或条件已经满足），1 = 失败。
- `--status`：**0 = 负载映射着、且可证明在当前 scope 上工作**；1 = 没装上（或映射着但不工作，
  此时 `--load` 会把它换成新的）；2 = 问不出来，或者映射着的负载有悬挂、无法安全退休。
- `--stop` 在"根本没装载"时返回 **0**：那正是"已经安全撤下"。返回非 0 只代表**真的还有负载
  没能安全撤下来**（主进程据此认为未撤下）。

## 状态：为什么不能只看"模块在不在"

主进程正常收尾或崩溃之后，负载会自己摘块、自己结束线程，但**模块仍然映射着**（要等注入器来卸）。
此时同一 scope 的新主进程启动，如果 `--status` 只按"mapped"回答 0，新主进程就会跳过装载，
任务栏上永远没有东西。所以状态分成五种：

| 状态 | 含义 | `--status` |
| --- | --- | --- |
| `not-mapped` | 模块表里没有这个负载 | 1 |
| `mapped-unknown-abi` | 映射着，但报不出状态（DLL 太旧 / 导出缺失 / 调用失败） | 1（`--load` 会换新） |
| `mapped-inactive` | 映射着但不能证明在工作（线程已结束 / 没挂上邮箱 / 生产者已死 / scope 不对） | 1 |
| `mapped-active` | 线程活着 + 挂上**本 scope** 的邮箱 + 生产者活着且身份一致 | 0 |
| `mapped-retained` | 有悬挂，或线程结束了却报告 `unload_safe=0` | 2（绝不强卸） |

"可证明在工作"依赖三份互相独立的证据：

1. **负载自己报的状态**：导出 `DWORD WINAPI DeskFeedGetStatus(void* pod)` 往调用方在目标进程里
   分配的定长 POD 里写字段（`status_pod.h`）。POD 带 `size`/`abi`，负载不认就写回 `abi=0`；
   没有指针跨越进程边界。
2. **邮箱帧头的生产者身份**：只读地打开 `display_frame_map_name(scope)` 这块共享内存，用 seqlock
   读帧头里的 `producer_pid` / `producer_start_time`，再用 `OpenProcess` + `GetProcessTimes`
   确认那个进程**就是**当初那个（PID 会被复用）。
3. **两者一致**：负载报的生产者身份必须等于帧头里的身份。

`DeskFeedGetStatus` 是**新增的内部 ABI**（不在公开协议里，也不改变任务的显示语义）：144 字节的
定长 POD，唯一出处是负载侧 `src/payload/status_pod.h`，注入器只转发包含 + 别名。读取端仍有已知
缺陷（由 Native 作者修：空 scope、借用生命周期、收尾覆盖 `unload_safe`），所以在它真正可信之前，
`--status` 对读不成/校验不过的 POD 一律 fail closed，报 `mapped-unknown-abi` → 1，绝不拿半份 POD
当结论。

## 装载与卸载的门禁

- `--load` 先查状态：已经 `mapped-active` 就什么都不做（幂等）；`mapped-inactive` /
  `mapped-unknown-abi` 先走一次 `--stop` 把它换成新的；`mapped-retained` 直接拒绝。
- **残留清理**：目标进程之外的 explorer.exe 里如果还挂着一份负载，装载与卸载都会把它收掉。
  这一条与"本轮对目标动不动它"无关：目标是 `mapped-active`（本轮 no-op）、甚至 `mapped-retained`
  （拒绝重装）时，别的 explorer 里的残留照样要按同一套安全门撤下；`--dry-run` 只说明会做什么。
  撤下走的是同一个 `DeskFeedStop` 门禁：只有它报"已退休"且远程线程退出才 `FreeLibrary`，
  超时保留映射、绝不按镜像名杀进程。
- 卸载只有一条路：远程 `DeskFeedStop` 返回 1（"既定回收门禁全部通过"）**并且**调用它的远程线程
  已经退出。返回 0（超时、有悬挂、导入表没恢复）时**保留映射**，稍后重试；`--no-unload` 时
  只让它退出、不卸 DLL。
- **调用任何远程导出之前必须核验"装载的那份就是磁盘那份"**：`MODULEENTRY32W` 的 `modBaseSize`
  只是 SizeOfImage，**尺寸相同不等于同一个 build**（旧模块还在映射、磁盘已被新构建覆盖）。所以在
  `Start` / `Stop` / `GetStatus`（含清理别的 explorer）调用前，都要受限读取远程镜像自己的
  `IMAGE_DOS_HEADER`/`IMAGE_NT_HEADERS`/导出表，核对 Machine、PE32+、SizeOfImage、TimeDateStamp
  与**同名导出的远程真实 RVA**，并证明该 RVA 落在 `IMAGE_SCN_MEM_EXECUTE` 的节内；全部相等才调用，
  而且调用时用**远程确认过的 RVA**（磁盘那个只是候选值）。读不到/字段越界一律拒绝（fail closed），
  绝不退化成"只看尺寸"。
  读取全部有界：`e_lfanew < 64 KiB`、节数 ≤ 96、导出名/函数数 ≤ 4096、名字必须在 256 字节内有
  NUL、转发导出拒绝、每段 `[RVA, RVA+size)` 必须落在 SizeOfImage 内、单次核验总读取 ≤ 1 MiB。
  诚实的限度：TimeDateStamp + SizeOfImage + 同名导出 RVA 映射一致**不是**代码内容哈希，只用于防
  "常见版本漂移"（旧 build 仍映射着）；它不构成对"同一用户自行替换 payload 代码"的认证。
- 装载确认用 `MODULEENTRY32W` 枚举**完整路径**：远程线程退出码是 `DWORD`，x64 的 `HMODULE`
  会被截断，绝不能当基址用。
- **不按镜像名杀进程、不强行卸载、不替换任何窗口过程**：本工具只做"装载/卸载一个 DLL、调它
  导出的三个入口、读写它的一块定长状态缓冲"。

## 租约：同一次动作只有一个 actor，崩掉的 actor 可以被解除

三个命名内核对象（唯一出处 `src/payload/guard_record.h`，两侧都用它的 `guard_format_object_name`
生成，禁止手拼）：

```text
Local\DeskFeed-payload-guard-<pid>-<creationhex>-mutex
Local\DeskFeed-payload-guard-<pid>-<creationhex>-event
Local\DeskFeed-payload-guard-<pid>-<creationhex>-record
```

- `<pid>`/`<creationhex>` 是**目标进程**的实际 PID 与它被捕获时的创建时间。PID 会被复用，所以
  身份判断一律带创建时间；一次动作只开一个进程句柄（`TargetProcess`），不靠后来按 PID 重开。
- **mutex**：一次动作只允许一个 actor；`WAIT_TIMEOUT` = 别人正在动作，**不排队、不强抢**。
  `WAIT_ABANDONED` = 上一位 actor 崩了（内核事实），由本 actor 持锁读记录判定。
- **event**：manual-reset **粘性**不确定事件。置位即"有未证实的入口"，此期间**绝不卸载**；只有
  记录能证明干净时才清掉它。
- **record**：40 字节、无指针、版本化（`size/version` 是不可变头，其余是 actor 快照 + FNV-1a）。
  工具侧**只转发**这份布局 + `static_assert`，不再自带第二份副本。

获取与恢复（全部在持锁状态下）：

| 情形 | 结论 |
| --- | --- |
| 正常持有 + 记录 `Idle` | `Acquired`：清掉良性残留事件，开始本次动作 |
| `WAIT_ABANDONED` + 记录 `Idle` | `Recovered`：清事件 + 把记录重写成 `Idle`，继续 |
| `WAIT_ABANDONED` + `PendingTid{tid}` 且 Toolhelp 确认该 TID **已不在** | `Recovered`（TID 复用只会 false-retain，方向安全） |
| `WAIT_ABANDONED` + `PendingTid` 仍在 / 查询失败 / `PendingNoTid` / 坏 header / 版本或 CRC 不认 | `Unknown`：**置粘性事件**，本次不做任何 Stop/eject/Start |
| 正常持有却看到 `Pending*`（有人违反写序） | `Unknown`：同样 fail closed |

写序（actor 是记录的唯一写者）：`PendingNoTid` 必须在 `CreateRemoteThread` **之前**写入 →
拿到 TID 后写 `PendingTid{tid}` → 远程调用**真实返回**（线程句柄真的 signaled）或**目标进程死亡**
之后才写 `Idle`。

释放：**只有一个** `ReleaseMutex` 调用点，谓词是 `没有任何未决远程调用 && 动作已定论`；不成立时
**不释放**、保持事实。异常、提前 `return`、对象析构的默认行为都是 **abandon（置粘性事件、不释放）**
——没有"离开作用域自动释放"的 RAII。`ReleaseMutex` 的返回值与"是否本线程持有"都要检查，跨线程
释放不算已释放。

范围（诚实边界）：只对本协议版本的 cooperating 参与者成立（本仓库注入器与负载 DLL）。第三方工具、
调试器、手工 `FreeLibrary`、不拿租约的调用者，以及**外部线程入口**（`DllGetClassObject` / XAML
回调 / 探针入口）都不在这套门里——它们仍靠原有 G1/G2 的收尾证明，不因租约而"看起来更安全"。
租约按目标 PID + 创建时间命名 ⇒ **跨 explorer 不互斥**；`Local\` ⇒ 跨会话天然隔离。

> 现状：`guard.h`/`guard.cpp` 这一层（获取、恢复、记录写序、单点释放）已实现并有离线用例
> （含"崩溃 actor 留下的 really-abandoned"与"第二个 actor 的 `WAIT_TIMEOUT` 作为释放可观测证据"，
> 以及两个编译期变异副本证明谓词真的会咬）。`do_load`/`do_stop`/`do_status`/`--watch` 每轮
> **接入**这一层（含把 parking 接进动作）是紧随其后的下一批，在那之前上面"每条路径都受租约保护"
> 还不成立。

## 安全边界

注入 explorer.exe 的风险与限制见 `docs/taskbar-injection.md` 与 `docs/native-safety.md`。
本工具的验收一律先走离线用例与 `deskfeed-probe.exe`，真机实验只能在明确授权后按
"注入 → 观察 → 立刻 stop"逐轮做；每一轮开始前确认上一轮的看门狗与后台进程都已结束。
