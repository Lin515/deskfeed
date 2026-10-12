#include "payload/reg_hook.h"

#include "platform/log.h"
#include "platform/win32_util.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <tlhelp32.h>
#include <vector>

#ifdef DESKFEED_NATIVE_TESTS
// 只有离线用例目标会定义它：那里把本文件再编译一次，用文件末尾那几个探针在**本进程**里真实地跑
// "补槽 → 自递归反证 → 模块卸载后恢复"。成品 DLL 里没有它们。
#endif

// 通过改写各模块的导入地址表让 XAML 读到 `DisableCompositionDiag=1`。头文件说明了为什么要有这一层
// 规避，以及**明确接受的残余窗口**；这里记三条实现纪律（都有对应的离线用例）：
//
//   1. 转发**绝不**再走本模块自己的 `::Reg*` 导入：替身一律经安装前用 `GetProcAddress` 存下来的真实
//      入口调用。否则本模块自己的槽也被改成替身后，替身会在自己的 IAT 上无限递归。
//   2. 本模块自己的导入表**从不**被补：没有理由让负载自己绕路，也彻底断掉"自己补自己"这条路径。
//   3. 槽地址属于别的模块，那个模块随时可能被卸载，所以写回/复核前先用 `VirtualQuery` 前检、再由
//      不带析构局部对象的 SEH 小函数真正读写；并借用该模块的一次加载器引用，保证"检查过了"与
//      "真正读写"之间它不会被卸掉（借来的引用成对归还）。
namespace deskfeed::payload {
namespace {

constexpr wchar_t kXamlDebugPath[] = L"Software\\Microsoft\\XAML\\Debug";
constexpr wchar_t kValueName[] = L"DisableCompositionDiag";
constexpr wchar_t kStandInPath[] = L"Software\\DeskFeed\\XamlDebug";
constexpr wchar_t kMarkerName[] = L"DeskFeedStandInMarker";

struct Slot {
    void** address = nullptr;
    void* original = nullptr;
    // 槽所属模块。写回/复核时用它借一次加载器引用，防止"前检通过之后模块被卸载"。
    HMODULE module = nullptr;
};

std::mutex g_mutex;
std::vector<Slot> g_slots;
HKEY g_stand_in = nullptr;

using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

// 安装前存下的**真实**函数地址。替身只经它们转发，绝不写 `::Reg*`（见文件头纪律 1）。
//
// 注意 advapi32 里的这三个导出**本身是转发器**：`advapi32!RegQueryValueExW` 的第一条指令就是
// `jmp qword ptr [ADVAPI32!_imp_RegQueryValueExW]`。如果只把 `GetProcAddress(advapi32, ...)` 的结果
// 当成"真实入口"，而 advapi32 自己的导入槽又被补成替身，那这个"真实入口"就会跳回替身——无限递归。
// 所以这里沿转发链一路解析到真正实现的模块（通常是 kernelbase）并记下那些模块名，供补槽时排除。
std::vector<std::string> g_forwarder_modules;

std::string module_base_name(HMODULE module) {
    if (module == nullptr) {
        return std::string();
    }
    char path[MAX_PATH]{};
    DWORD const length = ::GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return std::string();
    }
    std::string text(path, length);
    std::size_t const separator = text.find_last_of("\\/");
    return separator == std::string::npos ? text : text.substr(separator + 1);
}

bool is_forwarder_module(std::string const& base_name) {
    for (std::string const& name : g_forwarder_modules) {
        if (_stricmp(name.c_str(), base_name.c_str()) == 0) {
            return true;
        }
    }
    return false;
}

// 在这个模块的导入表里找一条同名的槽，返回它解析到的来源模块。找不到返回 nullptr。
// 用 `GetModuleHandle` 而不是 `GetProcAddress` 判断"来源模块里有没有这个导出"，是为了不把桩自己
// 又当成目标。
HMODULE find_import_source(HMODULE module, std::string const& function_name) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return nullptr;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        HMODULE const source = ::GetModuleHandleA(dll_name);
        if (source == nullptr || source == module) {
            continue;
        }
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (function_name == reinterpret_cast<char const*>(import->Name) &&
                ::GetProcAddress(source, function_name.c_str()) != nullptr) {
                return source;
            }
        }
    }
    return nullptr;
}

