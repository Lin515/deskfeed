// 原生侧的离线回归用例。刻意与 tests/tests.cpp 分开：那一份归主进程/帧/配置的作者，这一份只放
// 任务栏负载这条路上的东西——
//   * 显示帧的消费策略（纯逻辑，逐分支）；
//   * 钩子门的收尾协议（真的起一条带消息循环的线程 + 一个 user32 类的确认窗口，**不注入 explorer**）；
//   * C++/SEH 边界（故障必须被吃掉，不能穿出去）；
//   * 成品负载 DLL 的自动审计：PE 导出表能被按名解析、导入表里没有网络/符号库、map 里没有
//     JSON/快照/协议校验器的目标文件。
//
// 用法：
//   deskfeed-native-tests                     跑全部用例
//   deskfeed-native-tests --filter feed       只跑名字里含 feed 的用例
//   deskfeed-native-tests --audit <dll>       只跑成品审计（CTest 单独注册一条）
#include "payload/action_worker.h"
#include "payload/feed.h"
#include "payload/hook_post.h"
#include "payload/reg_hook.h"
#include "payload/retire_gates.h"
#include "payload/status_pod.h"
#include "platform/log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_filter;

void check(bool condition, char const* what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

bool name_matches(char const* name) {
    return g_filter.empty() || std::strstr(name, g_filter.c_str()) != nullptr;
}

// ---------------------------------------------------------------- 纯策略

void test_feed_policy() {
    using deskfeed::payload::FeedInput;
    using deskfeed::payload::FeedOutcome;
    if (!name_matches("feed")) {
        return;
    }

    FeedInput fresh;
    fresh.read_ok = true;
    fresh.owner_alive = true;
    check(deskfeed::payload::feed_decide(fresh) == FeedOutcome::Apply, "feed: a fresh frame applies");

    FeedInput closed;
    closed.read_ok = true;
    closed.closed = true;
    closed.owner_alive = true;
    check(deskfeed::payload::feed_decide(closed) == FeedOutcome::Remove,
          "feed: a closed frame removes the block");

    FeedInput dead;
    dead.read_ok = false;
    dead.owner_alive = false;
    dead.have_last_frame = true;
    check(deskfeed::payload::feed_decide(dead) == FeedOutcome::Remove,
          "feed: a dead producer never keeps the last reading");

    FeedInput transient;
    transient.read_ok = false;
    transient.owner_alive = true;
    transient.have_last_frame = true;
    transient.failures = 0;
    check(deskfeed::payload::feed_decide(transient) == FeedOutcome::Keep,
          "feed: a live producer with a bad frame keeps the last reading");

    transient.failures = deskfeed::payload::kFeedMaxRetainedFailures;
    check(deskfeed::payload::feed_decide(transient) == FeedOutcome::Remove,
          "feed: the retained reading is bounded");

    FeedInput nothing;
    nothing.read_ok = false;
    nothing.owner_alive = true;
    nothing.have_last_frame = false;
    check(deskfeed::payload::feed_decide(nothing) == FeedOutcome::Remove,
          "feed: nothing to show removes the block");
}

// ---------------------------------------------------------------- C++/SEH 边界

bool seh_probe(bool& caught) {
    __try {
        volatile int* pointer = nullptr;
        *pointer = 1;
        caught = false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        caught = true;
    }
    return true;
}

void test_seh_boundary() {
    if (!name_matches("seh")) {
        return;
    }
    bool caught = false;
    seh_probe(caught);
    check(caught, "seh: an access violation inside the boundary is caught");
}

// ---------------------------------------------------------------- 钩子门（离线）

struct PumpThread {
    HANDLE thread = nullptr;
    DWORD thread_id = 0;
    HWND window = nullptr;
    std::atomic<bool> quit{false};
    std::atomic<bool> hang{false};
};

std::atomic<int> g_applies{0};
std::atomic<bool> g_executor_slow{false};

bool test_executor(void*, deskfeed::payload::HookAction action, std::wstring& error) {
    if (g_executor_slow.load()) {
        ::Sleep(400);
    }
    if (action == deskfeed::payload::HookAction::Apply) {
        g_applies.fetch_add(1);
    }
    (void)error;
    return true;
}

LRESULT CALLBACK pump_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return ::DefWindowProcW(window, message, wparam, lparam);
}

