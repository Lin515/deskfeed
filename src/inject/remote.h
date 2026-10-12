#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
struct RemoteCall {
    bool thread_created = false;
    bool signaled = false;
    DWORD exit_code = 0;
    std::wstring error;
};

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

}  // namespace deskfeed::inject
