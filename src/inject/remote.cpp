#include "inject/remote.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <tlhelp32.h>

#include <cstring>
#include <cwctype>
#include <iterator>
#include <vector>

namespace deskfeed::inject {
namespace {

HANDLE open_target(unsigned long process_id, std::wstring& error) {
    HANDLE process = ::OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
                                       PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                   FALSE, process_id);
    if (process == nullptr) {
        error = L"OpenProcess failed: " + deskfeed::last_error_text();
    }
    return process;
}

std::wstring lower_case(std::wstring text) {    for (wchar_t& character : text) {
        character = static_cast<wchar_t>(::towlower(character));
    }
    return text;
}

std::wstring strip_trailing_separators(std::wstring path) {
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

// 只有 ASCII 的模块名与导出名会走这里（"kernel32.dll" / "LoadLibraryW"），所以不引入 UTF-8
// 转换层：注入器只需要 platform 与它自己的这几个文件。
std::wstring ascii_to_wide(char const* text) {
    std::wstring out;
    for (; text != nullptr && *text != '\0'; ++text) {
        out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return out;
}

// 一个已装载模块的 SizeOfImage（从进程内那份 PE 头读，地址是本进程自己的，安全）。
bool size_of_image(HMODULE module, unsigned long& out) {
    out = 0;
    if (module == nullptr) {
        return false;
    }
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(
        reinterpret_cast<unsigned char const*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    out = nt->OptionalHeader.SizeOfImage;
    return true;
}

// 本进程里一个地址所属的模块、它在该模块里的 RVA，以及该模块的完整路径。
//
// 为什么不能"直接返回本地 GetProcAddress 的地址"：`GetProcAddress(kernel32, "LoadLibraryW")`
// 拿到的往往是 **KERNELBASE.dll** 里的实现（kernel32 把它转发过去），本地地址的所属模块与
// 调用方以为的那个模块根本不是一回事；另外模块基址在目标进程里也可能不同（只有已知 DLL 才保证
// 会话内一致）。所以这里解析出"所属模块的完整路径 + 模块内 RVA"，再去目标进程里按**同一个
// 完整路径**找那个模块、加上同一个 RVA。找不到、路径取不到、或两份模块大小不同（不同版本）
// 一律 fail closed，绝不把本地地址直接当远程地址用。
bool local_address_owner(void const* address, std::wstring& owner_path, unsigned long long& rva,
                         unsigned long& owner_size, std::wstring& error) {
    HMODULE owner = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(address), &owner) == FALSE ||
        owner == nullptr) {
        error = L"cannot determine which module owns the local address";
        return false;
    }

    std::wstring const owner_path_full = deskfeed::module_file_path(owner);
    if (owner_path_full.empty()) {
        error = L"cannot read the full path of the module that owns the local address";
        return false;
    }
    owner_path = owner_path_full;

    rva = reinterpret_cast<unsigned long long>(address) - reinterpret_cast<unsigned long long>(owner);
    if (!size_of_image(owner, owner_size)) {
        error = L"cannot read SizeOfImage of " + owner_path;
        return false;
    }
    return true;
}

std::uint64_t file_time_value(FILETIME const& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

RemoteCall run_remote_thread(HANDLE process, void* start, void* parameter, DWORD wait_ms) {
    RemoteCall result;
    HANDLE thread = ::CreateRemoteThread(process, nullptr, 0,
                                         reinterpret_cast<LPTHREAD_START_ROUTINE>(start), parameter,
                                         0, nullptr);
    if (thread == nullptr) {
        result.error = L"CreateRemoteThread failed: " + deskfeed::last_error_text();
        return result;
    }
    result.thread_created = true;

    if (::WaitForSingleObject(thread, wait_ms) != WAIT_OBJECT_0) {
        // 线程没在期限内退出：不关线程句柄就直接返回会让句柄泄漏，关掉它不会结束线程。
        result.error = L"the remote thread did not exit within the timeout";
        ::CloseHandle(thread);
        return result;
    }

    DWORD exit_code = 0;
    ::GetExitCodeThread(thread, &exit_code);
    ::CloseHandle(thread);
    result.signaled = true;
    result.exit_code = exit_code;
    return result;
}

// 把一个本地解析出来的导出地址换算成目标进程里的地址：本地解析出"所属模块完整路径 + 模块内
// RVA"，再按同一个完整路径在目标进程的模块表里找那个模块，加同一个 RVA。
bool remote_address_of_export(unsigned long process_id, char const* module_name,
                              char const* export_name, void*& out, std::wstring& error) {
    out = nullptr;
    std::wstring const module = ascii_to_wide(module_name);
    HMODULE const local = ::GetModuleHandleW(module.c_str());
    if (local == nullptr) {
        error = L"cannot locate " + module + L" in this process";
        return false;
    }

    void* const local_address = reinterpret_cast<void*>(::GetProcAddress(local, export_name));
    if (local_address == nullptr) {
        error = L"GetProcAddress failed: " + ascii_to_wide(export_name);
        return false;
    }

    std::wstring owner_path;
    unsigned long long rva = 0;
    unsigned long owner_size = 0;
    if (!local_address_owner(local_address, owner_path, rva, owner_size, error)) {
        return false;
    }

    std::vector<ModuleEntry> modules;
    std::wstring list_error;
    if (!list_modules(process_id, modules, list_error)) {
        error = L"cannot read the target module list: " + list_error;
        return false;
    }

    for (ModuleEntry const& entry : modules) {
        if (!module_path_matches(entry.path, owner_path)) {
            continue;
        }
        // 同一个路径、不同的模块大小 = 目标进程装载的是另一个版本：偏移不再有意义，拒绝。
        if (owner_size != 0 && entry.size != 0 && entry.size != owner_size) {
            error = L"the target has a different build of " + owner_path + L" (size " +
                    deskfeed::hex_text(entry.size) + L" vs " + deskfeed::hex_text(owner_size) +
                    L"); refusing to reuse the local address";
            return false;
        }
        out = reinterpret_cast<void*>(entry.base + rva);
        return true;
    }

    error = L"the target process does not have " + owner_path +
            L" loaded; refusing to reuse an address from another module";
    return false;
}

}  // namespace

std::wstring file_name_of(std::wstring const& path) {
    std::wstring::size_type const slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool list_processes_by_image(std::wstring const& image_name, std::vector<ProcessEntry>& found,
                             std::wstring& error) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = L"CreateToolhelp32Snapshot(processes) failed: " + deskfeed::last_error_text();
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            if (::_wcsicmp(entry.szExeFile, image_name.c_str()) != 0) {
                continue;
            }
            ProcessEntry item;
            item.process_id = entry.th32ProcessID;
            HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                           entry.th32ProcessID);
            if (process != nullptr) {
                wchar_t buffer[MAX_PATH * 2]{};
                DWORD chars = static_cast<DWORD>(std::size(buffer));
                if (::QueryFullProcessImageNameW(process, 0, buffer, &chars)) {
                    item.image_path.assign(buffer, chars);
                }
                ::CloseHandle(process);
            }
            found.push_back(std::move(item));
        } while (::Process32NextW(snapshot, &entry));
    }

    ::CloseHandle(snapshot);
    return true;
}