DWORD WINAPI pump_entry(LPVOID parameter) {
    auto* state = static_cast<PumpThread*>(parameter);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = pump_window_proc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"DeskFeedNativeTestPump";
    ::RegisterClassExW(&window_class);

    // 顶层但从不显示：它属于这条线程，收尾 ping 的确认窗口就用它（必须是 user32 类的窗口，
    // 不能用本模块自己的窗口过程当确认目标）。
    state->window = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"DeskFeedNativeTestPump", L"pump",
                                      WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                      ::GetModuleHandleW(nullptr), nullptr);
    state->thread_id = ::GetCurrentThreadId();

    MSG message{};
    while (!state->quit.load()) {
        // 挂住的语义是**停止取消息**（钩子就是在线程取消息时被调用的），所以这个判断必须在
        // PeekMessage 之前：先取消息再判断的话，已经排队的 WM_NULL 会立刻把钩子叫起来。
        if (state->hang.load()) {
            ::Sleep(20);
            continue;
        }
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    if (state->window != nullptr) {
        ::DestroyWindow(state->window);
        state->window = nullptr;
    }
    return 0;
}

void test_hook_gate() {
    if (!name_matches("hook")) {
        return;
    }

    PumpThread pump;
    pump.thread = ::CreateThread(nullptr, 0, pump_entry, &pump, 0, nullptr);
    check(pump.thread != nullptr, "hook: the pump thread starts");
    for (int attempt = 0; attempt < 200 && pump.thread_id == 0; ++attempt) {
        ::Sleep(5);
    }
    check(pump.thread_id != 0 && pump.window != nullptr, "hook: the pump thread owns a window");

    deskfeed::payload::hook_post_set_executor(&test_executor, nullptr);
    deskfeed::payload::hook_post_set_ack_window(pump.window);

    // (1) 正常路径：动作在 T 上跑，完成事件 + ping 都拿到，收尾成功且可重复收尾。
    int const before = g_applies.load();
    auto posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply,
                                                   3000);
    check(posted.ok, "hook: the action runs on the taskbar thread");
    check(posted.retired, "hook: the record is removed inside the callback");
    check(g_applies.load() == before + 1, "hook: the executor ran exactly once");
    std::wstring detail;
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: the quiescence proof is obtained");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: a second retire is harmless");
    check(!deskfeed::payload::hook_post_dangling(), "hook: nothing dangles after a clean retire");

    // (2) 对抗：回调在"安装方发布句柄"之前就进入（把发布人为推迟）。握手必须救回来。
    deskfeed::payload::hook_post_set_install_delay_ms(200);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 3000);
    deskfeed::payload::hook_post_set_install_delay_ms(0);
    check(posted.ok && posted.retired, "hook: the install/publication handshake recovers the race");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: the race path still proves quiescence");

    // (3) UI 线程挂住：短超时必须是"未退休"（不卸钩、不还引用、不报告可卸载）；恢复后重试成功。
    pump.hang.store(true);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 150);
    check(!posted.ok, "hook: a hung UI thread is not reported as applied");
    check(deskfeed::payload::hook_post_dangling(), "hook: a hung UI thread is recorded as dangling");
    pump.hang.store(false);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 4000);
    check(posted.ok && posted.retired, "hook: the retry after the UI resumes succeeds");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: the retry also proves quiescence");

    // (4) 慢执行体：动作还没跑完时短超时收尾不能声称拿到证明。
    g_executor_slow.store(true);
    posted = deskfeed::payload::hook_post_run(pump.thread_id, deskfeed::payload::HookAction::Apply, 4000);
    g_executor_slow.store(false);
    check(posted.ok, "hook: a slow action still completes eventually");
    check(deskfeed::payload::hook_post_retire(pump.window, pump.thread_id, 3000, detail),
          "hook: a slow action still retires");

    pump.quit.store(true);
    ::WaitForSingleObject(pump.thread, 5000);
    ::CloseHandle(pump.thread);
}

// ---------------------------------------------------------------- 成品审计

struct ImportNames {
    std::vector<std::string> libraries;
};

