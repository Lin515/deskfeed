#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "inject/pe_export.h"

// 对**别的进程**动手的原语：枚举进程与模块、把 DLL 映射进去、调用它的导出、读写一块定长缓冲。
//
// 这一层只属于注入器与探针工具，**不进负载模块**：负载跑在 explorer 里，不参与自己的装载过程。
// 所有"等一次远程线程"的入口都返回真实退出码与"是否已退出"，因为卸载门禁依赖它：
// `DeskFeedStop` 报"可以卸载"之后，还必须确认调用它的**远程线程真的退出了**才能 `FreeLibrary`。
namespace deskfeed::inject {

struct ProcessEntry {
    unsigned long process_id = 0;
    std::wstring image_path;
    // 进程创建时间（FILETIME 的 100ns 计数）。PID 会被复用，所以任何"还是当初那个进程"的判断
    // 都要带上它；0 表示没取到（此时调用方不得把 PID 当成稳定身份）。
    std::uint64_t creation_time = 0;
};

struct ModuleEntry {
    std::wstring path;
    unsigned long long base = 0;
    unsigned long long size = 0;
};

// 一次远程线程调用的结果。
//
// `signaled == false` 表示线程在超时内没有退出——**这不等于"动作没有发生"**：调用方不得据此
// 重试、强卸或认为目标进程状态未变。`exit_code` 只在线程确实退出后有效。
//
// 句柄归属：线程没在等待期内退出时，`thread` **保持打开**（M4：不许用"超时 + CloseHandle"把
// "有一次入口调用没回来"这个事实扔掉）。每一个未决的调用都必须被恰好一次地收尾：
//   * `park_remote_call(...)`——等到真实 signal 或真实目标死亡，拿到**实际**结论；
//   * `release_remote_call(...)`——放弃观察，只关句柄（事实由调用方按"未知"记录）。
// 本结构返回时是移动语义的事实来源，**不要拷贝**（拷贝会让两个对象都认为自己拥有 `thread`）。
struct RemoteCall {
    bool thread_created = false;
    bool signaled = false;       // 线程确实退出（可能是在 parking 期间观察到的）
    bool completed_late = false; // 等待期内没退出，是后来 parking 观察到真实退出的
    bool target_died = false;    // parking 期间捕获的那个目标进程先死了：入口线程随进程消失
    DWORD exit_code = 0;
    unsigned long thread_id = 0; // CreateRemoteThread 拿到的真实 TID（记录 pending 用）
    HANDLE thread = nullptr;     // 未决调用持有的真实线程句柄；见上面的收尾规则
    std::wstring error;
};

// "这次远程调用还没有结论"（线程已创建、但没观察到它退出）。这样的调用必须 park 或显式释放。
bool remote_call_unresolved(RemoteCall const& call);

// "这次调用的结论是实际的"：要么从未执行（CreateRemoteThread 失败），要么线程真的退出了。
// `completed_late` 用来把"迟到但真实的完成"与"从未执行"区分开——两者都不是 Unknown。
bool remote_call_actual(RemoteCall const& call);

// 目标进程句柄：一次动作**只开一次**，并把打开时读到的创建时间当作身份。PID 会被复用，所以
// "还是当初那个进程"的判断一律基于这个真实的 hProcess + creation_time，而不是后来按 PID 重开。
// 句柄带 SYNCHRONIZE，所以 parking 能观察到"目标进程死亡"这个独立事实。
struct TargetProcess {
    HANDLE process = nullptr;
    unsigned long process_id = 0;
    std::uint64_t creation_time = 0;
};

// 打开并捕获身份。`expected_creation_time != 0` 时核对（对不上 = PID 复用/换了一个进程，拒绝）。
// 目标刚刚死亡时仍会成功：句柄有效、创建时间可读，"是否活着"用 `target_process_alive` 单独问。
bool open_target_process(unsigned long process_id, std::uint64_t expected_creation_time,
                         TargetProcess& out, std::wstring& error);
void close_target_process(TargetProcess& target);
bool target_process_alive(TargetProcess const& target);

// Parking 的结论。**除 `ThreadSignaled` 外，一律不得当成"卸载可以继续"**。
enum class ParkResult {
    ThreadSignaled,  // 真实 signal：`signaled/exit_code/completed_late` 已更新，结论是实际的
    TargetDied,      // 捕获的那个目标进程先死了：模块已随进程消失（不等于"已卸载"）
    Interrupted,     // 打断事件置位：结论**仍然未知**，句柄与缓冲一律保留
    NotPending,      // 没有未决的调用（从未创建 / 已经退出）：无需 park
    Error,           // 参数或系统调用失败：结论未知，保留事实
};

// 在**持有租约的同一条线程**上等到真实结论：真实线程 signal 或真实目标进程死亡。`tick_ms` 只是
// 周期性日志/让出时间片的粒度，**不是**放弃等待的期限——到点继续等，绝不 CloseHandle 之后当成功。
// `target_process` 必须是带 SYNCHRONIZE 的捕获句柄（nullptr 直接 Error：不假装能观察目标死亡）。
// `interrupt_event` 非空时置位会让它返回 `Interrupted`（结论仍然未知，调用方必须保留）。
ParkResult park_remote_call(RemoteCall& call, HANDLE target_process, DWORD tick_ms,
                            HANDLE interrupt_event = nullptr);

// 关闭未决调用持有的线程句柄（幂等）。**不等于**把结论变干净：调用方仍须按"未知"记录事实。
void release_remote_call(RemoteCall& call);

std::wstring file_name_of(std::wstring const& path);

// 把路径规整成绝对路径（GetFullPathNameW）并去掉末尾分隔符，用于完整路径比较。失败返回 false。
bool canonical_path(std::wstring const& path, std::wstring& out);
// 完整路径匹配：大小写与末尾分隔符等价，但**绝不**退化成文件名匹配。
//
// 为什么这是一条硬规则：如果按文件名匹配，当 "A\\x.dll" 没有装载、而 "B\\x.dll" 装载了时，
// 会拿 A 的导出 RVA 去 B 的基址上执行——在别人的进程里跳到错误的代码。所以名字相同而目录
// 不同**不算**命中，并且调用方必须把它当成"这个模块没装载"处理。
bool module_path_matches(std::wstring const& loaded_path, std::wstring const& wanted_path);

// 远程缓冲什么时候可以释放：只有"线程从未创建"或"线程确实已经退出"两种。超时（未退出）时
// 目标进程里的 LoadLibraryW 仍可能正在读那个路径串，释放它就是 use-after-free，所以保留
// （短命注入器可以安全地把它泄露到目标进程退出，绝不用 TerminateThread 去抢）。
bool may_release_remote_buffer(bool thread_created, bool thread_signaled);

// `LoadLibraryW` 的结论什么时候算是确定的：只有线程**确实退出**之后。模块表里已经出现这个
// 模块**不能**代替线程退出：那一刻 DllMain 可能还没返回，接着调 Start/FreeLibrary 就是在
// 一个还没有完成初始化的模块里跑代码。
bool load_result_conclusive(bool thread_created, bool thread_signaled);

// 目标进程是不是原生 x64。我们的负载是 x64；32 位（WOW64）任务栏属主装不了它，必须拒绝而不是
// 拿一个错的位数的地址去执行。读不到架构信息时也拒绝（fail closed）。
bool target_is_native_x64(unsigned long process_id, std::wstring& error);

// 按镜像文件名枚举进程（大小写不敏感），并尽力取到完整镜像路径。
bool list_processes_by_image(std::wstring const& image_name, std::vector<ProcessEntry>& found,
                             std::wstring& error);

bool list_modules(unsigned long process_id, std::vector<ModuleEntry>& found, std::wstring& error);

// 按**完整路径**在目标进程的模块表里找一个模块。三态而不是 bool：**"读不到模块表/路径"与
// "模块没装载"是两件完全不同的事**，把前者当成后者就是拿"不可检查"当结论（曾因此把
// access denied 当成 not mapped）。
enum class ModuleLookup {
    Found,     // 完整路径命中
    NotFound,  // 模块表读到了，但没有这个模块（含"同名但不同目录"这种明确不算命中的情形）
    Error,     // 读不到模块表 / 路径无法规整：不可检查
};

ModuleLookup lookup_module_by_full_path(unsigned long process_id, std::wstring const& path,
                                        ModuleEntry& found, std::wstring& error);

// 进程身份：创建时间（FILETIME 的 100ns 计数）。PID 会被复用，所以任何"这个 PID 就是当初那个
// 生产者"的判断都必须带上创建时间。
bool process_creation_time(unsigned long process_id, std::uint64_t& creation_time, std::wstring& error);

// 打开 PID 并核对创建时间。打不开、已退出或时间对不上都返回 false。
bool process_matches_identity(unsigned long process_id, std::uint64_t creation_time);

// 在目标进程里起一次 CreateRemoteThread 并等它退出。`start` 必须是**目标进程里的**函数地址
// （本工具只用会话内基址一致的 kernel32 导出，或刚装载进来的负载模块的导出）。
RemoteCall call_remote_thread(unsigned long process_id, void* start, void* parameter, DWORD wait_ms);

// 装载负载。已经装载（按**完整路径**命中）时把 `already_loaded` 置真并返回 true，**不再装载一次**
// ——重复 `LoadLibraryW` 只会多加一次引用，让后面的卸载永远还不上。
//
// 模块的真实基址一律用 `MODULEENTRY32W` 枚举取得：远程线程的退出码是 DWORD，x64 的 HMODULE
// 会被截断，绝不能拿它当基址用。
//
// **成功只在线程确实退出后成立**：`LoadLibraryW` 的远程线程没有在规定时间内退出时，即使模块名
// 已经出现在模块表里也返回 false（"结论不确定"）——那一刻它的 `DllMain` 可能还没返回，调用方
// 接着调 `DeskFeedStart` 就等于在一个尚未完成初始化的模块里跑代码。此时模块**保持映射**，
// 由调用方按"不确定"处理（稍后 `--status`/`--load` 再来一次，不盲卸）。
bool inject_library(unsigned long process_id, std::wstring const& dll_path, ModuleEntry& loaded,
                    bool& already_loaded, std::wstring& error);

// 卸载：远程 `FreeLibrary`，要求线程退出且退出码非 0，随后用模块枚举确认模块真的不在表里
// （引用数没归零时 `FreeLibrary` 会返回 TRUE 但模块仍然映射）。
bool eject_library(unsigned long process_id, unsigned long long module_base, std::wstring& error);

// 调用一个 `DWORD WINAPI f(void*)` 形式的导出。`parameter` 可以是 nullptr，或目标进程里一块
// 已写好的缓冲地址（状态 POD 就是后者）。
RemoteCall call_remote_export(unsigned long process_id, unsigned long long module_base,
                              unsigned long export_rva, void* parameter, DWORD wait_ms);

// 同一件事，但在**已经打开的**目标进程句柄上做（不再按 PID 重开一次）。这是 parking 需要的形状：
// "这次调用属于哪个进程"由捕获的那个 hProcess 决定，而不是由稍后可能被复用的 PID 决定。
RemoteCall call_remote_export_on(HANDLE target_process, unsigned long long module_base,
                                 unsigned long export_rva, void* parameter, DWORD wait_ms);

// 目标进程里的一块定长缓冲。定长 + 调用方给它填 `size`/`abi` 的头部，是"不跨进程传指针"的
// 安全 ABI：远程函数只写这块缓冲，调用方再读回来。
struct RemoteBuffer {
    unsigned long long address = 0;
    std::size_t bytes = 0;
};

bool remote_alloc(unsigned long process_id, std::size_t bytes, RemoteBuffer& out, std::wstring& error);
void remote_free(unsigned long process_id, RemoteBuffer const& buffer);
bool remote_write(unsigned long process_id, unsigned long long address, void const* data,
                  std::size_t bytes, std::wstring& error);
bool remote_read(unsigned long process_id, unsigned long long address, void* out, std::size_t bytes,
                 std::wstring& error);

// 一次完整的"远程取状态"：把调用方填好的 POD 写进目标进程 → 调导出 → 把 POD 读回来。
// 只做一次，不重试；调用方据 `call` 判断线程是否退出。
bool remote_fetch_pod(unsigned long process_id, unsigned long long module_base, unsigned long export_rva,
                      void* pod, std::size_t pod_bytes, DWORD wait_ms, RemoteCall& call,
                      std::wstring& error);

// ---------------------------------------------------------------------------
// 远程镜像身份核验（T7）
//
// 为什么需要：`MODULEENTRY32W.modBaseSize` 只是 SizeOfImage；**尺寸相同不等于同一个 build**。
// 目标进程里可能映射着一份旧 DLL（上一轮注入后文件被新构建覆盖），此时拿磁盘那份的导出 RVA 加到
// 远程基址上执行，就是跳到另一份 build 的代码。所以调用任何远程导出之前，必须**受限读取**远程
// 那份镜像自己的 PE 元数据与导出表，并证明"同名导出的远程真实 RVA"与磁盘一致。
//
// 这一层只读、不执行：所有读取都经过 `remote_read`，并且
//   * `e_lfanew < 64 KiB`，节数 ≤ 96，导出名/函数数 ≤ 4096；
//   * 每一段 [RVA, RVA+size) 都必须落在 SizeOfImage 之内（u64 比较，防 DWORD 回绕）；
//   * 总读取预算 1 MiB（不逐字节无限扫描）；
//   * 名字串必须在 256 字节内有 NUL（bounded NUL），转发导出一律拒绝；
//   * RVA 0、越界、或不在可执行节里 -> 拒绝。
// 任一步读失败/字段越界都返回 false：**未知即拒绝**，绝不退化成"只看尺寸"。
// ---------------------------------------------------------------------------
struct RemoteImageIdentity {
    unsigned short machine = 0;
    bool pe32_plus = false;
    unsigned long long image_size = 0;   // 远程 SizeOfImage（必须等于模块表报的尺寸）
    unsigned long time_date_stamp = 0;   // 换 build 的强区分位
    unsigned long export_rva = 0;        // 同名导出在**远程**那份镜像里的真实 RVA
    bool export_in_exec_section = false; // 该 RVA 已证明落在可执行节内
};

// 受限读取 `module_base` 那个模块的 PE 头/导出表，找 `export_name` 并核验上列各项。
// `module_size` 来自模块枚举（MODULEENTRY32W.modBaseSize），必须与远程 SizeOfImage 相等。
bool remote_image_identity(unsigned long process_id, unsigned long long module_base,
                           unsigned long long module_size, char const* export_name,
                           RemoteImageIdentity& out, std::wstring& error);

// 纯判定：磁盘那份与远程那份必须是同一个 build，且同名导出映射到同一个 RVA、且落在可执行节内。
// 不碰任何句柄，可离线测。
bool image_identity_matches(PeExportInfo const& disk, RemoteImageIdentity const& remote,
                            std::wstring& error);

// 调用点用的单一入口：读远程身份 + 判定，成功时把**远程确认过的 RVA** 交给调用方（不要用磁盘的）。
bool verify_remote_export(unsigned long process_id, unsigned long long module_base,
                          unsigned long long module_size, char const* export_name,
                          PeExportInfo const& disk, unsigned long& verified_rva, std::wstring& error);

}  // namespace deskfeed::inject