bool list_modules(unsigned long process_id, std::vector<ModuleEntry>& found, std::wstring& error) {
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, process_id);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = L"CreateToolhelp32Snapshot(modules) failed: " + deskfeed::last_error_text();
        return false;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Module32FirstW(snapshot, &entry)) {
        do {
            ModuleEntry item;
            item.path = entry.szExePath;
            item.base = reinterpret_cast<unsigned long long>(entry.modBaseAddr);
            item.size = entry.modBaseSize;
            found.push_back(std::move(item));
        } while (::Module32NextW(snapshot, &entry));
    }

    ::CloseHandle(snapshot);
    return true;
}

ModuleLookup lookup_module_by_full_path(unsigned long process_id, std::wstring const& path,
                                        ModuleEntry& found, std::wstring& error) {
    found = ModuleEntry{};

    std::vector<ModuleEntry> modules;
    if (!list_modules(process_id, modules, error)) {
        // 读不到模块表 = 不可检查。**不能**把它当成"没装载"：那会让调用方在状态未知时做决定。
        return ModuleLookup::Error;
    }

    bool saw_same_name_elsewhere = false;
    for (ModuleEntry const& module : modules) {
        if (module.path.empty()) {
            // 模块表没给出完整路径：绝不退回文件名猜，这一条就当作不可用。
            continue;
        }
        if (module_path_matches(module.path, path)) {
            found = module;
            return ModuleLookup::Found;
        }
        if (lower_case(file_name_of(module.path)) == lower_case(file_name_of(path))) {
            saw_same_name_elsewhere = true;
        }
    }

    if (saw_same_name_elsewhere) {
        error = L"a module with the same file name is loaded from a different path; refusing to "
                L"treat it as " + path;
        return ModuleLookup::NotFound;
    }
    error = L"module is not loaded: " + path;
    return ModuleLookup::NotFound;
}