bool read_file(std::wstring const& path, std::vector<unsigned char>& bytes) {
    HANDLE const file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (::GetFileSizeEx(file, &size) == FALSE || size.QuadPart <= 0) {
        ::CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    bool const ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) != FALSE;
    ::CloseHandle(file);
    return ok && read == bytes.size();
}

// 直接从磁盘上的 PE 解析导出表；RVA 必须经节表换算成文件偏移（直接把 RVA 当偏移用会得到
// "导出目录不在任何节里"）。
bool find_export(std::vector<unsigned char> const& image, char const* name, unsigned long& rva) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return false;
    }

    auto rva_to_offset = [&](unsigned long value) -> std::size_t {
        auto const* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
            unsigned long const start = section->VirtualAddress;
            unsigned long const size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize
                                                                     : section->SizeOfRawData;
            if (value >= start && value < start + size) {
                return section->PointerToRawData + (value - start);
            }
        }
        return 0;
    };

    std::size_t const export_offset = rva_to_offset(directory.VirtualAddress);
    if (export_offset == 0 || export_offset + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size()) {
        return false;
    }
    auto const* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY const*>(image.data() + export_offset);
    std::size_t const names_offset = rva_to_offset(exports->AddressOfNames);
    std::size_t const functions_offset = rva_to_offset(exports->AddressOfFunctions);
    std::size_t const ordinals_offset = rva_to_offset(exports->AddressOfNameOrdinals);
    if (names_offset == 0 || functions_offset == 0 || ordinals_offset == 0) {
        return false;
    }
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        unsigned long const name_rva =
            reinterpret_cast<DWORD const*>(image.data() + names_offset)[index];
        std::size_t const name_offset = rva_to_offset(name_rva);
        if (name_offset == 0 || name_offset >= image.size()) {
            continue;
        }
        if (std::strcmp(reinterpret_cast<char const*>(image.data() + name_offset), name) == 0) {
            unsigned short const ordinal =
                reinterpret_cast<unsigned short const*>(image.data() + ordinals_offset)[index];
            rva = reinterpret_cast<DWORD const*>(image.data() + functions_offset)[ordinal];
            return true;
        }
    }
    return false;
}

bool collect_imports(std::vector<unsigned char> const& image, ImportNames& out) {
    if (image.size() < sizeof(IMAGE_DOS_HEADER)) {
        return false;
    }
    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(image.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(image.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    auto const& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0) {
        return true;
    }
    auto const* section = IMAGE_FIRST_SECTION(nt);
    auto rva_to_offset = [&](unsigned long value) -> std::size_t {
        for (unsigned index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section) {
            unsigned long const start = section->VirtualAddress;
            unsigned long const size = section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize
                                                                     : section->SizeOfRawData;
            if (value >= start && value < start + size) {
                return section->PointerToRawData + (value - start);
            }
        }
        return 0;
    };

    std::size_t const import_offset = rva_to_offset(directory.VirtualAddress);
    if (import_offset == 0 || import_offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) > image.size()) {
        return false;
    }
    auto const* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(image.data() + import_offset);
    for (; descriptor->Name != 0; ++descriptor) {
        std::size_t const name_offset = rva_to_offset(descriptor->Name);
        if (name_offset == 0 || name_offset >= image.size()) {
            continue;
        }
        std::string name(reinterpret_cast<char const*>(image.data() + name_offset));
        for (char& character : name) {
            character = static_cast<char>(::tolower(static_cast<unsigned char>(character)));
        }
        out.libraries.push_back(name);
    }
    return true;
}