// 沿"这个模块自己那条同名导入槽"走一跳，找出下一跳模块。桩就是经它跳走的。
// 序号导入也要认（实测 advapi32 就是按序号从 apiset 导入这三个入口）。
HMODULE next_module_in_chain(HMODULE module, std::string const& function_name) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return nullptr;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        HMODULE const source = ::GetModuleHandleA(dll_name);
        if (source == nullptr || source == module) {
            continue;
        }
        void* const source_address = ::GetProcAddress(source, function_name.c_str());
        if (source_address == nullptr) {
            continue;
        }
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                WORD const ordinal = static_cast<WORD>(IMAGE_ORDINAL(name_thunk->u1.Ordinal));
                if (::GetProcAddress(source, MAKEINTRESOURCEA(ordinal)) == source_address) {
                    return source;
                }
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (function_name == reinterpret_cast<char const*>(import->Name)) {
                return source;
            }
        }
    }
    return nullptr;
}

// 解析并**记录**"缓存入口真正依赖的模块"。
//
// 裁决口径（MAIN）：**不**去猜"最终内部实现是哪个模块"，也**不**扫全部已加载模块的同名导出——
// 那可能选到无关的同名符号。缓存下来的 `GetProcAddress(受信 advapi32, 名字)` 入口本身可以合法地
// 是一段本地 `jmp` 桩；只要它**依赖的那些模块**（这里沿它自己的 IAT 链走到的 ADVAPI32 / KERNELBASE
// / 以及实际落点模块）都不被补，经缓存入口调用就绝不会回到替身。判定不了就返回 false，安装
// **fail closed**（拒绝安装、保留映射），绝不降级成"没有实际站位"的绿。
bool resolve_and_record_entry(std::string const& function_name, void*& address) {
    HMODULE module = ::GetModuleHandleW(L"advapi32.dll");
    if (module == nullptr) {
        return false;
    }
    address = nullptr;
    for (int hop = 0; hop < 4; ++hop) {
        std::string const base_name = module_base_name(module);
        if (base_name.empty()) {
            return false;
        }
        if (!is_forwarder_module(base_name)) {
            g_forwarder_modules.push_back(base_name);
        }
        if (address == nullptr) {
            address = ::GetProcAddress(module, function_name.c_str());
            if (address == nullptr) {
                return false;
            }
        }
        HMODULE const next = next_module_in_chain(module, function_name);
        if (next == nullptr) {
            return true;  // 这条链到此为止；已经记下的模块都不会被补
        }
        module = next;
    }
    // 链太长：说明还有没记录到的依赖模块，宁可拒绝安装。
    return false;
}

RegOpenKeyExWFn g_open = nullptr;
RegQueryValueExWFn g_query = nullptr;
RegGetValueWFn g_get_value = nullptr;

// 新到来调用是否仍走替身。恢复的第一步就是把它置 false：此后即使有迟到的调用进入替身，它也只
// 转发真实调用。注意这不关闭"已选中、未进入"那个单指令窗口（见头文件与 docs/native-safety.md）。
std::atomic<bool> g_active{false};
// 观测到的、正在替身里执行的调用数。恢复的最后一步等它归零。
std::atomic<unsigned> g_in_flight{0};
std::atomic<bool> g_restore_failed{false};
std::atomic<bool> g_installed{false};
// 替身键已经逻辑关闭（不再交出句柄），避免重试路径重复关闭同一个 HKEY。
std::atomic<bool> g_stand_in_closed{false};

HKEY stand_in_key() {
    if (g_stand_in != nullptr) {
        return g_stand_in;
    }
    if (g_stand_in_closed.load()) {
        // 已经逻辑关闭且删过键：不要在一次恢复之后又把它建回来。
        return nullptr;
    }
    HKEY key = nullptr;
    if (::RegCreateKeyExW(HKEY_CURRENT_USER, kStandInPath, 0, nullptr, 0, KEY_READ | KEY_WRITE,
                          nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return nullptr;
    }
    DWORD const one = 1;
    ::RegSetValueExW(key, kValueName, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&one), sizeof(one));
    ::RegSetValueExW(key, kMarkerName, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&one), sizeof(one));
    g_stand_in = key;
    g_stand_in_closed.store(false);
    return g_stand_in;
}