bool canonical_path(std::wstring const& path, std::wstring& out) {
    out.clear();
    if (path.empty()) {
        return false;
    }
    DWORD const needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return false;
    }
    std::wstring buffer(needed, L'\0');
    DWORD const written = ::GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (written == 0 || written >= needed) {
        return false;
    }
    buffer.resize(written);
    out = strip_trailing_separators(std::move(buffer));
    return !out.empty();
}

bool module_path_matches(std::wstring const& loaded_path, std::wstring const& wanted_path) {
    if (loaded_path.empty() || wanted_path.empty()) {
        return false;
    }
    // 两侧都规整成绝对路径；任何一侧规整失败就不算命中（fail closed，不退化成文件名比较）。
    std::wstring loaded;
    std::wstring wanted;
    if (!canonical_path(loaded_path, loaded) || !canonical_path(wanted_path, wanted)) {
        return false;
    }
    return lower_case(loaded) == lower_case(wanted);
}

bool may_release_remote_buffer(bool thread_created, bool thread_signaled) {
    return !thread_created || thread_signaled;
}

bool load_result_conclusive(bool thread_created, bool thread_signaled) {
    return thread_created && thread_signaled;
}

bool target_is_native_x64(unsigned long process_id, std::wstring& error) {
    error.clear();

    // IsWow64Process2（Win10 1709+）直接给出"目标进程的机器类型"，比只看 WOW64 位更准。
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    static auto const is_wow64_process2 = reinterpret_cast<IsWow64Process2Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        error = L"OpenProcess failed: " + deskfeed::last_error_text();
        return false;
    }

    bool determined = false;
    bool native = false;
    if (is_wow64_process2 != nullptr) {
        USHORT process_machine = 0;
        USHORT native_machine = 0;
        if (is_wow64_process2(process, &process_machine, &native_machine) != FALSE) {
            determined = true;
            native = process_machine == IMAGE_FILE_MACHINE_UNKNOWN;
        }
    }
    if (!determined) {
        BOOL wow64 = FALSE;
        if (::IsWow64Process(process, &wow64) != FALSE) {
            determined = true;
            native = wow64 == FALSE;
        }
    }
    ::CloseHandle(process);

    if (!determined) {
        error = L"cannot determine the target process architecture; refusing to inject";
        return false;
    }
    if (!native) {
        error = L"the taskbar owner is not a native x64 process; an x64 payload cannot be injected "
                L"into it";
        return false;
    }
    return true;
}

bool process_creation_time(unsigned long process_id, std::uint64_t& creation_time, std::wstring& error) {
    creation_time = 0;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        error = L"OpenProcess failed: " + deskfeed::last_error_text();
        return false;
    }
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    bool const ok = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    ::CloseHandle(process);
    if (!ok) {
        error = L"GetProcessTimes failed: " + deskfeed::last_error_text();
        return false;
    }
    creation_time = file_time_value(created);
    return true;
}

bool process_matches_identity(unsigned long process_id, std::uint64_t creation_time) {
    if (process_id == 0 || creation_time == 0) {
        return false;
    }
    HANDLE process = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        return false;
    }
    bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    bool const times_ok = ::GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
    ::CloseHandle(process);
    return alive && times_ok && file_time_value(created) == creation_time;
}

RemoteCall call_remote_thread(unsigned long process_id, void* start, void* parameter, DWORD wait_ms) {
    RemoteCall result;
    if (start == nullptr) {
        result.error = L"no remote entry point";
        return result;
    }
    std::wstring error;
    HANDLE process = open_target(process_id, error);
    if (process == nullptr) {
        result.error = error;
        return result;
    }
    result = run_remote_thread(process, start, parameter, wait_ms);
    ::CloseHandle(process);
    return result;
}