void audit_payload(std::wstring const& dll_path) {
    std::vector<unsigned char> image;
    check(read_file(dll_path, image), "audit: the payload DLL can be read");

    unsigned long start_rva = 0;
    unsigned long stop_rva = 0;
    check(find_export(image, "DeskFeedStart", start_rva), "audit: DeskFeedStart is a parseable export");
    check(find_export(image, "DeskFeedStop", stop_rva), "audit: DeskFeedStop is a parseable export");
    check(start_rva != 0 && stop_rva != 0 && start_rva != stop_rva,
          "audit: the two exports have distinct RVAs");

    ImportNames imports;
    check(collect_imports(image, imports), "audit: the import table can be parsed");
    for (std::string const& library : imports.libraries) {
        bool const forbidden = library.find("winhttp") != std::string::npos ||
                               library.find("dbghelp") != std::string::npos ||
                               library.find("ws2_32") != std::string::npos ||
                               library.find("wininet") != std::string::npos ||
                               library.find("httpapi") != std::string::npos;
        if (forbidden) {
            std::printf("FAIL audit: forbidden import %s\n", library.c_str());
            ++g_failures;
        }
        ++g_checks;
    }

    // map 文件（如果与 DLL 同目录存在）里不该出现 JSON/快照/协议校验器/符号解析的目标文件。
    std::wstring map_path = dll_path;
    std::wstring::size_type const dot = map_path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        map_path = map_path.substr(0, dot) + L".map";
    }
    std::vector<unsigned char> map_bytes;
    if (read_file(map_path, map_bytes)) {
        std::string const map_text(map_bytes.begin(), map_bytes.end());
        char const* const forbidden_objects[] = {
            "json.cpp.obj",   "snapshot.cpp.obj", "health.cpp.obj", "utf8.cpp.obj",
            "symbols.cpp.obj", "pdb.cpp.obj",     "http_client.cpp.obj", "url.cpp.obj",
            "taskbar_symbols.cpp.obj"};
        for (char const* object : forbidden_objects) {
            bool const present = map_text.find(object) != std::string::npos;
            if (present) {
                std::printf("FAIL audit: forbidden object %s is linked into the payload\n", object);
                ++g_failures;
            }
            ++g_checks;
        }
        // 正面的那一半：负载必须真的链到了线格式/邮箱/格式化器与它自己的单元。
        for (char const* expected : {"frame.cpp.obj", "mailbox.cpp.obj", "reading.cpp.obj",
                                     "hook_post.cpp.obj", "taskbar_xaml.cpp.obj", "xaml_tap.cpp.obj"}) {
            bool const present = map_text.find(expected) != std::string::npos;
            if (!present) {
                std::printf("FAIL audit: expected object %s is missing\n", expected);
                ++g_failures;
            }
            ++g_checks;
        }
    }
}

std::wstring widen(char const* text) {
    std::wstring result;
    for (; *text != '\0'; ++text) {
        result.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*text)));
    }
    return result;
}

// ---------------------------------------------------------------- 导入表改写（reg_hook）
//
// 这一组是**真的在本进程里跑**的：装钩子、把用例可执行文件自己导入表里的 `RegOpenKeyExW` 槽换成
// 替身（模拟"本模块自己的槽也被改掉"这个最坏情况）、再调一次真实注册表 API。如果替身还经
// `::Reg*` 转发，这里会栈溢出；改成经安装前存下的真实入口转发之后，它必须正常返回。

constexpr wchar_t kXamlDebugPath[] = L"Software\\Microsoft\\XAML\\Debug";
constexpr wchar_t kXamlValueName[] = L"DisableCompositionDiag";

constexpr wchar_t kCallerDllName[] = L"deskfeed-reg-hook-caller.dll";

// 测试专用 fixture：与用例可执行文件同目录（CMake 把两者都放在 bin/）。相对文件名不参与
// 默认搜索路径，所以这里按"用例自己的目录"拼绝对路径。
std::wstring caller_dll_path() {
    wchar_t buffer[MAX_PATH]{};
    DWORD const length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return std::wstring(kCallerDllName);
    }
    std::wstring path(buffer, length);
    std::size_t const separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return std::wstring(kCallerDllName);
    }
    return path.substr(0, separator + 1) + kCallerDllName;
}

using CallerOpenFn = LONG(WINAPI*)(HKEY, LPCWSTR, REGSAM, PHKEY);
using CallerQueryFn = LONG(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPBYTE, LPDWORD);
using CallerGetFn = LONG(WINAPI*)(HKEY, LPCWSTR, LPCWSTR, DWORD, LPDWORD, PVOID, LPDWORD);
using CallerCloseFn = LONG(WINAPI*)(HKEY);

bool key_exists(HKEY root, wchar_t const* path) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    ::RegCloseKey(key);
    return true;
}