// 经**保存下来的真实入口**调用查询（不是 `::RegQueryValueExW`）。替身自己的辨认逻辑也走这里：
// 本模块（或把本文件一起编进去的自测可执行文件）的导入表槽可能已经被改成替身，走导入就是自我递归。
LSTATUS call_real_query(HKEY key, LPCWSTR value_name, LPDWORD type, LPBYTE data, LPDWORD size) {
    if (g_query == nullptr) {
        return ERROR_INVALID_FUNCTION;
    }
    LSTATUS status = ERROR_INVALID_FUNCTION;
    __try {
        status = g_query(key, value_name, nullptr, type, data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ERROR_INVALID_FUNCTION;
    }
    return status;
}

// 句柄被 RegCloseKey 关掉之后地址会被系统复用，所以只认"标记值还在"的那一个。
bool is_stand_in(HKEY key) {
    if (key == nullptr) {
        return false;
    }
    DWORD marker = 0;
    DWORD size = sizeof(marker);
    DWORD type = 0;
    if (call_real_query(key, kMarkerName, &type, reinterpret_cast<LPBYTE>(&marker), &size) !=
        ERROR_SUCCESS) {
        return false;
    }
    return type == REG_DWORD && marker == 1;
}

// ---------------------------------------------------------------------------
// 替身里的边界。三个替身是"被别的模块间接调用"的入口：异常绝不能穿进系统调用点，计数也必须在
// **任何**路径上归零——包括 SEH 捕获到的硬件故障，那种情况下 C++ 的 catch 不一定接管，所以减计数
// 放在 SEH 包装返回之后，而不是某个 catch 里。这些叶子不做可能失败的事，也不记日志（日志可能在
// 故障路径上抛出，而那里已经有了一次异常）。
// ---------------------------------------------------------------------------

// 替身键的辨认。这里的 `::RegQueryValueExW` 是本模块自己的导入，而本模块的导入表**从不**被补
// （见文件头纪律 2），所以它一定是真实的 advapi32 入口。
bool stand_in_matches(HKEY key) {
    if (key == nullptr || key == HKEY_CURRENT_USER || key == HKEY_LOCAL_MACHINE) {
        return false;
    }
    __try {
        return is_stand_in(key);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 替身只回答"是的，就是这个值"；其余一切（读、写、其它值名）转给真实入口。
bool answer_open(LPCWSTR sub_key, PHKEY result, LSTATUS& status) {
    if (!g_active.load(std::memory_order_acquire) || sub_key == nullptr || result == nullptr ||
        _wcsicmp(sub_key, kXamlDebugPath) != 0) {
        return false;
    }
    HKEY const stand_in = stand_in_key();
    if (stand_in == nullptr) {
        return false;
    }
    *result = stand_in;
    status = ERROR_SUCCESS;
    return true;
}

bool answer_query(HKEY key, LPCWSTR value_name, LPDWORD type, LPBYTE data, LPDWORD size,
                  LSTATUS& status) {
    if (!g_active.load(std::memory_order_acquire) || value_name == nullptr ||
        _wcsicmp(value_name, kValueName) != 0 || !stand_in_matches(key)) {
        return false;
    }
    DWORD const one = 1;
    if (type != nullptr) {
        *type = REG_DWORD;
    }
    if (data != nullptr && size != nullptr && *size >= sizeof(one)) {
        std::memcpy(data, &one, sizeof(one));
    }
    if (size != nullptr) {
        *size = sizeof(one);
    }
    status = ERROR_SUCCESS;
    return true;
}

bool answer_get(HKEY key, LPCWSTR sub_key, LPCWSTR value_name, DWORD flags, LPDWORD type, PVOID data,
                LPDWORD size, LSTATUS& status) {
    (void)key;
    bool const allows_dword = (flags & RRF_RT_REG_DWORD) != 0 || (flags & 0xFF) == 0;
    if (!g_active.load(std::memory_order_acquire) || !allows_dword || value_name == nullptr ||
        _wcsicmp(value_name, kValueName) != 0 ||
        (sub_key != nullptr && _wcsicmp(sub_key, kXamlDebugPath) != 0)) {
        return false;
    }
    DWORD const one = 1;
    if (type != nullptr) {
        *type = REG_DWORD;
    }
    if (data != nullptr && size != nullptr && *size >= sizeof(one)) {
        std::memcpy(data, &one, sizeof(one));
    }
    if (size != nullptr) {
        *size = sizeof(one);
    }
    status = ERROR_SUCCESS;
    return true;
}

// ---------------------------------------------------------------------------
// 替身入口的**整段**边界。替身是"被别的模块间接调用"的入口，调用方给进来的每个指针都可能是坏的
// （`_wcsicmp`、`*size`、`memcpy` 都会碰它们），所以：答话 + 转发这一整段都在同一个 SEH 叶子里，
// 叶子内部不构造任何需要析构的东西。叶子返回之后才做**唯一一次**减计数，且不再有任何可能失败且
// 未受保护的调用（日志放最后、单独 try 包住）。
// ---------------------------------------------------------------------------

bool hook_reg_open_seh(HKEY key, LPCWSTR sub_key, DWORD options, REGSAM access, PHKEY result,
                       LSTATUS& status, bool& answered) {
    __try {
        answered = answer_open(sub_key, result, status);
        if (!answered) {
            status = g_open(key, sub_key, options, access, result);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = ERROR_INVALID_FUNCTION;
        answered = true;  // 已经碰过调用方的输出，不能再转发一次
        return false;
    }
    return true;
}

bool hook_reg_query_seh(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type, LPBYTE data,
                        LPDWORD size, LSTATUS& status, bool& answered) {
    __try {
        answered = answer_query(key, value_name, type, data, size, status);
        if (!answered) {
            status = g_query(key, value_name, reserved, type, data, size);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = ERROR_INVALID_FUNCTION;
        answered = true;
        return false;
    }
    return true;
}

bool hook_reg_get_seh(HKEY key, LPCWSTR sub_key, LPCWSTR value_name, DWORD flags, LPDWORD type,
                      PVOID data, LPDWORD size, LSTATUS& status, bool& answered) {
    __try {
        answered = answer_get(key, sub_key, value_name, flags, type, data, size, status);
        if (!answered) {
            status = g_get_value(key, sub_key, value_name, flags, type, data, size);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = ERROR_INVALID_FUNCTION;
        answered = true;
        return false;
    }
    return true;
}

LSTATUS WINAPI hook_reg_open(HKEY key, LPCWSTR sub_key, DWORD options, REGSAM access, PHKEY result) {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    LSTATUS status = ERROR_INVALID_FUNCTION;
    bool answered = false;
    hook_reg_open_seh(key, sub_key, options, access, result, status, answered);
    // 叶子返回之后无条件减一次：无论它正常返回还是被 SEH 收走，计数都必须回到原值。
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    if (answered && status == ERROR_SUCCESS) {
        try {
            log_format(L"reg hook: answered an open of %s with the stand-in key", kXamlDebugPath);
        } catch (...) {
            // 日志失败不许影响返回值，也不许穿进调用点。
        }
    }
    return status;
}

LSTATUS WINAPI hook_reg_query(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type,
                              LPBYTE data, LPDWORD size) {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    LSTATUS status = ERROR_INVALID_FUNCTION;
    bool answered = false;
    hook_reg_query_seh(key, value_name, reserved, type, data, size, status, answered);
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return status;
}

LSTATUS WINAPI hook_reg_get(HKEY key, LPCWSTR sub_key, LPCWSTR value_name, DWORD flags, LPDWORD type,
                            PVOID data, LPDWORD size) {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    LSTATUS status = ERROR_INVALID_FUNCTION;
    bool answered = false;
    hook_reg_get_seh(key, sub_key, value_name, flags, type, data, size, status, answered);
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return status;
}

void* replacement_for(char const* name) {
    if (std::strcmp(name, "RegOpenKeyExW") == 0) {
        return reinterpret_cast<void*>(&hook_reg_open);
    }
    if (std::strcmp(name, "RegQueryValueExW") == 0) {
        return reinterpret_cast<void*>(&hook_reg_query);
    }
    if (std::strcmp(name, "RegGetValueW") == 0) {
        return reinterpret_cast<void*>(&hook_reg_get);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 槽的读写。VirtualQuery 只是"前检"：它与后面的读写之间存在 TOCTOU，所以真正的读写仍然在
// 不携带析构局部对象的 SEH 叶子里，并且期间借住槽所属模块的一次加载器引用。
// ---------------------------------------------------------------------------

bool guarded_read_slot(void** address, void*& value) {
    __try {
        value = *address;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool guarded_write_slot(void** address, void* value) {
    __try {
        *address = value;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool slot_committed(void** address) {
    if (address == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    if ((info.Protect & PAGE_GUARD) != 0 || (info.Protect & PAGE_NOACCESS) != 0) {
        return false;
    }
    return true;
}

// 借用 / 归还槽地址所在模块的一次加载器引用。借用失败说明模块已经不在映射里，此时绝不去碰那个
// 地址。借来的引用必须成对归还。
bool borrow_module_reference(HMODULE module) {
    HMODULE borrowed = nullptr;
    return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                reinterpret_cast<LPCWSTR>(module), &borrowed) != FALSE &&
           borrowed == module;
}

void release_module_reference(HMODULE module) {
    if (module != nullptr) {
        ::FreeLibrary(module);
    }
}

struct RestoreCounters {
    unsigned written = 0;
    unsigned skipped = 0;
    unsigned write_failed = 0;
    unsigned still_mine = 0;
};

void restore_slot(Slot const& slot, RestoreCounters& counters) {
    if (!slot_committed(slot.address)) {
        // 模块已经被卸载（或页面已经不可访问）：这里没有我们的东西可还原，跳过并且不认作失败。
        ++counters.skipped;
        return;
    }
    if (!borrow_module_reference(slot.module)) {
        ++counters.skipped;
        return;
    }

    DWORD previous = 0;
    bool const unprotected =
        ::VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &previous) != FALSE;
    bool wrote = false;
    if (unprotected) {
        wrote = guarded_write_slot(slot.address, slot.original);
        DWORD ignored = 0;
        ::VirtualProtect(slot.address, sizeof(void*), previous, &ignored);
    }

    void* current = nullptr;
    bool const readable = guarded_read_slot(slot.address, current);
    release_module_reference(slot.module);

    if (!unprotected || !wrote) {
        ++counters.write_failed;
        return;
    }
    ++counters.written;
    if (!readable || current != slot.original) {
        // 别的模块在我们写回之后又把它改掉了：这个槽仍然不是原值。
        ++counters.still_mine;
    }
}

}  // namespace

// 定义在下面；`reg_hook_install` 已经持有 `g_mutex`，只能走这个"调用方持锁"的版本。
unsigned rescan_locked();

// 只改"当前值正好等于真实导出地址"的槽：别人已经改过的槽不动，避免和别的模块抢同一个槽。
// 本模块自己**从不**被补（见文件头纪律 2）；进程主模块也**从不**被补（替身的辨认与转发都在那里）；
// 转发链上的模块（advapi32 / kernelbase…）同样**绝不**被补——它们的导出就是"真实入口"的落点，
// 补它们等于让真实入口跳回替身。
unsigned patch_module(HMODULE module) {
    if (module == nullptr || module == module_self_handle()) {
        return 0;
    }
    if (is_forwarder_module(module_base_name(module))) {
        return 0;
    }
    if (g_open == nullptr || g_query == nullptr || g_get_value == nullptr) {
        return 0;
    }
    auto* base = reinterpret_cast<unsigned char*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return 0;
    }

    unsigned patched = 0;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        HMODULE const source = ::GetModuleHandleA(dll_name);
        if (source == nullptr) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            void* const replacement = replacement_for(reinterpret_cast<char const*>(import->Name));
            if (replacement == nullptr) {
                continue;
            }
            void* const original = ::GetProcAddress(source, reinterpret_cast<char const*>(import->Name));
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            if (original == nullptr || !slot_committed(slot)) {
                continue;
            }
            void* current = nullptr;
            if (!guarded_read_slot(slot, current) || current != original) {
                continue;
            }
            DWORD previous = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
                continue;
            }
            bool const wrote = guarded_write_slot(slot, replacement);
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previous, &ignored);
            if (!wrote) {
                continue;
            }
            g_slots.push_back(Slot{slot, original, module});
            ++patched;
        }
    }
    return patched;
}

bool reg_hook_install(std::wstring& error) {
    std::lock_guard<std::mutex> guard(g_mutex);

    if (g_open == nullptr || g_query == nullptr || g_get_value == nullptr) {
        // 缓存入口必须在**任何**槽被改写之前取好，并把它依赖的模块全部记进排除表；记不全就
        // **fail closed**：拒绝安装（调用方据此走"不做这层规避"的旧路径，而绝不是"补了但会递归"）。
        g_forwarder_modules.clear();
        void* open_entry = nullptr;
        void* query_entry = nullptr;
        void* get_entry = nullptr;
        if (!resolve_and_record_entry("RegOpenKeyExW", open_entry) ||
            !resolve_and_record_entry("RegQueryValueExW", query_entry) ||
            !resolve_and_record_entry("RegGetValueW", get_entry) || open_entry == nullptr ||
            query_entry == nullptr || get_entry == nullptr) {
            error = L"无法确认 Reg* 缓存入口依赖的系统模块（拒绝安装，避免自我递归）";
            return false;
        }
        g_open = reinterpret_cast<RegOpenKeyExWFn>(open_entry);
        g_query = reinterpret_cast<RegQueryValueExWFn>(query_entry);
        g_get_value = reinterpret_cast<RegGetValueWFn>(get_entry);
    }

    if (stand_in_key() == nullptr) {
        error = L"建不了替身键 " + std::wstring(kStandInPath) + L": " + last_error_text();
        return false;
    }
    g_active.store(true, std::memory_order_release);

    unsigned const patched = rescan_locked();  // 已经持有 g_mutex：绝不在这里调公开的 rescan
    if (patched == 0 && g_slots.empty()) {
        g_active.store(false, std::memory_order_release);
        error = L"没有任何模块导入 Reg* 这三个入口";
        return false;
    }
    g_installed.store(true);
    log_format(L"reg hook: %u import slot(s) patched (%s)", g_slots.size(),
               reg_hook_cfg_state().c_str());
    return true;
}

// 调用方**已经**持有 `g_mutex`。`reg_hook_install` 与公开的 `reg_hook_rescan` 都经这里，
// 但绝不互相套锁（同一个非递归 `std::mutex` 再锁一次就是死锁——安装路径会永远卡住，
// XAML 永远起不来、`DeskFeedStop` 只能报未退休）。
unsigned rescan_locked() {
    if (!g_active.load(std::memory_order_acquire)) {
        return 0;
    }
    unsigned patched = 0;

    HANDLE const snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Module32FirstW(snapshot, &entry) != FALSE) {
        do {
            // 本模块与"缓存入口依赖的系统模块"都在 patch_module 里被挡掉；这里再挡一次，读起来更直白。
            if (entry.hModule == module_self_handle() ||
                is_forwarder_module(module_base_name(entry.hModule))) {
                continue;
            }
            patched += patch_module(entry.hModule);
        } while (::Module32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    if (patched != 0) {
        log_format(L"reg hook: patched %u more import slot(s)", patched);
    }
    return patched;
}

unsigned reg_hook_rescan() {
    std::lock_guard<std::mutex> guard(g_mutex);
    return rescan_locked();
}

bool reg_hook_restore(std::wstring& error) {
    std::lock_guard<std::mutex> guard(g_mutex);

    // 第一步：不再有新入口走替身。
    g_active.store(false, std::memory_order_release);

    // 第二步：把全部槽写回原值（逐槽前检 + SEH 读写 + 借用模块引用）。
    RestoreCounters counters;
    for (Slot const& slot : g_slots) {
        restore_slot(slot, counters);
    }

    // 第三步：等**观测到的**在途调用归零（有界）。
    constexpr unsigned kSettleMs = 1000;
    unsigned waited = 0;
    while (g_in_flight.load(std::memory_order_acquire) != 0 && waited < kSettleMs) {
        ::Sleep(10);
        waited += 10;
    }
    unsigned const in_flight = g_in_flight.load(std::memory_order_acquire);

    if (counters.write_failed != 0 || counters.still_mine != 0) {
        g_restore_failed.store(true);
        error = L"还原导入表失败（写回失败 " + std::to_wstring(counters.write_failed) + L" 个，仍有 " +
                std::to_wstring(counters.still_mine) + L" 个槽不是原值）";
        log_format(L"reg hook: restore incomplete: %u write failure(s), %u slot(s) still patched",
                   counters.write_failed, counters.still_mine);
        return false;
    }

    // 复核通过、在途已经观测归零之后才丢弃替身键：此后任何迟到的调用都会走真实入口。替身键与它的
    // HKCU 键一起删掉，避免每次实验都在用户注册表里留一个持久键。
    HKEY const stand_in = g_stand_in;
    g_stand_in = nullptr;
    g_stand_in_closed.store(true);
    if (stand_in != nullptr) {
        ::RegCloseKey(stand_in);
        LSTATUS const removed = ::RegDeleteKeyW(HKEY_CURRENT_USER, kStandInPath);
        if (removed != ERROR_SUCCESS) {
            log_format(L"reg hook: the HKCU stand-in key could not be deleted (%ld)", removed);
        }
    }

    g_installed.store(false);
    log_format(L"reg hook: %zu import slot(s) restored (%u unreachable slot(s) skipped)", g_slots.size(),
               counters.skipped);
    if (in_flight != 0) {
        // 这不阻止恢复成功：不再有新入口，剩下的只是观测窗口内还没退出的替身调用。
        log_format(L"reg hook: %u call(s) were still inside the replacements after %u ms", in_flight,
                   kSettleMs);
    }
    g_slots.clear();
    return true;
}

bool reg_hook_installed() { return g_installed.load(); }

std::wstring reg_hook_cfg_state() {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY policy{};
    bool const process_cfg =
        ::GetProcessMitigationPolicy(::GetCurrentProcess(), ProcessControlFlowGuardPolicy, &policy,
                                     sizeof(policy)) != FALSE &&
        policy.EnableControlFlowGuard != 0;

    bool image_cfg = false;
    HMODULE self = module_self_handle();
    if (self != nullptr) {
        auto* base = reinterpret_cast<unsigned char*>(self);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE) {
                image_cfg = (nt->OptionalHeader.DllCharacteristics &
                             IMAGE_DLLCHARACTERISTICS_GUARD_CF) != 0;
            }
        }
    }

    std::wstring text = process_cfg ? L"process CFG on" : L"process CFG off";
    text += image_cfg ? L", payload image CFG on" : L", payload image CFG OFF (the reg hook is not safe)";
    return text;
}

bool reg_hook_probe_read(std::wstring& detail) {
    HKEY key = nullptr;
    LSTATUS const opened = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kXamlDebugPath, 0, KEY_READ, &key);
    std::wstring text = L"RegOpenKeyExW(HKLM\\" + std::wstring(kXamlDebugPath) + L") -> " +
                        std::to_wstring(opened);
    if (opened != ERROR_SUCCESS) {
        detail = text + L" (the key does not exist; the stand-in should answer after install)";
        return false;
    }

    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LSTATUS const queried =
        ::RegQueryValueExW(key, kValueName, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(key);

    text += L", RegQueryValueExW -> " + std::to_wstring(queried) + L", type=" +
            std::to_wstring(type) + L", value=" + std::to_wstring(value);
    detail = text;
    return queried == ERROR_SUCCESS && type == REG_DWORD && value == 1;
}

#ifdef DESKFEED_NATIVE_TESTS

bool reg_hook_self_test_rescan() { return reg_hook_rescan() != 0; }

bool reg_hook_self_test_call(const wchar_t* sub_key, bool& answered_by_stand_in) {
    answered_by_stand_in = false;
    if (sub_key == nullptr) {
        return false;
    }
    HKEY key = nullptr;
    LSTATUS const opened = ::RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub_key, 0, KEY_READ, &key);
    if (opened != ERROR_SUCCESS) {
        return false;
    }
    // 替身对我们自己写的那条路径返回的是它自己的键。真实 HKLM 上这条路径不存在，所以"打开成功"
    // 本身就是替身回答的证据；再用 `DisableCompositionDiag` 复核一次值。
    answered_by_stand_in = true;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    LSTATUS const queried = ::RegQueryValueExW(key, kValueName, nullptr, &type,
                                               reinterpret_cast<LPBYTE>(&value), &size);
    ::RegCloseKey(key);
    return queried == ERROR_SUCCESS && type == REG_DWORD && value == 1;
}

// 把本进程（用例可执行文件自己）导入表里指向 `RegOpenKeyExW` 的槽换成我们的替身，模拟
// "本模块自己的槽也被改了"这个最坏情况。返回是否真的改到。用例随后必须调用一次真实的注册表 API。
bool reg_hook_self_test_patch_own_import() {
    HMODULE const advapi32 = ::GetModuleHandleW(L"advapi32.dll");
    if (advapi32 == nullptr) {
        return false;
    }
    void* const real = ::GetProcAddress(advapi32, "RegOpenKeyExW");
    if (real == nullptr) {
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(::GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return false;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        if (::GetModuleHandleA(dll_name) != advapi32) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char const*>(import->Name), "RegOpenKeyExW") != 0) {
                continue;
            }
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD previous = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
                return false;
            }
            *slot = reinterpret_cast<void*>(&hook_reg_open);
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previous, &ignored);
            return true;
        }
    }
    return false;
}