bool inject_library(unsigned long process_id, std::wstring const& dll_path, ModuleEntry& loaded,
                    bool& already_loaded, std::wstring& error) {
    already_loaded = false;

    ModuleEntry existing;
    std::wstring lookup_error;
    switch (lookup_module_by_full_path(process_id, dll_path, existing, lookup_error)) {
        case ModuleLookup::Found:
            already_loaded = true;
            loaded = existing;
            return true;
        case ModuleLookup::Error:
            error = L"cannot read the target module list: " + lookup_error;
            return false;
        case ModuleLookup::NotFound:
            break;
    }

    HANDLE process = open_target(process_id, error);
    if (process == nullptr) {
        return false;
    }

    bool ok = false;
    do {
        SIZE_T const path_bytes = (dll_path.size() + 1) * sizeof(wchar_t);
        void* remote_path = ::VirtualAllocEx(process, nullptr, path_bytes, MEM_COMMIT | MEM_RESERVE,
                                             PAGE_READWRITE);
        if (remote_path == nullptr) {
            error = L"VirtualAllocEx failed: " + deskfeed::last_error_text();
            break;
        }

        SIZE_T written = 0;
        if (!::WriteProcessMemory(process, remote_path, dll_path.c_str(), path_bytes, &written)) {
            error = L"WriteProcessMemory failed: " + deskfeed::last_error_text();
            ::VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            break;
        }

        std::wstring address_error;
        void* load_library = nullptr;
        if (!remote_address_of_export(process_id, "kernel32.dll", "LoadLibraryW", load_library,
                                      address_error)) {
            error = address_error;
            ::VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
            break;
        }

        // 装载可能比 30 秒更久（网络驱动器上的 DLL、安全软件扫描），但**超时不算成功**：
        // 线程没退出时 DllMain 可能还在跑，调用方接着调 DeskFeedStart 就是在没初始化完的模块里
        // 执行代码。这里只报告"结论不确定"，模块保持映射，由调用方稍后复查。
        RemoteCall const call = run_remote_thread(process, load_library, remote_path, 30000);

        // 远程缓冲只在"线程从未创建"或"线程确实退出"后释放。超时释放就是 use-after-free：
        // 目标进程里的 LoadLibraryW 可能还在读那个路径串。短命注入器可以安全地把它泄露到
        // 目标进程退出，绝不用 TerminateThread 去抢。
        if (may_release_remote_buffer(call.thread_created, call.signaled)) {
            ::VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        } else {
            deskfeed::log_format(L"note: the remote path buffer (0x%llx) stays allocated in the "
                                 L"target until it exits: the LoadLibraryW thread has not returned",
                                 static_cast<unsigned long long>(
                                     reinterpret_cast<unsigned long long>(remote_path)));
        }

        if (!call.thread_created) {
            error = call.error;
            break;
        }
        if (!load_result_conclusive(call.thread_created, call.signaled)) {
            error = L"LoadLibraryW has not returned within 30 s; whether the module is loaded is "
                    L"unknown, and starting it now could run inside a DllMain that has not finished. "
                    L"The module (if any) stays mapped; check --status before trying again";
            break;
        }

        // 线程退出了，结论才算确定。退出码是 DWORD，x64 的 HMODULE 会被截断，所以它**不是**
        // 基址来源；模块表（按完整路径）才是。
        ModuleEntry found;
        std::wstring find_error;
        switch (lookup_module_by_full_path(process_id, dll_path, found, find_error)) {
            case ModuleLookup::Found:
                loaded = found;
                if (call.exit_code == 0) {
                    deskfeed::log_format(L"note: LoadLibraryW reported 0 (a truncated x64 handle or "
                                         L"a failure) yet the module is mapped; the module list is "
                                         L"the authority here");
                }
                ok = true;
                break;
            case ModuleLookup::NotFound:
                error = L"LoadLibraryW did not map the module (remote thread exit code " +
                        deskfeed::hex_text(call.exit_code) + L")";
                break;
            case ModuleLookup::Error:
                error = L"the module was loaded but the target module list could not be read: " +
                        find_error;
                break;
        }
        break;
    } while (false);

    ::CloseHandle(process);
    return ok;
}

bool eject_library(unsigned long process_id, unsigned long long module_base, std::wstring& error) {
    HANDLE process = open_target(process_id, error);
    if (process == nullptr) {
        return false;
    }

    bool ok = false;
    do {
        std::wstring address_error;
        void* free_library = nullptr;
        if (!remote_address_of_export(process_id, "kernel32.dll", "FreeLibrary", free_library,
                                      address_error)) {
            error = address_error;
            break;
        }

        RemoteCall const call = run_remote_thread(
            process, free_library, reinterpret_cast<void*>(module_base), 15000);
        if (!call.thread_created) {
            error = call.error;
            break;
        }
        if (!call.signaled) {
            error = call.error;
            break;
        }
        if (call.exit_code == 0) {
            error = L"FreeLibrary returned FALSE in the target process";
            break;
        }

        // FreeLibrary 返回 TRUE 也可能只是把引用数减了一，模块仍在表里。卸载必须用模块表确认。
        std::vector<ModuleEntry> modules;
        std::wstring module_error;
        if (!list_modules(process_id, modules, module_error)) {
            error = L"cannot confirm the unload: " + module_error;
            break;
        }
        bool still_mapped = false;
        for (ModuleEntry const& module : modules) {
            if (module.base == module_base) {
                still_mapped = true;
                break;
            }
        }
        if (still_mapped) {
            error = L"the module is still mapped after FreeLibrary (its reference count is not zero)";
            break;
        }
        ok = true;
    } while (false);

    ::CloseHandle(process);
    return ok;
}