void test_reg_hook() {
    if (!name_matches("reghook")) {
        return;
    }

    bool already_installed = deskfeed::payload::reg_hook_installed();

    std::wstring detail;
    bool const before = deskfeed::payload::reg_hook_probe_read(detail);
    check(!before || already_installed,
          "reghook: before install the real HKLM XAML debug key does not answer 1");

    std::wstring error;
    check(deskfeed::payload::reg_hook_install(error), "reghook: install succeeds");
    if (!error.empty()) {
        std::printf("      install said: %S\n", error.c_str());
    }
    check(deskfeed::payload::reg_hook_installed(), "reghook: installed flag is set");

    // ---- P1 硬路径：真模块被补 → 替身答话 → 正向真实调用不递归 ----
    // `deskfeed-reg-hook-caller.dll` 是**测试专用** fixture：它按名字导入这三个注册表入口，
    // 所以 rescan 会补它的槽。经它的导出调用就走的是"caller → 替身 → 缓存真实入口"这条链。
    HMODULE const caller = ::LoadLibraryW(caller_dll_path().c_str());
    check(caller != nullptr, "reghook: the test-only caller DLL loads");
    if (caller != nullptr) {
        auto const call_open = reinterpret_cast<CallerOpenFn>(::GetProcAddress(caller, "CallerRegOpen"));
        auto const call_query =
            reinterpret_cast<CallerQueryFn>(::GetProcAddress(caller, "CallerRegQuery"));
        auto const call_get = reinterpret_cast<CallerGetFn>(::GetProcAddress(caller, "CallerRegGet"));
        auto const call_close =
            reinterpret_cast<CallerCloseFn>(::GetProcAddress(caller, "CallerRegClose"));
        check(call_open != nullptr && call_query != nullptr && call_get != nullptr &&
                  call_close != nullptr,
              "reghook: the caller DLL exports all four entry points");

        // 新装载的模块要再补一次槽（这也是 rescan 的真实用途）。
        bool const rescanned = deskfeed::payload::reg_hook_self_test_rescan();
        check(rescanned, "reghook: rescan patches the freshly loaded caller module");

        if (call_open != nullptr && call_query != nullptr && call_close != nullptr) {
            // 正向：替身必须把这条 HKLM 路径答成"打开成功 + DisableCompositionDiag=1"，
            // 而真实机器上那个键并不存在。
            HKEY key = nullptr;
            LONG const opened = call_open(HKEY_LOCAL_MACHINE, kXamlDebugPath, KEY_READ, &key);
            check(opened == ERROR_SUCCESS, "reghook: the stand-in answers the caller's open");
            DWORD value = 0;
            DWORD size = sizeof(value);
            DWORD type = 0;
            LONG const queried =
                key != nullptr ? call_query(key, kXamlValueName, &type,
                                            reinterpret_cast<LPBYTE>(&value), &size)
                               : ERROR_INVALID_HANDLE;
            check(queried == ERROR_SUCCESS && type == REG_DWORD && value == 1,
                  "reghook: the stand-in answers the caller's value read");
            if (key != nullptr) {
                call_close(key);
            }

            // 反向（不递归）：同一个模块去读一条**真实存在**的键，替身不答话，必须原样转发给
            // 真实入口并拿到真实结果。替身如果还经 `::Reg*` 转发，这里就会栈溢出。
            HKEY real = nullptr;
            LONG const real_open =
                call_open(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion",
                          KEY_READ, &real);
            check(real_open == ERROR_SUCCESS, "reghook: a real key still opens through the caller");
            if (real != nullptr) {
                call_close(real);
            }
        }

        // ---- P2 硬路径：模块卸载之后再 restore，绝不能 AV ----
        ::FreeLibrary(caller);
        HKEY after_unload = nullptr;
        check(::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kXamlDebugPath, 0, KEY_READ, &after_unload) ==
                  ERROR_FILE_NOT_FOUND,
              "reghook: the caller module is really unloaded");
        if (after_unload != nullptr) {
            ::RegCloseKey(after_unload);
        }
    }

    // 别人已经改过的槽不再被抢：第二次补槽不该覆盖已替换的槽。
    bool const rescan_changed = deskfeed::payload::reg_hook_self_test_rescan();
    check(!rescan_changed, "reghook: a rescan leaves an already-replaced slot alone");

    // P2：恢复之后替身键必须被删掉，而且"已观测在途"必须归零。
    check(deskfeed::payload::reg_hook_restore(error), "reghook: restore succeeds");
    check(!key_exists(HKEY_CURRENT_USER, deskfeed::payload::reg_hook_self_test_stand_in_path().c_str()),
          "reghook: the HKCU stand-in key is deleted on restore");

    deskfeed::payload::HookProbeStatus status{};
    status.size = deskfeed::payload::kProbeStatusSize;
    status.version = deskfeed::payload::kProbeStatusVersion;
    check(deskfeed::payload::hook_post_probe_status(status),
          "reghook: the hook status POD can be filled");
    check(status.in_flight == 0, "reghook: no replacement call is left in flight");
}