void reg_hook_self_test_restore_own_import() {
    HMODULE const advapi32 = ::GetModuleHandleW(L"advapi32.dll");
    if (advapi32 == nullptr) {
        return;
    }
    void* const real = ::GetProcAddress(advapi32, "RegOpenKeyExW");
    if (real == nullptr) {
        return;
    }
    auto* base = reinterpret_cast<unsigned char*>(::GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        if (::GetModuleHandleA(dll_name) != advapi32) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char const*>(import->Name), "RegOpenKeyExW") != 0) {
                continue;
            }
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD previous = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
                return;
            }
            *slot = real;
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previous, &ignored);
            return;
        }
    }
}

std::wstring reg_hook_self_test_stand_in_path() { return std::wstring(kStandInPath); }

// 直接对**主模块**跑一次补槽，用来验证"caller 的槽能不能被补"。
unsigned reg_hook_self_test_patch_main_module() { return patch_module(::GetModuleHandleW(nullptr)); }

// 把用例可执行文件自己的 `RegQueryValueExW` 槽换成替身（最坏情况的自我递归入口）。
bool reg_hook_self_test_patch_own_query_import() {
    HMODULE const advapi32 = ::GetModuleHandleW(L"advapi32.dll");
    if (advapi32 == nullptr) {
        return false;
    }
    void* const real = ::GetProcAddress(advapi32, "RegQueryValueExW");
    if (real == nullptr) {
        return false;
    }
    auto* base = reinterpret_cast<unsigned char*>(::GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return false;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        if (::GetModuleHandleA(dll_name) != advapi32) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char const*>(import->Name), "RegQueryValueExW") != 0) {
                continue;
            }
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD previous = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
                return false;
            }
            *slot = reinterpret_cast<void*>(&hook_reg_query);
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previous, &ignored);
            return true;
        }
    }
    return false;
}