RemoteCall call_remote_export(unsigned long process_id, unsigned long long module_base,
                              unsigned long export_rva, void* parameter, DWORD wait_ms) {
    void* const start = reinterpret_cast<void*>(module_base + export_rva);
    return call_remote_thread(process_id, start, parameter, wait_ms);
}

bool remote_alloc(unsigned long process_id, std::size_t bytes, RemoteBuffer& out, std::wstring& error) {
    out = RemoteBuffer{};
    if (bytes == 0) {
        error = L"remote_alloc with zero bytes";
        return false;
    }
    HANDLE process = ::OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE,
                                   process_id);
    if (process == nullptr) {
        error = L"OpenProcess failed: " + deskfeed::last_error_text();
        return false;
    }
    void* const block = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                         PAGE_READWRITE);
    ::CloseHandle(process);
    if (block == nullptr) {
        error = L"VirtualAllocEx failed: " + deskfeed::last_error_text();
        return false;
    }
    out.address = reinterpret_cast<unsigned long long>(block);
    out.bytes = bytes;
    return true;
}

void remote_free(unsigned long process_id, RemoteBuffer const& buffer) {
    if (buffer.address == 0) {
        return;
    }
    HANDLE process = ::OpenProcess(PROCESS_VM_OPERATION, FALSE, process_id);
    if (process == nullptr) {
        return;
    }
    ::VirtualFreeEx(process, reinterpret_cast<void*>(buffer.address), 0, MEM_RELEASE);
    ::CloseHandle(process);
}

bool remote_write(unsigned long process_id, unsigned long long address, void const* data,
                  std::size_t bytes, std::wstring& error) {
    HANDLE process = ::OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, process_id);
    if (process == nullptr) {
        error = L"OpenProcess failed: " + deskfeed::last_error_text();
        return false;
    }
    SIZE_T written = 0;
    bool const ok = ::WriteProcessMemory(process, reinterpret_cast<void*>(address), data, bytes,
                                         &written) != FALSE &&
                    written == bytes;
    ::CloseHandle(process);
    if (!ok) {
        error = L"WriteProcessMemory failed: " + deskfeed::last_error_text();
    }
    return ok;
}

bool remote_read(unsigned long process_id, unsigned long long address, void* out, std::size_t bytes,
                 std::wstring& error) {
    HANDLE process = ::OpenProcess(PROCESS_VM_READ, FALSE, process_id);
    if (process == nullptr) {
        error = L"OpenProcess failed: " + deskfeed::last_error_text();
        return false;
    }
    SIZE_T read = 0;
    bool const ok = ::ReadProcessMemory(process, reinterpret_cast<void const*>(address), out, bytes,
                                        &read) != FALSE &&
                    read == bytes;
    ::CloseHandle(process);
    if (!ok) {
        error = L"ReadProcessMemory failed: " + deskfeed::last_error_text();
    }
    return ok;
}

bool remote_fetch_pod(unsigned long process_id, unsigned long long module_base, unsigned long export_rva,
                      void* pod, std::size_t pod_bytes, DWORD wait_ms, RemoteCall& call,
                      std::wstring& error) {
    call = RemoteCall{};

    RemoteBuffer buffer;
    if (!remote_alloc(process_id, pod_bytes, buffer, error)) {
        return false;
    }

    bool ok = false;
    do {
        if (!remote_write(process_id, buffer.address, pod, pod_bytes, error)) {
            break;
        }
        call = call_remote_export(process_id, module_base, export_rva,
                                  reinterpret_cast<void*>(buffer.address), wait_ms);
        if (!call.thread_created) {
            error = call.error;
            break;
        }
        // 线程没退出时缓冲区可能正被写：读回来会得到半张 POD，所以这里只报告调用结果，
        // 由调用方决定是否把它当作"未知"（绝不把半张 POD 当结论）。
        if (!call.signaled) {
            error = call.error;
            break;
        }
        if (!remote_read(process_id, buffer.address, pod, pod_bytes, error)) {
            break;
        }
        ok = true;
    } while (false);

    remote_free(process_id, buffer);
    return ok;
}