// ---------------------------------------------------------------- 回收门：真实 failedWorker + 粘性
//
// 两条互补的证据：
//   1) **真实的失败**：只在本用例目标里编译的测试开关让动作工作线程每次醒来后真的停住，于是
//      `action_worker_stop(150)` 真的等不到线程句柄——不是把结果 mock 掉；
//   2) **粘性/派生**：把这个真实结果按生产路径记进门位，断言"其它门全过、且调用方声称没崩"之后
//      `derived_unload_safe()` **仍然是 0**。旧代码那条"最后一次赋值把 0 覆盖成 1"在这里必红。
void test_retire_gates() {
    if (!name_matches("retire")) {
        return;
    }

    std::wstring error;
    check(deskfeed::payload::action_worker_start(L"nativecheck", error),
          "retire: the action worker starts");
    deskfeed::payload::action_worker_probe_set_test_sleep_ms(1500);
    deskfeed::payload::action_worker_post_refresh();  // 叫醒线程，让它进入那次真实的睡眠
    ::Sleep(60);

    bool const stopped_in_time = deskfeed::payload::action_worker_stop(150);
    check(!stopped_in_time, "retire: a really-still-running worker misses the deadline");

    // 与 host.cpp 的 retire_gates 用的是同一份门位代码。
    deskfeed::payload::RetirementGates gates;
    if (stopped_in_time) {
        gates.worker.store(true);  // 只有在真的等到了线程句柄时才记
    }
    // 其余门先都当作已经拿到——这是对"结论会不会被别的东西盖住"最有利的情形。
    gates.xaml.store(true);
    gates.tap.store(true);
    gates.iat.store(true);
    gates.consumer.store(true);
    gates.thread_done.store(true);
    check(gates.derived_unload_safe() == 0,
          "retire: one failed worker gate keeps unload_safe at 0 even with every other gate passed");

    // 恢复：线程真的退出之后，重试必须能拿到证据并让派生结论变成可卸载。
    deskfeed::payload::action_worker_probe_set_test_sleep_ms(0);
    bool const stopped_on_retry = deskfeed::payload::action_worker_stop(4000);
    check(stopped_on_retry, "retire: the retry really waits the thread out");
    if (stopped_on_retry) {
        gates.worker.store(true);
    }
    check(gates.derived_unload_safe() == 1,
          "retire: the retry completes the gate set and the derived conclusion becomes unloadable");

    // crash 粘性：全部门都过了也不能把故障抹掉；只有新一代才清。
    gates.crashed.store(true);
    check(gates.derived_unload_safe() == 0, "retire: a recorded crash is sticky");
    gates.crashed.store(false);
    check(gates.derived_unload_safe() == 1, "retire: clearing is a new-generation action");
}

// ---------------------------------------------------------------- 状态 ABI 与回收（端到端，离线）
//
// 这一段**真的把成品负载 DLL 加载进本进程**：不起 XAML、不碰任务栏、不注入 explorer。这一轮注入
// 必然在"打不开显示邮箱"处提前返回（本进程里没有生产者），但它把三条硬路径真的跑了一遍：
//   * `DeskFeedStart` **实际接受**的 scope 必须出现在状态 POD 里（从前这里恒为空串）；
//   * `DeskFeedGetStatus` 在没有消费者的进程里必须安全返回（从前它解引用指向栈上对象的裸指针）；
//   * `DeskFeedStop` 的结论必须来自"真实线程句柄已触发 + 模块侧门位全过"，而不是最后一次赋值。

