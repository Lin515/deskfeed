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

namespace deskfeed::payload {
namespace {

constexpr wchar_t kXamlDebugPath[] = L"Software\\Microsoft\\XAML\\Debug";
constexpr wchar_t kValueName[] = L"DisableCompositionDiag";
constexpr wchar_t kStandInPath[] = L"Software\\DeskFeed\\XamlDebug";
constexpr wchar_t kMarkerName[] = L"DeskFeedStandInMarker";

struct Slot {
    void** address = nullptr;
    void* original = nullptr;
};

std::mutex g_mutex;
std::vector<Slot> g_slots;
HKEY g_stand_in = nullptr;

// 新到来调用是否仍走替身。恢复的第一步就是把它置 false：此后即使有迟到的调用进入替身，它也只
// 转发真实调用。注意这不关闭"已选中、未进入"那个单指令窗口（见头文件）。
std::atomic<bool> g_active{false};
// 观测到的、正在替身里执行的调用数。恢复的最后一步等它归零。
std::atomic<unsigned> g_in_flight{0};
std::atomic<bool> g_restore_failed{false};
std::atomic<bool> g_installed{false};

using RegOpenKeyExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
using RegQueryValueExWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
using RegGetValueWFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);

HKEY stand_in_key() {
    if (g_stand_in != nullptr) {
        return g_stand_in;
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
    return g_stand_in;
}

// 句柄被 RegCloseKey 关掉之后地址会被系统复用，所以只认"标记值还在"的那一个。
bool is_stand_in(HKEY key) {
    if (key == nullptr) {
        return false;
    }
    DWORD marker = 0;
    DWORD size = sizeof(marker);
    DWORD type = 0;
    if (::RegQueryValueExW(key, kMarkerName, nullptr, &type, reinterpret_cast<LPBYTE>(&marker),
                           &size) != ERROR_SUCCESS) {
        return false;
    }
    return type == REG_DWORD && marker == 1;
}

LSTATUS WINAPI hook_reg_open(HKEY key, LPCWSTR sub_key, DWORD options, REGSAM access, PHKEY result) {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    LSTATUS status = ::RegOpenKeyExW(key, sub_key, options, access, result);
    if (g_active.load(std::memory_order_acquire) && status != ERROR_SUCCESS && sub_key != nullptr &&
        _wcsicmp(sub_key, kXamlDebugPath) == 0) {
        HKEY const stand_in = stand_in_key();
        if (stand_in != nullptr && result != nullptr) {
            *result = stand_in;
            status = ERROR_SUCCESS;
            log_format(L"reg hook: answered an open of %s with the stand-in key", kXamlDebugPath);
        }
    }
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return status;
}

LSTATUS WINAPI hook_reg_query(HKEY key, LPCWSTR value_name, LPDWORD reserved, LPDWORD type,
                              LPBYTE data, LPDWORD size) {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    LSTATUS status = 0;
    if (g_active.load(std::memory_order_acquire) && value_name != nullptr &&
        _wcsicmp(value_name, kValueName) == 0 && is_stand_in(key)) {
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
    } else {
        status = ::RegQueryValueExW(key, value_name, reserved, type, data, size);
    }
    g_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    return status;
}

LSTATUS WINAPI hook_reg_get(HKEY key, LPCWSTR sub_key, LPCWSTR value_name, DWORD flags,
                            LPDWORD type, PVOID data, LPDWORD size) {
    g_in_flight.fetch_add(1, std::memory_order_acq_rel);
    LSTATUS status = 0;
    bool const allows_dword = (flags & RRF_RT_REG_DWORD) != 0 || (flags & 0xFF) == 0;
    if (g_active.load(std::memory_order_acquire) && allows_dword && value_name != nullptr &&
        _wcsicmp(value_name, kValueName) == 0 &&
        (sub_key == nullptr || _wcsicmp(sub_key, kXamlDebugPath) == 0)) {
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
    } else {
        status = ::RegGetValueW(key, sub_key, value_name, flags, type, data, size);
    }
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

// 只改"当前值正好等于真实导出地址"的槽：别人已经改过的槽不动，避免和别的模块抢同一个槽。
unsigned patch_module(HMODULE module) {
    if (module == nullptr) {
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
            if (original == nullptr || *slot != original) {
                continue;
            }
            DWORD previous = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
                continue;
            }
            *slot = replacement;
            DWORD ignored = 0;
            ::VirtualProtect(slot, sizeof(void*), previous, &ignored);
            g_slots.push_back(Slot{slot, original});
            ++patched;
        }
    }
    return patched;
}

bool module_already_patched(HMODULE module) {
    (void)module;
    return !g_slots.empty();
}

}  // namespace