// ---------------------------------------------------------------------------
// 远程镜像身份核验（T7）。全部只读、有界：见 remote.h 的说明。
// ---------------------------------------------------------------------------
namespace {

constexpr unsigned long long kMaxDosHeaderBytes = 64;
constexpr unsigned long long kMaxLfanew = 64 * 1024;
constexpr unsigned long kMaxSectionCount = 96;
constexpr unsigned long kMaxExportNames = 4096;
constexpr unsigned long kMaxExportFunctions = 4096;
constexpr unsigned long long kMaxNameBytes = 256;
constexpr unsigned long long kIdentityReadBudget = 1024 * 1024;

// [rva, rva+size) 是否整个落在镜像里。64 位相加，DWORD 回绕不能绕过边界。
bool rva_range_inside(unsigned long rva, unsigned long long size, unsigned long long image_size) {
    if (size == 0) {
        return false;
    }
    unsigned long long const begin = rva;
    if (begin >= image_size) {
        return false;
    }
    return size <= image_size - begin;
}

// 一次核验内的读取计数：任何一段读取都先扣预算，超了就整条核验失败。
struct IdentityReader {
    unsigned long process_id = 0;
    unsigned long long base = 0;
    unsigned long long budget_left = kIdentityReadBudget;
    std::wstring& error;

    bool read(unsigned long long rva, void* out, std::size_t bytes, char const* what) {
        if (bytes == 0 || static_cast<unsigned long long>(bytes) > budget_left) {
            error = L"the identity probe read budget is exhausted while reading ";
            error += widen_ascii(what);
            return false;
        }
        budget_left -= bytes;
        if (!remote_read(process_id, base + rva, out, bytes, error)) {
            error = L"cannot read " + widen_ascii(what) + L": " + error;
            return false;
        }
        return true;
    }

    static std::wstring widen_ascii(char const* text) {
        std::wstring out;
        for (; text != nullptr && *text != '\0'; ++text) {
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
        }
        return out;
    }
};

}  // namespace