using StatusStartFn = DWORD(WINAPI*)(void*);
using StatusStopFn = DWORD(WINAPI*)(void*);
using StatusGetFn = DWORD(WINAPI*)(void*);

std::wstring sibling_file_path(wchar_t const* file_name) {
    wchar_t buffer[MAX_PATH]{};
    DWORD const length = ::GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (length == 0 || length >= std::size(buffer)) {
        return std::wstring(file_name);
    }
    std::wstring path(buffer, length);
    std::size_t const separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) {
        return std::wstring(file_name);
    }
    return path.substr(0, separator + 1) + file_name;
}

bool query_status(StatusGetFn get, deskfeed::payload::PayloadStatusPod& pod) {
    pod = deskfeed::payload::PayloadStatusPod{};
    pod.size = deskfeed::payload::kPayloadStatusSize;
    pod.abi = deskfeed::payload::kPayloadStatusAbi;
    return get(&pod) == 1;
}

void test_status_abi() {
    if (!name_matches("status")) {
        return;
    }
    HMODULE const module = ::LoadLibraryW(sibling_file_path(L"deskfeed-payload.dll").c_str());
    check(module != nullptr, "status: the payload DLL loads into this process");
    if (module == nullptr) {
        return;
    }
    auto const start = reinterpret_cast<StatusStartFn>(::GetProcAddress(module, "DeskFeedStart"));
    auto const stop = reinterpret_cast<StatusStopFn>(::GetProcAddress(module, "DeskFeedStop"));
    auto const get = reinterpret_cast<StatusGetFn>(::GetProcAddress(module, "DeskFeedGetStatus"));
    check(start != nullptr && stop != nullptr && get != nullptr,
          "status: Start/Stop/GetStatus all resolve");
    if (start != nullptr && stop != nullptr && get != nullptr) {
        deskfeed::payload::PayloadStatusPod pod{};
        check(query_status(get, pod), "status: GetStatus answers before any start");
        check(pod.started == 0, "status: nothing has been started yet");

        // scope 缓冲必须活到负载线程真的退出（负载会先复制一份，这里保守地一直留着）。
        std::wstring const scope = L"nativecheck";
        check(start(const_cast<wchar_t*>(scope.c_str())) == 1, "status: Start accepts the scope");

        bool scope_seen = false;
        for (int attempt = 0; attempt < 600; ++attempt) {
            deskfeed::payload::PayloadStatusPod view{};
            if (query_status(get, view) && view.started == 1) {
                if (std::wstring(view.scope) == scope) {
                    scope_seen = true;
                }
                if (view.thread_alive == 0) {
                    break;  // 负载线程已经结束（本进程没有生产者，它会立刻拆干净）
                }
            }
            ::Sleep(10);
        }
        check(scope_seen, "status: GetStatus reports the scope Start actually accepted");

        check(stop(reinterpret_cast<void*>(static_cast<std::uintptr_t>(5000))) == 1,
              "status: Stop retires it (thread handle + all module-side gates)");

        deskfeed::payload::PayloadStatusPod after{};
        check(query_status(get, after), "status: GetStatus answers after retirement");
        check(after.thread_alive == 0, "status: the payload thread really exited");
        check(after.unload_safe == 1,
              "status: unload_safe is derived from the gate set (not from the last write)");
        check(std::wstring(after.scope) == scope, "status: the scope survives into retirement");
        check(after.frame_generation == 0, "status: frame_generation stays reserved/unknown");
    }
    ::FreeLibrary(module);
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring audit_path;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
            g_filter = argv[++index];
        } else if (std::strcmp(argv[index], "--audit") == 0 && index + 1 < argc) {
            audit_path = widen(argv[++index]);
        }
    }

    if (!audit_path.empty()) {
        audit_payload(audit_path);
    } else {
        test_feed_policy();
        test_seh_boundary();
        test_hook_gate();
        test_reg_hook();
        test_retire_gates();
        test_status_abi();
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
