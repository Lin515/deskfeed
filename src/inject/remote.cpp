#include "inject/remote.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <tlhelp32.h>

#include <cwctype>
#include <iterator>

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

}  // namespace deskfeed::inject