bool reg_hook_install(std::wstring& error) {
    if (stand_in_key() == nullptr) {
        error = L"建不了替身键 " + std::wstring(kStandInPath) + L": " + last_error_text();
        return false;
    }
    g_active.store(true, std::memory_order_release);
    unsigned const patched = reg_hook_rescan();
    if (patched == 0 && g_slots.empty()) {
        error = L"没有任何模块导入 Reg* 这三个入口";
        return false;
    }
    g_installed.store(true);
    log_format(L"reg hook: %u import slot(s) patched (%s)", g_slots.size(),
               reg_hook_cfg_state().c_str());
    return true;
}

unsigned reg_hook_rescan() {
    std::lock_guard<std::mutex> guard(g_mutex);
    unsigned patched = 0;

    HANDLE const snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ::GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Module32FirstW(snapshot, &entry) != FALSE) {
        do {
            patched += patch_module(entry.hModule);
        } while (::Module32NextW(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    if (patched != 0) {
        log_format(L"reg hook: patched %u more import slot(s)", patched);
    }
    return patched;
}

bool reg_hook_restore(std::wstring& error) {
    std::lock_guard<std::mutex> guard(g_mutex);

    // 第一步：不再有新入口走替身。
    g_active.store(false, std::memory_order_release);

    // 第二步：把全部槽写回原值。
    unsigned failed = 0;
    for (Slot const& slot : g_slots) {
        void* const replacement = nullptr;
        (void)replacement;
        DWORD previous = 0;
        if (::VirtualProtect(slot.address, sizeof(void*), PAGE_READWRITE, &previous) == FALSE) {
            ++failed;
            continue;
        }
        *slot.address = slot.original;
        DWORD ignored = 0;
        ::VirtualProtect(slot.address, sizeof(void*), previous, &ignored);
    }

    // 第三步：等**观测到的**在途调用归零（有界）。
    constexpr unsigned kSettleMs = 1000;
    unsigned waited = 0;
    while (g_in_flight.load(std::memory_order_acquire) != 0 && waited < kSettleMs) {
        ::Sleep(10);
        waited += 10;
    }

    // 复核：还有槽指着本模块就绝不允许卸载。
    unsigned still_mine = 0;
    for (Slot const& slot : g_slots) {
        if (*slot.address != slot.original) {
            ++still_mine;
        }
    }

    if (failed != 0 || still_mine != 0) {
        g_restore_failed.store(true);
        error = L"还原导入表失败（写回失败 " + std::to_wstring(failed) + L" 个，仍有 " +
                std::to_wstring(still_mine) + L" 个槽指着本模块）";
        log_format(L"reg hook: restore incomplete: %u write failure(s), %u slot(s) still patched",
                   failed, still_mine);
        return false;
    }
    if (g_in_flight.load(std::memory_order_acquire) != 0) {
        // 这不阻止恢复成功：不再有新入口，剩下的只是观测窗口内还没退出的替身调用。
        log_format(L"reg hook: %u call(s) were still inside the replacements after %u ms",
                   g_in_flight.load(std::memory_order_acquire), kSettleMs);
    }

    g_installed.store(false);
    log_format(L"reg hook: %zu import slot(s) restored", g_slots.size());
    // 原函数地址与替身键**不在这里清**：还可能有已选中、未进入的调用要转发到它们。
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

}  // namespace deskfeed::payload