bool remote_image_identity(unsigned long process_id, unsigned long long module_base,
                           unsigned long long module_size, char const* export_name,
                           RemoteImageIdentity& out, std::wstring& error) {
    out = RemoteImageIdentity{};
    error.clear();
    if (export_name == nullptr || *export_name == '\0') {
        error = L"the identity probe needs an export name";
        return false;
    }
    if (module_size == 0) {
        error = L"the module table reports a zero image size";
        return false;
    }
    IdentityReader reader{process_id, module_base, kIdentityReadBudget, error};

    // 1) DOS 头：e_lfanew 必须落在有界范围内。
    unsigned char dos[kMaxDosHeaderBytes]{};
    if (!reader.read(0, dos, sizeof(dos), "the DOS header")) {
        return false;
    }
    unsigned short const dos_magic = static_cast<unsigned short>(dos[0] | (dos[1] << 8));
    if (dos_magic != 0x5A4D) {
        error = L"the loaded module does not start with MZ";
        return false;
    }
    LONG lfanew = 0;
    std::memcpy(&lfanew, dos + 0x3C, sizeof(lfanew));
    if (lfanew <= 0 || static_cast<unsigned long long>(lfanew) >= kMaxLfanew) {
        error = L"the loaded module has an out-of-range e_lfanew";
        return false;
    }
    unsigned long long const nt = static_cast<unsigned long long>(lfanew);

    // 2) NT 头：签名、Machine、节数、TimeDateStamp、PE32+、SizeOfImage、导出目录项。
    //    一次读 4 + 20 + 240 字节（PE32+ 可选头头部含 16 个数据目录）。多读的尾部不会被解释。
    unsigned char nt_headers[4 + 20 + 240]{};
    if (!reader.read(nt, nt_headers, sizeof(nt_headers), "the NT headers")) {
        return false;
    }
    if (std::memcmp(nt_headers, "PE\0\0", 4) != 0) {
        error = L"the loaded module has no PE signature";
        return false;
    }
    unsigned char const* const file_header = nt_headers + 4;
    unsigned char const* const optional = file_header + 20;
    unsigned short machine = 0;
    unsigned short section_count = 0;
    unsigned long time_date_stamp = 0;
    unsigned short optional_size = 0;
    std::memcpy(&machine, file_header + 0, sizeof(machine));
    std::memcpy(&section_count, file_header + 2, sizeof(section_count));
    std::memcpy(&time_date_stamp, file_header + 4, sizeof(time_date_stamp));
    std::memcpy(&optional_size, file_header + 16, sizeof(optional_size));
    if (machine != IMAGE_FILE_MACHINE_AMD64) {
        error = L"the loaded module is not AMD64";
        return false;
    }
    if (section_count == 0 || section_count > kMaxSectionCount) {
        error = L"the loaded module declares an out-of-range section count";
        return false;
    }
    if (optional_size < 112 + 8) {
        error = L"the loaded module has no room for the export data directory";
        return false;
    }
    unsigned short magic = 0;
    unsigned long size_of_image = 0;
    unsigned long number_of_directories = 0;
    unsigned long export_rva = 0;
    unsigned long export_size = 0;
    std::memcpy(&magic, optional + 0, sizeof(magic));
    std::memcpy(&size_of_image, optional + 56, sizeof(size_of_image));
    std::memcpy(&number_of_directories, optional + 108, sizeof(number_of_directories));
    std::memcpy(&export_rva, optional + 112, sizeof(export_rva));
    std::memcpy(&export_size, optional + 116, sizeof(export_size));
    if (magic != 0x20B) {
        error = L"the loaded module is not PE32+";
        return false;
    }
    if (size_of_image == 0 || static_cast<unsigned long long>(size_of_image) != module_size) {
        error = L"the loaded module reports a different SizeOfImage than the module table";
        return false;
    }
    if (number_of_directories < 1) {
        error = L"the loaded module declares no data directories";
        return false;
    }
    if (export_rva == 0 || export_size < 40) {
        error = L"the loaded module has no usable export directory";
        return false;
    }
    unsigned long long const image_size = size_of_image;
    if (!rva_range_inside(export_rva, export_size, image_size)) {
        error = L"the loaded module's export directory is out of range";
        return false;
    }

    // 3) 导出目录本体。
    unsigned char directory[40]{};
    if (!reader.read(export_rva, directory, sizeof(directory), "the export directory")) {
        return false;
    }
    unsigned long function_count = 0;
    unsigned long name_count = 0;
    unsigned long functions_rva = 0;
    unsigned long names_rva = 0;
    unsigned long ordinals_rva = 0;
    std::memcpy(&function_count, directory + 20, sizeof(function_count));
    std::memcpy(&name_count, directory + 24, sizeof(name_count));
    std::memcpy(&functions_rva, directory + 28, sizeof(functions_rva));
    std::memcpy(&names_rva, directory + 32, sizeof(names_rva));
    std::memcpy(&ordinals_rva, directory + 36, sizeof(ordinals_rva));
    if (name_count == 0 || name_count > kMaxExportNames || function_count == 0 ||
        function_count > kMaxExportFunctions) {
        error = L"the loaded module declares an out-of-range export table";
        return false;
    }
    if (functions_rva == 0 || names_rva == 0 || ordinals_rva == 0 ||
        !rva_range_inside(functions_rva, function_count * 4ull, image_size) ||
        !rva_range_inside(names_rva, name_count * 4ull, image_size) ||
        !rva_range_inside(ordinals_rva, name_count * 2ull, image_size)) {
        error = L"the loaded module's export arrays are out of range";
        return false;
    }

    // 4) 在名字表里找目标导出。先读 4 字节做前缀筛选，只有前缀相符才把整串读回来（有界 NUL）。
    std::size_t const wanted_length = std::strlen(export_name);
    unsigned long found_ordinal = 0;
    bool found = false;
    for (unsigned long index = 0; index < name_count && !found; ++index) {
        unsigned long name_rva = 0;
        if (!reader.read(names_rva + index * 4ull, &name_rva, sizeof(name_rva), "an export name RVA")) {
            return false;
        }
        if (name_rva == 0 || static_cast<unsigned long long>(name_rva) >= image_size) {
            error = L"the loaded module has an out-of-range export name RVA";
            return false;
        }
        unsigned char prefix[4]{};
        if (!reader.read(name_rva, prefix, sizeof(prefix), "an export name prefix")) {
            return false;
        }
        std::size_t const prefix_length = wanted_length < sizeof(prefix) ? wanted_length : sizeof(prefix);
        if (std::memcmp(prefix, export_name, prefix_length) != 0) {
            continue;
        }
        // 名字必须在一个有界的窗口内以 NUL 结尾；找不到结尾就说明这不是一个可信的名字表。
        unsigned long long const available = image_size - name_rva;
        unsigned long long const window = available < kMaxNameBytes ? available : kMaxNameBytes;
        std::vector<unsigned char> window_bytes(static_cast<std::size_t>(window));
        if (!reader.read(name_rva, window_bytes.data(), window_bytes.size(), "an export name")) {
            return false;
        }
        std::size_t name_length = 0;
        while (name_length < window_bytes.size() && window_bytes[name_length] != 0) {
            ++name_length;
        }
        if (name_length >= window_bytes.size()) {
            error = L"the loaded module has an export name without a bounded NUL";
            return false;
        }
        if (name_length != wanted_length ||
            std::memcmp(window_bytes.data(), export_name, wanted_length) != 0) {
            continue;
        }
        unsigned short ordinal = 0;
        if (!reader.read(ordinals_rva + index * 2ull, &ordinal, sizeof(ordinal), "an export ordinal")) {
            return false;
        }
        if (ordinal >= function_count) {
            error = L"the loaded module's export ordinal is out of range";
            return false;
        }
        found_ordinal = ordinal;
        found = true;
    }
    if (!found) {
        error = L"the loaded module does not export ";
        error += IdentityReader::widen_ascii(export_name);
        return false;
    }

    // 5) 函数 RVA：转发导出（落在导出目录自身里）一律拒绝；RVA 0 / 越界也拒绝。
    unsigned long function_rva = 0;
    if (!reader.read(functions_rva + static_cast<unsigned long long>(found_ordinal) * 4, &function_rva,
                     sizeof(function_rva), "an export function RVA")) {
        return false;
    }
    unsigned long long const directory_begin = export_rva;
    unsigned long long const directory_end = static_cast<unsigned long long>(export_rva) + export_size;
    if (static_cast<unsigned long long>(function_rva) >= directory_begin &&
        static_cast<unsigned long long>(function_rva) < directory_end) {
        error = L"the loaded module's export is a forwarder";
        return false;
    }
    if (function_rva == 0 || static_cast<unsigned long long>(function_rva) >= image_size) {
        error = L"the loaded module's export RVA is zero or out of range";
        return false;
    }

    // 6) 节表：证明这个 RVA 落在**可执行**节内。
    unsigned long long const sections_at = nt + 4 + 20 + optional_size;
    std::vector<unsigned char> sections(static_cast<std::size_t>(section_count) * 40);
    if (!reader.read(sections_at, sections.data(), sections.size(), "the section table")) {
        return false;
    }
    bool in_exec_section = false;
    for (unsigned long index = 0; index < section_count && !in_exec_section; ++index) {
        unsigned char const* const entry = sections.data() + index * 40;
        unsigned long virtual_size = 0;
        unsigned long virtual_address = 0;
        unsigned long characteristics = 0;
        std::memcpy(&virtual_size, entry + 8, sizeof(virtual_size));
        std::memcpy(&virtual_address, entry + 12, sizeof(virtual_address));
        std::memcpy(&characteristics, entry + 36, sizeof(characteristics));
        if (virtual_size == 0) {
            continue;
        }
        unsigned long long const begin = virtual_address;
        unsigned long long const end = begin + virtual_size;
        if (function_rva < begin || function_rva >= end) {
            continue;
        }
        if (end > image_size) {
            error = L"the loaded module's executable section extends past SizeOfImage";
            return false;
        }
        if ((characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            error = L"the loaded module's export RVA lives in a non-executable section";
            return false;
        }
        in_exec_section = true;
    }
    if (!in_exec_section) {
        error = L"the loaded module's export RVA is not inside any executable section";
        return false;
    }

    out.machine = machine;
    out.pe32_plus = true;
    out.image_size = image_size;
    out.time_date_stamp = time_date_stamp;
    out.export_rva = function_rva;
    out.export_in_exec_section = true;
    return true;
}

bool image_identity_matches(PeExportInfo const& disk, RemoteImageIdentity const& remote,
                            std::wstring& error) {
    error.clear();
    if (disk.machine != IMAGE_FILE_MACHINE_AMD64 || remote.machine != IMAGE_FILE_MACHINE_AMD64) {
        error = L"the disk image and the loaded module are not both AMD64";
        return false;
    }
    if (!disk.is_pe32_plus || !remote.pe32_plus) {
        error = L"the disk image and the loaded module are not both PE32+";
        return false;
    }
    if (disk.image_size != remote.image_size) {
        error = L"the loaded module has a different SizeOfImage than the on-disk DLL";
        return false;
    }
    if (disk.time_date_stamp != remote.time_date_stamp) {
        error = L"the loaded module has a different TimeDateStamp than the on-disk DLL (a different "
                L"build is mapped)";
        return false;
    }
    if (disk.rva != remote.export_rva) {
        error = L"the same export name maps to a different RVA in the loaded module";
        return false;
    }
    if (disk.rva < disk.section_va || disk.rva >= disk.section_end) {
        error = L"the on-disk export RVA is not inside the executable section it was parsed from";
        return false;
    }
    if (!remote.export_in_exec_section) {
        error = L"the loaded module's export RVA was not proven to be executable";
        return false;
    }
    return true;
}

bool verify_remote_export(unsigned long process_id, unsigned long long module_base,
                          unsigned long long module_size, char const* export_name,
                          PeExportInfo const& disk, unsigned long& verified_rva, std::wstring& error) {
    verified_rva = 0;
    RemoteImageIdentity remote;
    if (!remote_image_identity(process_id, module_base, module_size, export_name, remote, error)) {
        return false;
    }
    if (!image_identity_matches(disk, remote, error)) {
        return false;
    }
    // 调用时用**远程确认过的** RVA：磁盘那个只是候选值。
    verified_rva = remote.export_rva;
    return true;
}

}  // namespace deskfeed::inject