void reg_hook_self_test_restore_own_query_import() {
    HMODULE const advapi32 = ::GetModuleHandleW(L"advapi32.dll");
    if (advapi32 == nullptr) {
        return;
    }
    void* const real = ::GetProcAddress(advapi32, "RegQueryValueExW");
    if (real == nullptr) {
        return;
    }
    auto* base = reinterpret_cast<unsigned char*>(::GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        char const* const dll_name = reinterpret_cast<char const*>(base + descriptor->Name);
        if (::GetModuleHandleA(dll_name) != advapi32) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char const*>(import->Name), "RegQueryValueExW") != 0) {
                continue;
            }
            auto** slot = reinterpret_cast<void**>(&thunk->u1.Function);
            DWORD previous = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
                return;
            }
            *slot = real;
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previous, &ignored);
            return;
        }
    }
}

// 用例可执行文件自己那条 `RegOpenKeyExW` 导入槽现在指向谁（用来验证"caller 的槽真的被补了"）。
void* reg_hook_self_test_own_open_slot() {
    auto* base = reinterpret_cast<unsigned char*>(::GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return nullptr;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return nullptr;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return nullptr;
    }
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        if (descriptor->FirstThunk == 0 || descriptor->OriginalFirstThunk == 0) {
            continue;
        }
        auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        auto* name_thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
        for (; name_thunk->u1.AddressOfData != 0; ++name_thunk, ++thunk) {
            if (IMAGE_SNAP_BY_ORDINAL(name_thunk->u1.Ordinal)) {
                continue;
            }
            auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + name_thunk->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<char const*>(import->Name), "RegOpenKeyExW") == 0) {
                return reinterpret_cast<void*>(thunk->u1.Function);
            }
        }
    }
    return nullptr;
}

std::wstring reg_hook_self_test_diag() {
    wchar_t buffer[256]{};
    ::swprintf_s(buffer, L"g_open=%p g_query=%p g_get=%p hook_open=%p hook_query=%p hook_get=%p slots=%zu",
                 reinterpret_cast<void*>(g_open), reinterpret_cast<void*>(g_query),
                 reinterpret_cast<void*>(g_get_value), reinterpret_cast<void*>(&hook_reg_open),
                 reinterpret_cast<void*>(&hook_reg_query), reinterpret_cast<void*>(&hook_reg_get),
                 g_slots.size());
    std::wstring text(buffer);
    for (std::string const& name : g_forwarder_modules) {
        text += L" excl=";
        text += std::wstring(name.begin(), name.end());
    }
    return text;
}

#endif  // DESKFEED_NATIVE_TESTS

}  // namespace deskfeed::payload
