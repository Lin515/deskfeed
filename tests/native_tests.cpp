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
//   deskfeed-native-tests --audit <dll>       只跑成品审计（CTest 注册两条：生产侧与测试侧）
//   deskfeed-native-tests --filter glue       只跑"测试 DLL 上某条回收门真失败"的粘合回归
//   deskfeed-native-tests --filter failclosed 只跑 GetStatus 的坏指针/不认 ABI 失败路径
//   deskfeed-native-tests --filter guard      只跑进程守卫（纯函数/名字/校验和/恢复表 + 子进程场景）
//   deskfeed-native-tests --guard-child <场景> 内部用法：父进程把自己当**新进程**起出来跑一个守卫场景
//                                              （含 reload 的句柄零增长、hookevents 的无名钩子事件归还，
//                                              以及 probtray 的 R5 托盘目标覆盖与失败关闭）
#include "display/mailbox.h"
#include "payload/action_worker.h"
#include "payload/feed.h"
#include "payload/guard_record.h"
#include "payload/hook_post.h"
#include "payload/reg_hook.h"
#include "payload/retire_gates.h"
#include "payload/status_pod.h"
#include "platform/log.h"

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
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

// 枚举导出**名字**：生产与测试两个 DLL 的差异恰恰是"有没有探针/测试入口"，所以审计必须按名字判定，
// 不能只看几个已知 RVA。
bool collect_export_names(std::vector<unsigned char> const& image, std::vector<std::string>& out) {
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
        return true;  // 没有导出表
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

    std::size_t const export_offset = rva_to_offset(directory.VirtualAddress);
    if (export_offset == 0 || export_offset + sizeof(IMAGE_EXPORT_DIRECTORY) > image.size()) {
        return false;
    }
    auto const* exports =
        reinterpret_cast<IMAGE_EXPORT_DIRECTORY const*>(image.data() + export_offset);
    std::size_t const names_offset = rva_to_offset(exports->AddressOfNames);
    if (names_offset == 0) {
        return false;
    }
    for (unsigned long index = 0; index < exports->NumberOfNames; ++index) {
        std::size_t const entry = names_offset + index * sizeof(unsigned long);
        if (entry + sizeof(unsigned long) > image.size()) {
            return false;
        }
        unsigned long name_rva = 0;
        std::memcpy(&name_rva, image.data() + entry, sizeof(name_rva));
        std::size_t const name_offset = rva_to_offset(name_rva);
        if (name_offset == 0 || name_offset >= image.size()) {
            return false;
        }
        out.emplace_back(reinterpret_cast<char const*>(image.data() + name_offset));
    }
    return true;
}

bool has_export_named(std::vector<std::string> const& names, char const* name) {
    for (std::string const& entry : names) {
        if (entry == name) {
            return true;
        }
    }
    return false;
}

bool has_export_prefix(std::vector<std::string> const& names, char const* prefix) {
    std::size_t const length = std::strlen(prefix);
    for (std::string const& entry : names) {
        if (entry.size() >= length && entry.compare(0, length, prefix) == 0) {
            return true;
        }
    }
    return false;
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

    // 风味：两条 CTest 各自把对应目标文件的真实路径传进来。判不出来时按**生产**（更严的一侧）来，
    // 所以名字写错只会更早报红，不会把生产悄悄当测试放过。
    bool const test_flavor = dll_path.find(L"-test.dll") != std::wstring::npos;
    std::printf("audit: flavor=%s\n", test_flavor ? "test" : "prod");

    std::vector<std::string> export_names;
    check(collect_export_names(image, export_names), "audit: the export name table can be parsed");
    check(has_export_named(export_names, "DeskFeedGetStatus"), "audit: DeskFeedGetStatus is exported");
    if (test_flavor) {
        check(has_export_prefix(export_names, "DeskFeedProbe"),
              "audit: the self-test payload still exports its DeskFeedProbe* entries");
    } else {
        check(!has_export_prefix(export_names, "DeskFeedProbe"),
              "audit: the production payload exports no DeskFeedProbe* entry");
        check(!has_export_prefix(export_names, "DeskFeedTest"),
              "audit: the production payload exports no test-only entry");
    }

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

// GetStatus 的失败路径：坏指针与不认的 size/abi 都必须用**真 DLL**返回失败，并且把 abi 写回 0，
// 让调用方据此报"没有状态 ABI"。绝不用 stdout 里的一个标量当证据。
void test_status_fail_closed() {
    if (!name_matches("failclosed")) {
        return;
    }
    HMODULE const module = ::LoadLibraryW(sibling_file_path(L"deskfeed-payload.dll").c_str());
    check(module != nullptr, "failclosed: the production payload DLL loads into this process");
    if (module == nullptr) {
        return;
    }
    auto const get = reinterpret_cast<StatusGetFn>(::GetProcAddress(module, "DeskFeedGetStatus"));
    check(get != nullptr, "failclosed: GetStatus resolves");
    if (get != nullptr) {
        check(get(nullptr) == 0, "failclosed: a null buffer is refused");
        check(get(reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x10))) == 0,
              "failclosed: an unmapped pointer is refused instead of faulting out");

        deskfeed::payload::PayloadStatusPod wrong_abi{};
        wrong_abi.size = deskfeed::payload::kPayloadStatusSize;
        wrong_abi.width = deskfeed::payload::kPayloadStatusWidth;
        wrong_abi.abi = deskfeed::payload::kPayloadStatusAbi + 1;
        check(get(&wrong_abi) == 0, "failclosed: an unrecognised abi is refused");
        check(wrong_abi.abi == 0, "failclosed: the refused pod comes back with abi 0");

        deskfeed::payload::PayloadStatusPod wrong_size{};
        wrong_size.size = 8;
        wrong_size.abi = deskfeed::payload::kPayloadStatusAbi;
        check(get(&wrong_size) == 0, "failclosed: an unrecognised size is refused");
        check(wrong_size.abi == 0, "failclosed: the size mismatch also comes back with abi 0");
    }
    ::FreeLibrary(module);
}

// 测试 DLL 上的粘合回归：某条回收门**真的**失败时 `DeskFeedStop` 必须返回 0、派生值保持 0；解除
// 之后重试必须真的拿到 1。门位推导与生产路径共用 `retire_gates.h` 与 `host.cpp`，用例不复制假逻辑。
void test_retire_glue() {
    if (!name_matches("glue")) {
        return;
    }

    // 动作工作线程只在邮箱 attach 成功之后才起来，所以本进程先当一次生产者（同一个 scope）。
    std::wstring const scope = L"nativeglue";
    deskfeed::DisplayFrameProducer producer;
    std::string producer_error;
    check(producer.create(scope, nullptr, producer_error),
          "glue: the test process owns a display mailbox producer");
    if (!producer.created()) {
        std::printf("      producer error: %s\n", producer_error.c_str());
        return;
    }

    HMODULE const module = ::LoadLibraryW(sibling_file_path(L"deskfeed-payload-test.dll").c_str());
    check(module != nullptr, "glue: the self-test payload DLL loads into this process");
    if (module == nullptr) {
        producer.close();
        return;
    }

    auto const start = reinterpret_cast<StatusStartFn>(::GetProcAddress(module, "DeskFeedStart"));
    auto const stop = reinterpret_cast<StatusStopFn>(::GetProcAddress(module, "DeskFeedStop"));
    auto const get = reinterpret_cast<StatusGetFn>(::GetProcAddress(module, "DeskFeedGetStatus"));
    using WorkerDelayFn = int(__stdcall*)(unsigned);
    auto const set_worker_delay =
        reinterpret_cast<WorkerDelayFn>(::GetProcAddress(module, "DeskFeedTestSetWorkerDelayMs"));
    auto const stall_worker =
        reinterpret_cast<WorkerDelayFn>(::GetProcAddress(module, "DeskFeedTestStallWorker"));
    check(start != nullptr && stop != nullptr && get != nullptr && set_worker_delay != nullptr &&
              stall_worker != nullptr,
          "glue: Start/Stop/GetStatus and both test-only worker entries resolve");
    if (start != nullptr && stop != nullptr && get != nullptr && set_worker_delay != nullptr &&
        stall_worker != nullptr) {
        check(start(const_cast<wchar_t*>(scope.c_str())) == 1, "glue: Start accepts the scope");

        // 只有工作线程真的起来、并且真的进入那一次睡眠之后，短超时才有确定的失败可咬。
        // `DeskFeedTestStallWorker` 返回 1 正是这个窗口。
        bool stalled = false;
        for (int attempt = 0; attempt < 600 && !stalled; ++attempt) {
            stalled = stall_worker(1500) == 1;
            if (!stalled) {
                ::Sleep(10);
            }
        }
        check(stalled, "glue: the action worker is really up and really stalled");
        deskfeed::payload::PayloadStatusPod attached{};
        check(query_status(get, attached) && attached.mailbox_attached == 1,
              "glue: the payload attached the display mailbox");

        check(stop(reinterpret_cast<void*>(static_cast<std::uintptr_t>(150))) == 0,
              "glue: a short Stop cannot retire while the worker gate is unproven");
        deskfeed::payload::PayloadStatusPod blocked{};
        check(query_status(get, blocked) && blocked.unload_safe == 0,
              "glue: unload_safe stays 0 after the gate really failed");

        check(set_worker_delay(0) == 1, "glue: the worker stall is disarmed");
        check(stop(reinterpret_cast<void*>(static_cast<std::uintptr_t>(4000))) == 1,
              "glue: the retry retires it once the worker really exits");
        deskfeed::payload::PayloadStatusPod after{};
        check(query_status(get, after) && after.unload_safe == 1,
              "glue: unload_safe is 1 on the retry (same derivation as production)");
    }
    ::FreeLibrary(module);
    producer.close();
}

// ---------------------------------------------------------------- 进程守卫（E）
//
// 这一组咬的是"内核对象事实"这条门，而不是任何标量：
//   * **进程内**：`payload/guard_record.h` 的纯函数、对象名格式、校验和向量，以及
//     `guard_classify`/`guard_recovery` 的分支表（TID 在场/离场用**真线程** + 官方 Toolhelp 制造，
//     不 mock 一个布尔出来）；
//   * **子进程**：守卫对象名 = 目标 pid + 创建时间，而 `DllMain` 每个进程只 attach 一次，所以
//     "Native 自己 init / actor 先 init / 事件已置位 / 坏 header / 错类型对象 / 只读零 header /
//     abandoned 恢复 / 引用跨 FreeLibrary / 每轮 reload 不泄漏"这些场景**必须各自在新进程里**跑。
//     父进程用 `CreateProcessW` 把自己起出来（`--guard-child <场景>`）并断言退出码；子进程里
//     的检查行照样打到同一个控制台，所以红的时候能看到是哪一条。
//
// 用例自己就是 cooperating actor：它用唯一的 `guard_record.h` 建对象、持租约、写记录——**不复制**
// 任何布局或常量，也不 mock Native 的结论。actor 侧允许做的（置事件、在能证干净时清事件）在用例里
// 都显式写出来，Native 侧"永不置、永不清、永不读快照"则用真 DLL 的 Stop 返回值与事件状态咬住。

namespace gp = deskfeed::payload;
using GuardRecord = gp::GuardRecord;

std::uint64_t current_process_creation() {
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetProcessTimes(::GetCurrentProcess(), &created, &exited, &kernel, &user) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER stamp{};
    stamp.LowPart = created.dwLowDateTime;
    stamp.HighPart = created.dwHighDateTime;
    return stamp.QuadPart;
}

// 本进程那三个守卫对象名（与 Native 用的是同一个格式化函数）。
bool format_guard_names(wchar_t names[3][gp::kGuardNameCapacity]) {
    std::uint64_t const creation = current_process_creation();
    if (creation == 0) {
        return false;
    }
    auto const pid = static_cast<std::uint32_t>(::GetCurrentProcessId());
    return gp::guard_format_object_name(names[0], gp::kGuardNameCapacity, gp::kGuardSuffixMutex, pid,
                                        creation) &&
           gp::guard_format_object_name(names[1], gp::kGuardNameCapacity, gp::kGuardSuffixEvent, pid,
                                        creation) &&
           gp::guard_format_object_name(names[2], gp::kGuardNameCapacity, gp::kGuardSuffixRecord, pid,
                                        creation);
}

// 三个命名对象里**还有任何一个**活着吗。不能拼出名字时按"还在"（不可证 ⇒ fail closed）。
bool guard_objects_exist() {
    wchar_t names[3][gp::kGuardNameCapacity]{};
    if (!format_guard_names(names)) {
        return true;
    }
    HANDLE const mutex = ::OpenMutexW(SYNCHRONIZE, FALSE, names[0]);
    HANDLE const event = ::OpenEventW(SYNCHRONIZE, FALSE, names[1]);
    HANDLE const mapping = ::OpenFileMappingW(FILE_MAP_READ, FALSE, names[2]);
    bool const any = mutex != nullptr || event != nullptr || mapping != nullptr;
    if (mutex != nullptr) {
        ::CloseHandle(mutex);
    }
    if (event != nullptr) {
        ::CloseHandle(event);
    }
    if (mapping != nullptr) {
        ::CloseHandle(mapping);
    }
    return any;
}

// 本进程那条命名清理事件（`payload_prepare` 建、`payload_release_owned_handles` 在 `DllMain(DETACH)`
// 归还）的名字与存在性。口径限定：这条"对象消失"只在本 own-PID + 本协议合作者的 fixture 里可证——
// 同一个进程里若有别的合作者持着同名事件，对象当然还会活着，那时不能据此断言泄漏（与 actor-refs 的
// 限定同一口径）。
std::wstring cleanup_event_name() {
    return L"Local\\DeskFeed-payload-cleanup-" + std::to_wstring(::GetCurrentProcessId());
}

bool cleanup_event_exists() {
    HANDLE const event = ::OpenEventW(SYNCHRONIZE, FALSE, cleanup_event_name().c_str());
    if (event == nullptr) {
        return false;
    }
    ::CloseHandle(event);
    return true;
}

// 用例侧的 cooperating actor：建/开那三个命名对象、持租约、按写序写记录。
struct GuardActor {
    HANDLE mutex = nullptr;
    HANDLE event = nullptr;
    HANDLE mapping = nullptr;
    GuardRecord* record = nullptr;
    bool owns_lease = false;

    bool setup(bool write_header) {
        wchar_t names[3][gp::kGuardNameCapacity]{};
        if (!format_guard_names(names)) {
            return false;
        }
        ::SetLastError(ERROR_SUCCESS);
        mutex = ::CreateMutexW(nullptr, FALSE, names[0]);
        event = ::CreateEventW(nullptr, TRUE, FALSE, names[1]);
        ::SetLastError(ERROR_SUCCESS);
        mapping =
            ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, names[2]);
        DWORD const mapping_error = ::GetLastError();
        if (mutex == nullptr || event == nullptr || mapping == nullptr) {
            close();
            return false;
        }
        record = static_cast<GuardRecord*>(
            ::MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GuardRecord)));
        if (record == nullptr) {
            close();
            return false;
        }
        // 协议规约：actor 先 create + init 再 LoadLibraryW。
        if (write_header && mapping_error != ERROR_ALREADY_EXISTS) {
            gp::guard_init_new_record(*record, 1);
        }
        return true;
    }

    DWORD acquire_raw(unsigned timeout_ms) {
        DWORD const wait = ::WaitForSingleObject(mutex, timeout_ms);
        owns_lease = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
        return wait;
    }
    bool release_lease() {
        if (!owns_lease) {
            return false;
        }
        owns_lease = false;
        return ::ReleaseMutex(mutex) != FALSE;
    }
    bool header_ok() const { return record != nullptr && gp::guard_header_ok(*record); }
    bool write_idle(std::uint64_t seq) {
        if (record == nullptr) return false;
        gp::guard_make_idle(*record, seq);
        return true;
    }
    bool write_pending_no_tid(std::uint64_t seq) {
        if (record == nullptr) return false;
        gp::guard_make_pending_no_tid(*record, seq);
        return true;
    }
    bool write_pending_tid(std::uint64_t tid, std::uint64_t seq) {
        if (record == nullptr) return false;
        gp::guard_make_pending_tid(*record, tid, seq);
        return true;
    }
    // 对抗性 setup（用例专用）：写一个坏 header，验证 Native 永不覆盖/复置它。
    bool write_bad_header(std::uint32_t size, std::uint32_t version) {
        if (record == nullptr) return false;
        *record = GuardRecord{};
        record->size = size;
        record->version = version;
        return true;
    }
    bool set_event_now() { return ::SetEvent(event) != FALSE; }
    bool clear_event_now() { return ::ResetEvent(event) != FALSE; }
    bool event_signaled() const {
        return event != nullptr && ::WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
    }
    void close() {
        if (owns_lease && mutex != nullptr) {
            ::ReleaseMutex(mutex);
            owns_lease = false;
        }
        if (record != nullptr) {
            ::UnmapViewOfFile(record);
            record = nullptr;
        }
        if (mapping != nullptr) {
            ::CloseHandle(mapping);
            mapping = nullptr;
        }
        if (event != nullptr) {
            ::CloseHandle(event);
            event = nullptr;
        }
        if (mutex != nullptr) {
            ::CloseHandle(mutex);
            mutex = nullptr;
        }
    }
};

// 载入成品负载并取三个导出。子进程场景用**测试 DLL**（它同时导出 Start/Stop/GetStatus）。
struct PayloadApi {
    HMODULE module = nullptr;
    StatusStartFn start = nullptr;
    StatusStopFn stop = nullptr;
    StatusGetFn get = nullptr;

    bool load(wchar_t const* file_name) {
        module = ::LoadLibraryW(sibling_file_path(file_name).c_str());
        if (module == nullptr) {
            return false;
        }
        start = reinterpret_cast<StatusStartFn>(::GetProcAddress(module, "DeskFeedStart"));
        stop = reinterpret_cast<StatusStopFn>(::GetProcAddress(module, "DeskFeedStop"));
        get = reinterpret_cast<StatusGetFn>(::GetProcAddress(module, "DeskFeedGetStatus"));
        return start != nullptr && stop != nullptr && get != nullptr;
    }
    DWORD stop_ms(unsigned ms) const {
        return stop(reinterpret_cast<void*>(static_cast<std::uintptr_t>(ms)));
    }
    bool status(deskfeed::payload::PayloadStatusPod& pod) const { return query_status(get, pod); }
    bool unload_safe() const {
        deskfeed::payload::PayloadStatusPod pod{};
        return status(pod) && pod.unload_safe == 1;
    }
    bool unload() {
        if (module == nullptr) {
            return false;
        }
        bool const ok = ::FreeLibrary(module) != FALSE;
        module = nullptr;
        return ok;
    }
};

int child_result(char const* scenario) {
    std::printf("guard-child %s: %d checks, %d failed\n", scenario, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

// --- 子进程场景 ---------------------------------------------------------------

// 没有任何 actor：`DllMain` 是本进程的创建者，走一次性 init（header + Idle）后降级只读视图。
int child_native_create() {
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "native-create: the self-test payload loads");
    if (api.module == nullptr) {
        return child_result("native-create");
    }
    check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "native-create: Start accepts");
    check(api.stop_ms(5000) == 1,
          "native-create: Stop retires when Native created and initialised the record itself");
    check(api.unload_safe(), "native-create: unload_safe derives 1");
    check(api.unload(), "native-create: FreeLibrary succeeds after a retired Stop");
    return child_result("native-create");
}

// actor 先 create + init 再 Load；记录写成 PendingTid{自己} 且事件**未置位** ⇒ 仍必须走正常门禁并
// 返回 1。这正是"Native 不读 actor 快照"的反例：拿快照当门会把这次 SelfStop 拒掉。
int child_actor_inited_self_pending() {
    GuardActor actor;
    check(actor.setup(true), "actor-inited: the actor creates and initialises the record");
    check(actor.header_ok(), "actor-inited: the immutable header is valid");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "actor-inited: the self-test payload loads");
    if (api.module == nullptr) {
        actor.close();
        return child_result("actor-inited");
    }
    check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "actor-inited: Start accepts");
    check(actor.acquire_raw(2000) == WAIT_OBJECT_0, "actor-inited: the actor holds the lease");
    check(actor.write_pending_tid(::GetCurrentThreadId(), 2),
          "actor-inited: the actor records PendingTid{this thread}");
    check(gp::guard_classify(*actor.record) == gp::GuardSnapshot::PendingTid,
          "actor-inited: the snapshot is a valid PendingTid (the actor's own pending Stop)");
    check(actor.release_lease(), "actor-inited: the actor concludes and releases the lease");
    check(api.stop_ms(5000) == 1,
          "actor-inited: a valid header + PendingTid{self} + event clear still retires (SelfStop)");
    check(actor.write_idle(3), "actor-inited: the actor settles the record back to Idle");
    check(api.unload(), "actor-inited: FreeLibrary succeeds after a retired Stop");
    actor.close();
    return child_result("actor-inited");
}

// 粘性不确定事件已置位：Stop 必须拒绝，而且**重试仍然拒绝**、事件仍被 Native 留着（永不替它清）。
int child_event_signaled() {
    GuardActor actor;
    check(actor.setup(true), "event-signaled: the actor creates and initialises the record");
    check(actor.set_event_now(), "event-signaled: the actor sets the sticky uncertainty event");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "event-signaled: the self-test payload loads");
    if (api.module == nullptr) {
        actor.close();
        return child_result("event-signaled");
    }
    check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "event-signaled: Start accepts");
    check(api.stop_ms(5000) == 0, "event-signaled: a signalled uncertainty event refuses retirement");
    check(api.stop_ms(5000) == 0, "event-signaled: the refusal is sticky across retries");
    check(actor.event_signaled(), "event-signaled: Native never clears the sticky event");
    check(!api.unload_safe(), "event-signaled: unload_safe stays 0");
    // Stop 说未退休 ⇒ 映射必须保留：这里**不** FreeLibrary（子进程退出时自然收场）。
    check(api.module != nullptr, "event-signaled: the module stays mapped after a refused Stop");
    actor.close();
    return child_result("event-signaled");
}

// 不可变头坏（version 不认）：Native 必须 fail closed，而且**永不覆盖/复置**这个坏 header。
int child_bad_header() {
    GuardActor actor;
    check(actor.setup(false), "bad-header: the actor creates the record without a valid header");
    check(actor.write_bad_header(gp::kGuardRecordSize, gp::kGuardRecordVersion + 1),
          "bad-header: the actor writes an unrecognised version");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "bad-header: the self-test payload loads");
    if (api.module == nullptr) {
        actor.close();
        return child_result("bad-header");
    }
    check(api.stop_ms(5000) == 0, "bad-header: an unrecognised header refuses retirement");
    check(api.stop_ms(5000) == 0, "bad-header: the refusal is sticky");
    check(actor.record->size == gp::kGuardRecordSize &&
              actor.record->version == gp::kGuardRecordVersion + 1,
          "bad-header: Native never overwrites or resets a bad header");
    actor.close();
    return child_result("bad-header");
}

// record 名下是一个**错类型**的内核对象（event 而不是 file mapping）：Native 的 CreateFileMappingW
// 必然失败 ⇒ 粘性未知 ⇒ Stop 恒 0。
int child_wrong_object_type() {
    wchar_t names[3][gp::kGuardNameCapacity]{};
    check(format_guard_names(names), "wrong-type: the guard object names can be formatted");
    HANDLE const wrong = ::CreateEventW(nullptr, TRUE, FALSE, names[2]);
    check(wrong != nullptr, "wrong-type: an event exists under the record name");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "wrong-type: the self-test payload loads");
    if (api.module != nullptr) {
        check(api.stop_ms(5000) == 0, "wrong-type: a wrong kernel object type refuses retirement");
        check(api.stop_ms(5000) == 0, "wrong-type: the refusal is sticky");
    }
    if (wrong != nullptr) {
        ::CloseHandle(wrong);
    }
    return child_result("wrong-type");
}

// 只读 section + 全零 header：Native 既写不进去、header 也不认 ⇒ fail closed，且零 header 原样保留。
int child_readonly_zero_header() {
    wchar_t names[3][gp::kGuardNameCapacity]{};
    check(format_guard_names(names), "readonly-zero: the guard object names can be formatted");
    HANDLE const mutex = ::CreateMutexW(nullptr, FALSE, names[0]);
    HANDLE const event = ::CreateEventW(nullptr, TRUE, FALSE, names[1]);
    HANDLE const section =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READONLY, 0, 4096, names[2]);
    check(mutex != nullptr && event != nullptr && section != nullptr,
          "readonly-zero: the actor creates a read-only section under the record name");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "readonly-zero: the self-test payload loads");
    if (api.module != nullptr) {
        check(api.stop_ms(5000) == 0, "readonly-zero: a zero header refuses retirement");
        check(api.stop_ms(5000) == 0, "readonly-zero: the refusal is sticky");
    }
    if (section != nullptr) {
        auto const* const view = static_cast<GuardRecord const*>(
            ::MapViewOfFile(section, FILE_MAP_READ, 0, 0, sizeof(GuardRecord)));
        check(view != nullptr && view->size == 0 && view->version == 0,
              "readonly-zero: Native never writes the zero header");
        if (view != nullptr) {
            ::UnmapViewOfFile(view);
        }
    }
    if (section != nullptr) ::CloseHandle(section);
    if (event != nullptr) ::CloseHandle(event);
    if (mutex != nullptr) ::CloseHandle(mutex);
    return child_result("readonly-zero");
}

// 上一位 actor 在写下 PendingNoTid 之后崩掉（线程带锁退出 ⇒ WAIT_ABANDONED）。这个形状**不可证**：
// 演员必须置粘性事件并收场，Native 随后必须拒绝卸载。
int child_abandoned_pending_no_tid() {
    GuardActor actor;
    check(actor.setup(true), "abandoned-pending: the actor creates and initialises the record");
    HANDLE const helper = ::CreateThread(
        nullptr, 0,
        [](void* parameter) -> DWORD {
            auto* const self = static_cast<GuardActor*>(parameter);
            if (self->acquire_raw(2000) != WAIT_OBJECT_0) {
                return 1;
            }
            self->write_pending_no_tid(2);
            return 0;  // ExitThread：**不** ReleaseMutex ⇒ abandoned
        },
        &actor, 0, nullptr);
    check(helper != nullptr, "abandoned-pending: the crashing helper thread starts");
    if (helper != nullptr) {
        ::WaitForSingleObject(helper, 5000);
        ::CloseHandle(helper);
    }
    check(actor.acquire_raw(2000) == WAIT_ABANDONED,
          "abandoned-pending: the next actor sees WAIT_ABANDONED");
    check(actor.record != nullptr && gp::guard_classify(*actor.record) == gp::GuardSnapshot::PendingNoTid,
          "abandoned-pending: the record really says PendingNoTid");
    check(gp::guard_recovery(gp::GuardSnapshot::PendingNoTid, gp::GuardTidProbe::Absent) ==
              gp::GuardRecovery::KeepUncertain,
          "abandoned-pending: PendingNoTid is never provable");
    check(actor.set_event_now(), "abandoned-pending: the actor sets the sticky event");
    // 按 actor 纪律：结论是 Unknown ⇒ **不释放**租约，事实留在内核对象里（本线程随即结束）。
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "abandoned-pending: the self-test payload loads");
    if (api.module != nullptr) {
        check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "abandoned-pending: Start accepts");
        check(api.stop_ms(5000) == 0, "abandoned-pending: Native refuses retirement");
        check(actor.event_signaled(), "abandoned-pending: the sticky event is still set");
    }
    return child_result("abandoned-pending");
}

// 上一位 actor 崩掉时记录是 Idle ⇒ **能证干净**：只有演员可以清事件并重写 Idle，之后 Native 的
// Stop 必须走正常门禁并返回 1（Native 自己永不清、永不置）。
int child_abandoned_idle_recovered() {
    GuardActor actor;
    check(actor.setup(true), "abandoned-idle: the actor creates and initialises the record");
    HANDLE const helper = ::CreateThread(
        nullptr, 0,
        [](void* parameter) -> DWORD {
            auto* const self = static_cast<GuardActor*>(parameter);
            if (self->acquire_raw(2000) != WAIT_OBJECT_0) {
                return 1;
            }
            self->write_idle(2);
            return 0;  // ExitThread：**不** ReleaseMutex ⇒ abandoned
        },
        &actor, 0, nullptr);
    check(helper != nullptr, "abandoned-idle: the crashing helper thread starts");
    if (helper != nullptr) {
        ::WaitForSingleObject(helper, 5000);
        ::CloseHandle(helper);
    }
    check(actor.acquire_raw(2000) == WAIT_ABANDONED,
          "abandoned-idle: the next actor sees WAIT_ABANDONED");
    check(actor.record != nullptr && gp::guard_classify(*actor.record) == gp::GuardSnapshot::Idle,
          "abandoned-idle: the record proves Idle");
    check(gp::guard_recovery(gp::GuardSnapshot::Idle, gp::GuardTidProbe::NotQueried) ==
              gp::GuardRecovery::ClearAndContinue,
          "abandoned-idle: Idle is provable");
    check(actor.clear_event_now(), "abandoned-idle: only the actor clears the sticky event");
    check(actor.write_idle(3), "abandoned-idle: the actor rewrites Idle(seq+1)");
    check(actor.release_lease(), "abandoned-idle: the actor concludes and releases the lease");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "abandoned-idle: the self-test payload loads");
    if (api.module != nullptr) {
        check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "abandoned-idle: Start accepts");
        check(api.stop_ms(5000) == 1,
              "abandoned-idle: after the actor proved it clean, Native retires normally");
        check(api.unload(), "abandoned-idle: FreeLibrary succeeds after a retired Stop");
    }
    actor.close();
    return child_result("abandoned-idle");
}

// 引用语义：actor 先建对象再 Load。healthy FreeLibrary 之后本模块归还自己那 3 个引用，但 actor
// 还持着 ⇒ 记录**不消失**；actor 最后关掉 ⇒ 三个对象消失。若本模块把引用泄漏成"永远活着"，
// 最后那条断言必红。
//
// 口径限定：这条"对象消失"只在本 fixture 里可证——对象名按**本进程 pid+creation** 命名，而这个
// 子进程里除了本用例这个 actor 没有任何其它 cooperating 持有者（也没有第三方拿租约）。真实机器上
// 若同一进程里还有别的合作者持引用，对象当然会继续活着，那时不能据此断言"引用泄漏"。
int child_actor_held_references() {
    GuardActor actor;
    check(actor.setup(true), "actor-refs: the actor creates and initialises the record");
    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), "actor-refs: the self-test payload loads");
    if (api.module == nullptr) {
        actor.close();
        return child_result("actor-refs");
    }
    check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "actor-refs: Start accepts");
    check(api.stop_ms(5000) == 1, "actor-refs: Stop retires");
    check(api.unload(), "actor-refs: FreeLibrary succeeds after a retired Stop");
    check(actor.header_ok(), "actor-refs: the record survives FreeLibrary while the actor holds it");
    check(guard_objects_exist(), "actor-refs: the three named objects are still alive");
    actor.close();
    check(!guard_objects_exist(), "actor-refs: after the last reference closes the objects disappear");
    return child_result("actor-refs");
}

// 每轮 Load/Start/Stop/FreeLibrary 之后三条都必须是机械事实：
//   1. 三个守卫对象消失（E 的机械证据：泄漏一个 `Open*` 就还能成功）；
//   2. 命名清理事件的对象消失（`OpenEventW` 失败）：它同时证明 DETACH 真的跑到了，并且本 own-PID
//      fixture 里没有别的持有者（真实机器上不能据此断言，见 `cleanup_event_name` 的口径）；
//   3. **进程句柄数逐轮零增长**：`DllMain(DETACH)` 归还本模块自己拿的那 4 个句柄（命名清理事件 +
//      守卫 mutex/event/mapping），所以"装载之后比上一轮的稳态恰好多 4 个、卸载之后一个不剩"这
//      两条等式必须在第一轮之后的每一轮都成立。
// 首轮会额外产生一次性的**进程级**状态（user32/COM/WinRT 的进程级缓存：这里实测 +35，之后恒定），
// 所以用它之后的稳态值当参考。这条断言对"少一次 CloseHandle"是确定性的：删掉任一归还，句柄数就
// 每轮 +1/+2，两条等式立刻不成立。
int child_reload_objects_gone() {
    constexpr int kCycles = 6;
    DWORD baseline = 0;
    ::GetProcessHandleCount(::GetCurrentProcess(), &baseline);
    std::printf("reload: handles before any load=%lu\n", static_cast<unsigned long>(baseline));
    DWORD plateau = 0;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        PayloadApi api;
        check(api.load(L"deskfeed-payload-test.dll"), "reload: the self-test payload loads");
        if (api.module == nullptr) {
            break;
        }
        DWORD loaded = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &loaded);
        check(cleanup_event_exists(),
              "reload: the named cleanup event exists while the payload is mapped");
        check(api.start(const_cast<wchar_t*>(L"nativeguard")) == 1, "reload: Start accepts");
        check(api.stop_ms(5000) == 1, "reload: Stop retires");
        check(api.unload(), "reload: FreeLibrary succeeds after a retired Stop");
        check(!guard_objects_exist(), "reload: every keeper reference is released on FreeLibrary");
        check(!cleanup_event_exists(), "reload: the named cleanup event is gone after FreeLibrary");
        DWORD after = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &after);
        std::printf("reload: cycle %d loaded=%lu after=%lu\n", cycle,
                    static_cast<unsigned long>(loaded), static_cast<unsigned long>(after));
        if (cycle == 0) {
            plateau = after;
            continue;
        }
        check(loaded == plateau + 4,
              "reload: one load adds exactly the module's four own handles (cleanup + guard x3)");
        check(after == plateau, "reload: FreeLibrary returns every one of the module's own handles");
    }
    return child_result("reload");
}

// 无名钩子事件（`hook_post` 的 `done` / `published`）在 DETACH 的归还条件：门已完全退休。这两条事件
// 外面看不见，所以这里用测试 DLL 的探针入口在**本进程自己的一条隐藏 UI 线程**上真造一次安装：
//   * 线程 T 只建一个不可见的 STATIC 窗口并泵消息。绝不查找/触碰任何真实托盘窗口，不注入 explorer，
//     不起可见 GUI，不动光标；
//   * `DeskFeedProbePostApply` 走真实 `hook_post_run`：真装 WH_GETMESSAGE 线程钩子、真发布、
//     T 真进回调、真自摘、真置完成事件（执行体未注册 ⇒ 动作本身报失败，这里不咬动作）；
//   * `DeskFeedProbeHookStatus` 咬住"这一次真的发生过"（retired=1、hook_installed=0、in_flight=0、
//     completion_expected=1、dangling=0），避免"什么都没装、收尾当然返回 1"这种假绿；
//   * `DeskFeedProbeRetire` 走同一条收尾门：等完成事件、向 T 的窗口发 `WM_NULL` 并拿到应答之后才
//     归还非 PIN 自引用 ⇒ 此后 `DllMain(DETACH)` 才有权关那两条事件。
// 每轮卸载后句柄数必须回到基线：少关一条事件就会每轮 +1/+2，这里必红。
struct SyntheticUiThread {
    std::atomic<bool> stop{false};
    std::atomic<HWND> window{nullptr};
    // 第二个 own 窗口：给"覆盖窗口被销毁、确认窗口还活着"这种组合用（两个窗口都在这一条 UI 线程上，
    // 所以钩子门那次身份复验看的是同一条线程）。
    std::atomic<HWND> second{nullptr};
    std::atomic<bool> destroy_second_requested{false};
    std::atomic<DWORD> thread_id{0};
    HANDLE thread = nullptr;

    static HWND make_window() {
        return ::CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
    }

    static DWORD WINAPI entry(void* parameter) {
        auto* const self = static_cast<SyntheticUiThread*>(parameter);
        self->thread_id.store(::GetCurrentThreadId());
        HWND const window = make_window();
        self->window.store(window);
        self->second.store(make_window());
        if (window == nullptr) {
            return 1;
        }
        MSG message{};
        while (!self->stop.load(std::memory_order_acquire)) {
            // 窗口只能在它的属主线程上销毁，所以"销毁第二个窗口"是一次跨线程请求。
            if (self->destroy_second_requested.exchange(false, std::memory_order_acq_rel)) {
                HWND const target = self->second.load();
                if (target != nullptr) {
                    ::DestroyWindow(target);
                    self->second.store(nullptr);
                }
            }
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            ::MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        HWND const leftover = self->second.load();
        if (leftover != nullptr) {
            ::DestroyWindow(leftover);
            self->second.store(nullptr);
        }
        ::DestroyWindow(window);
        self->window.store(nullptr);
        return 0;
    }

    bool start() {
        thread = ::CreateThread(nullptr, 0, &entry, this, 0, nullptr);
        for (int attempt = 0; attempt < 500 && window.load() == nullptr; ++attempt) {
            ::Sleep(10);
        }
        return thread != nullptr && window.load() != nullptr && second.load() != nullptr;
    }

    // 让 UI 线程销毁第二个窗口，并有界地等它真的消失。
    bool destroy_second_window() {
        if (second.load() == nullptr) {
            return false;
        }
        destroy_second_requested.store(true, std::memory_order_release);
        ::PostThreadMessageW(thread_id.load(), WM_NULL, 0, 0);
        for (int attempt = 0; attempt < 300 && second.load() != nullptr; ++attempt) {
            ::Sleep(10);
        }
        return second.load() == nullptr;
    }

    void shutdown() {
        stop.store(true, std::memory_order_release);
        if (thread != nullptr) {
            ::WaitForSingleObject(thread, 5000);
            ::CloseHandle(thread);
            thread = nullptr;
        }
    }
};

int child_hook_events_released() {
    SyntheticUiThread ui;
    check(ui.start(), "hookevents: the hidden synthetic UI thread is up with its own window");
    if (ui.window.load() == nullptr) {
        ui.shutdown();
        return child_result("hookevents");
    }

    DWORD baseline = 0;
    ::GetProcessHandleCount(::GetCurrentProcess(), &baseline);
    std::printf("hookevents: handles before any load=%lu\n", static_cast<unsigned long>(baseline));

    DWORD plateau = 0;
    DWORD loaded_reference = 0;
    constexpr int kCycles = 4;
    for (int cycle = 0; cycle < kCycles; ++cycle) {
        PayloadApi api;
        check(api.load(L"deskfeed-payload-test.dll"), "hookevents: the self-test payload loads");
        if (api.module == nullptr) {
            break;
        }
        auto const set_tray = reinterpret_cast<int(__stdcall*)(HWND)>(
            ::GetProcAddress(api.module, "DeskFeedProbeSetTrayWindow"));
        auto const post = reinterpret_cast<int(__stdcall*)(unsigned, unsigned)>(
            ::GetProcAddress(api.module, "DeskFeedProbePostApply"));
        auto const retire = reinterpret_cast<int(__stdcall*)(unsigned)>(
            ::GetProcAddress(api.module, "DeskFeedProbeRetire"));
        auto const hook_status = reinterpret_cast<int(__stdcall*)(gp::HookProbeStatus*, unsigned)>(
            ::GetProcAddress(api.module, "DeskFeedProbeHookStatus"));
        check(set_tray != nullptr && post != nullptr && retire != nullptr && hook_status != nullptr,
              "hookevents: the probe entries resolve");
        if (set_tray == nullptr || post == nullptr || retire == nullptr || hook_status == nullptr) {
            api.unload();
            break;
        }
        check(set_tray(ui.window.load()) == 1,
              "hookevents: the synthetic window is accepted as the same-thread ack target");

        DWORD loaded = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &loaded);
        check(cleanup_event_exists(),
              "hookevents: the named cleanup event exists while the payload is mapped");

        post(ui.thread_id.load(), 3000);
        DWORD posted_handles = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &posted_handles);
        gp::HookProbeStatus armed{};
        armed.size = gp::kProbeStatusSize;
        armed.version = gp::kProbeStatusVersion;
        check(hook_status(&armed, gp::kProbeStatusSize) == 1 && armed.retired == 1 &&
                  armed.hook_installed == 0 && armed.in_flight == 0 &&
                  armed.completion_expected == 1 && armed.dangling == 0,
              "hookevents: the hook really went in, self-removed, and fired the completion event");

        check(retire(3000) == 1,
              "hookevents: the completion event + same-thread WM_NULL ack gate is really proven");
        gp::HookProbeStatus after{};
        after.size = gp::kProbeStatusSize;
        after.version = gp::kProbeStatusVersion;
        check(hook_status(&after, gp::kProbeStatusSize) == 1 && after.completion_expected == 0 &&
                  after.dangling == 0,
              "hookevents: the gate is closed and nothing dangles before the unload");

        DWORD armed_handles = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &armed_handles);

        check(api.unload(), "hookevents: FreeLibrary succeeds after the gate was proven");
        check(!cleanup_event_exists(), "hookevents: the named cleanup event is gone after FreeLibrary");
        DWORD released = 0;
        ::GetProcessHandleCount(::GetCurrentProcess(), &released);
        std::printf("hookevents: cycle %d loaded=%lu posted=%lu armed=%lu released=%lu\n", cycle,
                    static_cast<unsigned long>(loaded),
                    static_cast<unsigned long>(posted_handles),
                    static_cast<unsigned long>(armed_handles),
                    static_cast<unsigned long>(released));
        if (cycle == 0) {
            // 首轮会额外产生一次性的进程级状态（装载器 + user32 各一次）；用它之后的稳态值当参考。
            plateau = released;
            continue;
        }
        if (cycle == 1) {
            loaded_reference = loaded;
        } else {
            check(loaded == loaded_reference,
                  "hookevents: the load path itself adds the same handles every cycle");
        }
        // 稳态实测：装载 +6、钩子门 +2（就是那两条无名事件）、卸载 -8，净 0。那 +6 里只有 4 个是本
        // 模块的（命名清理事件 + 守卫 mutex/event/mapping）：同一次循环里 `DllMain(DETACH)` 只关这 4 个
        // （没有钩子动作时），而句柄数仍然回到原值 ⇒ 另外 2 个由系统侧开、也由系统侧在 `FreeLibrary`
        // 时关（带清单模块的装载器/激活上下文开销），不是本模块的归属。
        check(armed_handles == loaded + 2,
              "hookevents: the two unnamed hook events really exist before the unload");
        check(released == plateau,
              "hookevents: DETACH returns the unnamed hook events (no per-cycle growth)");
    }
    ui.shutdown();
    return child_result("hookevents");
}

// ---------------------------------------------------------------------------
// R5 的托盘目标覆盖（`--feed` 那条路）
// ---------------------------------------------------------------------------
//
// `DeskFeedProbeSetTrayWindow` 登记的是"负载要把帧投进哪个窗口"的那个 T；attach 必须真的认它，而且
// 登记之后**绝不**回退去找真实的 `Shell_TrayWnd`。两个子用例都在**本进程**里给机械判据（不查、不碰
// 真实托盘窗口，不起可见 GUI，不动光标）：
//
//   * 判据不是某个标志位，而是收尾那次 `WM_NULL` 的身份复验：`hook_post_retire` 要求**确认窗口的
//     属主线程 == 这次选定的 T 的线程**。子用例先在目标线程上真装一次钩子（`DeskFeedProbePostApply`
//     加 `DeskFeedProbeHookStatus` 证明真的走过、门是开的），再 `DeskFeedStop`：
//       - 覆盖被消费 ⇒ T 就是那个岛 ⇒ 线程一致 ⇒ Stop 1；
//       - 覆盖没被消费（回退真实 `Shell_TrayWnd`）⇒ T 是 explorer 的线程、确认窗口是自己的 ⇒ Stop 0。
//   * 负例：登记之后把岛销毁再 Start ⇒ attach 必须直接失败（不回退）；确认窗口换成另一个**还活着**
//     的 own 窗口，再照上面装一次钩子 ⇒ Stop 仍必须是 1（回退那条路会选成真实托盘 ⇒ 0）。
//
// 这一个子用例返回 Stop 的返回值；-1 表示前置步骤没走到（调用方据此判红）。
DWORD probe_target_round(SyntheticUiThread& ui, deskfeed::DisplayFrameProducer& producer, HWND island,
                         bool destroy_island, HWND live_ack, std::wstring const& scope,
                         std::string const& tag) {
    using SetTrayFn = int(__stdcall*)(HWND);
    using SetHostFn = void(__stdcall*)(HWND);
    using PostFn = int(__stdcall*)(unsigned, unsigned);
    using StatusFn = int(__stdcall*)(gp::HookProbeStatus*, unsigned);

    PayloadApi api;
    check(api.load(L"deskfeed-payload-test.dll"), (tag + ": the self-test payload loads").c_str());
    if (api.module == nullptr) {
        return static_cast<DWORD>(-1);
    }
    auto const set_tray =
        reinterpret_cast<SetTrayFn>(::GetProcAddress(api.module, "DeskFeedProbeSetTrayWindow"));
    auto const set_host =
        reinterpret_cast<SetHostFn>(::GetProcAddress(api.module, "DeskFeedProbeSetHost"));
    auto const post =
        reinterpret_cast<PostFn>(::GetProcAddress(api.module, "DeskFeedProbePostApply"));
    auto const hook_status =
        reinterpret_cast<StatusFn>(::GetProcAddress(api.module, "DeskFeedProbeHookStatus"));

    bool ok = set_tray != nullptr && set_host != nullptr && post != nullptr && hook_status != nullptr;
    check(ok, (tag + ": the probe entries resolve").c_str());
    if (ok) {
        ok = set_tray(island) == 1;
        check(ok, (tag + ": R5 accepts this process's own island window").c_str());
    }
    if (ok && destroy_island) {
        ok = ui.destroy_second_window();
        check(ok, (tag + ": the registered island is really gone before Start").c_str());
    }
    if (ok && live_ack != nullptr) {
        // 把"帧已弹掉"的确认窗口挪到另一个**还活着**的 own 窗口上：负例要单独咬"T 没有被选错"。
        set_host(live_ack);
    }
    if (ok) {
        DWORD const started = api.start(const_cast<wchar_t*>(scope.c_str()));
        std::printf("%s: Start -> %lu\n", tag.c_str(), static_cast<unsigned long>(started));
        ok = started == 1;
        check(ok, (tag + ": Start accepts").c_str());
    }
    if (ok) {
        // **必须**让生产者发一帧有效快照：没有可读帧时负载的 `feed_decide` 会立刻判 Remove 并退出
        // 消息循环（负载线程会在我们装钩子之前就自己退休，那这条用例就变成空转的假绿）。
        deskfeed::Frame frame;
        frame.generation = 1;
        frame.produced_at_ms = ::GetTickCount64();
        frame.kind = deskfeed::ViewKind::Content;
        deskfeed::FrameItem item;
        item.id = "probe";
        item.label = "probe";
        item.value = "1";
        item.state = deskfeed::ItemState::Ok;
        frame.items.push_back(item);
        bool const published = producer.publish(frame, static_cast<long long>(::GetTickCount64()));
        check(published, (tag + ": the producer published a complete frame").c_str());
        if (ok) {
            ok = published;
        }
    }
    if (ok) {
        bool attached = false;
        for (int attempt = 0; attempt < 500 && !attached; ++attempt) {
            deskfeed::payload::PayloadStatusPod pod{};
            attached = query_status(api.get, pod) && pod.mailbox_attached == 1;
            if (!attached) {
                ::Sleep(10);
            }
        }
        check(attached, (tag + ": the payload attached the display mailbox").c_str());
        ok = attached;
    }

    DWORD stop_code = static_cast<DWORD>(-1);
    if (ok) {
        // attach 的 T 选择就发生在邮箱挂上之后的同一条线程上；给它一点时间走完这一步。
        ::Sleep(300);
        post(ui.thread_id.load(), 3000);
        gp::HookProbeStatus gate{};
        gate.size = gp::kProbeStatusSize;
        gate.version = gp::kProbeStatusVersion;
        bool const gate_open = hook_status(&gate, gp::kProbeStatusSize) == 1 && gate.retired == 1 &&
                               gate.hook_installed == 0 && gate.in_flight == 0 &&
                               gate.completion_expected == 1 && gate.dangling == 0;
        check(gate_open, (tag + ": a real hook went in on the target thread and the gate is open")
                             .c_str());
        if (gate_open) {
            stop_code = api.stop_ms(5000);
        }
    }
    if (api.module != nullptr) {
        // 与探针宿主同一口径：把这一轮剩下的加载器引用逐个还回去并计数（子进程随即退出，所以把
        // 别人的引用也一起还掉是安全的），同时如实打印模块是否真的从映射表里消失。
        int references = 0;
        while (::FreeLibrary(api.module)) {
            ++references;
        }
        api.module = nullptr;
        std::printf("%s: module references at the end: %d (%s)\n", tag.c_str(), references,
                    ::GetModuleHandleW(sibling_file_path(L"deskfeed-payload-test.dll").c_str()) ==
                            nullptr
                        ? "fully unloaded"
                        : "still mapped");
    }
    return stop_code;
}

int child_probe_tray_target() {
    SyntheticUiThread ui;
    check(ui.start(), "probtray: the hidden synthetic UI thread owns its own windows");
    if (ui.window.load() == nullptr || ui.second.load() == nullptr) {
        ui.shutdown();
        return child_result("probtray");
    }

    std::wstring const scope = L"nativeprobtray";
    deskfeed::DisplayFrameProducer producer;
    std::string producer_error;
    check(producer.create(scope, nullptr, producer_error),
          "probtray: the test process owns a display mailbox producer");
    if (!producer.created()) {
        std::printf("      producer error: %s\n", producer_error.c_str());
        ui.shutdown();
        return child_result("probtray");
    }

    DWORD const consumed = probe_target_round(ui, producer, ui.window.load(), /*destroy_island=*/false,
                                              /*live_ack=*/nullptr, scope, "probtray");
    check(consumed == 1,
          "probtray: Stop retires, so the T attach chose is the registered own island");

    producer.close();
    ui.shutdown();
    return child_result("probtray");
}

// 负例：登记之后岛没了 ⇒ attach 必须失败关闭，**绝不**回退真实 `Shell_TrayWnd`。确认窗口换成另一个
// 还活着的 own 窗口，再装一次钩子，所以回退那条路会因为"T 的线程 != 确认窗口的线程"而让 Stop 返回 0。
int child_probe_tray_target_dead() {
    SyntheticUiThread ui;
    check(ui.start(), "probtraydead: the hidden synthetic UI thread owns its own windows");
    if (ui.window.load() == nullptr || ui.second.load() == nullptr) {
        ui.shutdown();
        return child_result("probtraydead");
    }

    std::wstring const scope = L"nativeprobtraydead";
    deskfeed::DisplayFrameProducer producer;
    std::string producer_error;
    check(producer.create(scope, nullptr, producer_error),
          "probtraydead: the test process owns a display mailbox producer");
    if (!producer.created()) {
        std::printf("      producer error: %s\n", producer_error.c_str());
        ui.shutdown();
        return child_result("probtraydead");
    }

    DWORD const no_fallback = probe_target_round(ui, producer, ui.second.load(), /*destroy_island=*/true,
                                                 /*live_ack=*/ui.window.load(), scope, "probtraydead");
    check(no_fallback == 1,
          "probtraydead: a dead registration never falls back to the real tray");

    producer.close();
    ui.shutdown();
    return child_result("probtraydead");
}

// 父进程侧的驱动器：每个场景一个**新进程**（守卫对象名按 pid+creation，DllMain 每进程只跑一次）。
bool run_guard_child(wchar_t const* scenario, char const* label) {
    wchar_t exe[MAX_PATH]{};
    DWORD const length = ::GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    if (length == 0 || length >= std::size(exe)) {
        check(false, label);
        return false;
    }
    std::wstring command = L"\"" + std::wstring(exe, length) + L"\" --guard-child " + scenario;
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                         &startup, &process) == FALSE) {
        check(false, label);
        return false;
    }
    ::CloseHandle(process.hThread);
    DWORD const wait = ::WaitForSingleObject(process.hProcess, 60000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) {
        ::GetExitCodeProcess(process.hProcess, &code);
    }
    ::CloseHandle(process.hProcess);
    bool const ok = wait == WAIT_OBJECT_0 && code == 0;
    check(ok, label);
    return ok;
}

void test_guard_child_scenarios() {
    if (!name_matches("guardchild")) {
        return;
    }
    struct Case {
        wchar_t const* scenario;
        char const* label;
    };
    Case const cases[] = {
        {L"native-create", "guard-child native-create: Native's one-time init retires normally"},
        {L"actor-inited", "guard-child actor-inited: a valid header + PendingTid{self} retires"},
        {L"event-signaled", "guard-child event-signaled: the sticky event refuses retirement"},
        {L"bad-header", "guard-child bad-header: an unrecognised header refuses retirement"},
        {L"wrong-type", "guard-child wrong-type: a wrong kernel object type refuses retirement"},
        {L"readonly-zero", "guard-child readonly-zero: a zero header refuses retirement"},
        {L"abandoned-pending", "guard-child abandoned-pending: PendingNoTid stays uncertain"},
        {L"abandoned-idle", "guard-child abandoned-idle: the actor's provable clear works"},
        {L"actor-refs", "guard-child actor-refs: references survive FreeLibrary until the actor closes"},
        {L"reload", "guard-child reload: no keeper reference leaks per reload"},
        {L"hookevents", "guard-child hookevents: the unnamed hook events are returned on DETACH"},
        {L"probtray",
         "guard-child probtray: the R5 registered island really is the T attach chose"},
        {L"probtraydead",
         "guard-child probtraydead: a dead R5 registration never falls back to the real tray"},
    };
    for (Case const& item : cases) {
        run_guard_child(item.scenario, item.label);
    }
}

int run_guard_child_scenario(char const* scenario) {
    struct Entry {
        char const* name;
        int (*run)();
    };
    Entry const entries[] = {
        {"native-create", &child_native_create},
        {"actor-inited", &child_actor_inited_self_pending},
        {"event-signaled", &child_event_signaled},
        {"bad-header", &child_bad_header},
        {"wrong-type", &child_wrong_object_type},
        {"readonly-zero", &child_readonly_zero_header},
        {"abandoned-pending", &child_abandoned_pending_no_tid},
        {"abandoned-idle", &child_abandoned_idle_recovered},
        {"actor-refs", &child_actor_held_references},
        {"reload", &child_reload_objects_gone},
        {"hookevents", &child_hook_events_released},
        {"probtray", &child_probe_tray_target},
        {"probtraydead", &child_probe_tray_target_dead},
    };
    for (Entry const& entry : entries) {
        if (std::strcmp(entry.name, scenario) == 0) {
            return entry.run();
        }
    }
    std::printf("guard-child: unknown scenario %s\n", scenario);
    return 2;
}

// --- 进程内：纯函数、对象名、校验和向量 ---------------------------------------

void test_guard_record_contract() {
    if (!name_matches("guardrecord")) {
        return;
    }
    check(sizeof(GuardRecord) == gp::kGuardRecordSize, "guard: the record is exactly 40 bytes");
    check(gp::kGuardRecordVersion == 1, "guard: the record version is frozen at 1");

    // 对象名：唯一格式化函数、完整 16 位小写 hex、容量不足绝不截断。
    wchar_t name[gp::kGuardNameCapacity]{};
    check(gp::guard_format_object_name(name, std::size(name), gp::kGuardSuffixRecord, 4321,
                                       0x0123456789ABCDEFull),
          "guard: the object name can be formatted");
    check(std::wstring(name) == L"Local\\DeskFeed-payload-guard-4321-0123456789abcdef-record",
          "guard: the name is pid + 16 lowercase hex + suffix");
    wchar_t tiny[16]{};
    check(!gp::guard_format_object_name(tiny, std::size(tiny), gp::kGuardSuffixRecord, 4321, 1),
          "guard: a too-small buffer is refused instead of truncated");
    check(!gp::guard_format_object_name(nullptr, 128, gp::kGuardSuffixRecord, 1, 1),
          "guard: a null buffer is refused");
    check(!gp::guard_format_object_name(name, std::size(name), nullptr, 1, 1),
          "guard: a null suffix is refused");
    // 恰好 kGuardNameCapacity 个字符且没有 NUL 时，长度必须停在容量上（不许读到缓冲之外）。
    wchar_t full[gp::kGuardNameCapacity];
    for (std::size_t index = 0; index < std::size(full); ++index) {
        full[index] = L'x';
    }
    check(gp::guard_name_length(full) == gp::kGuardNameCapacity,
          "guard: name length stops at the capacity boundary");

    // 校验和向量（冻结）：init(seq=1) / PendingNoTid(seq=2) / PendingTid(0x1234, seq=3)。
    GuardRecord record{};
    gp::guard_init_new_record(record, 1);
    check(record.size == gp::kGuardRecordSize && record.version == gp::kGuardRecordVersion,
          "guard: init writes the immutable header");
    check(gp::guard_header_ok(record), "guard: init leaves a valid header");
    check(gp::guard_snapshot_ok(record), "guard: init leaves a valid snapshot");
    check(record.checksum == 3603915268u, "guard: init(seq=1) checksum vector");
    check(gp::guard_classify(record) == gp::GuardSnapshot::Idle, "guard: init classifies as Idle");

    gp::guard_make_pending_no_tid(record, 2);
    check(record.checksum == 795202630u, "guard: PendingNoTid(seq=2) checksum vector");
    check(gp::guard_classify(record) == gp::GuardSnapshot::PendingNoTid,
          "guard: PendingNoTid classifies as itself");
    check(record.size == gp::kGuardRecordSize && record.version == gp::kGuardRecordVersion,
          "guard: writing the snapshot never touches the immutable header");

    gp::guard_make_pending_tid(record, 0x1234, 3);
    check(record.checksum == 1666159386u, "guard: PendingTid(0x1234, seq=3) checksum vector");
    check(gp::guard_classify(record) == gp::GuardSnapshot::PendingTid,
          "guard: PendingTid classifies as itself");

    gp::guard_make_idle(record, 4);
    check(record.tid == 0 && gp::guard_classify(record) == gp::GuardSnapshot::Idle,
          "guard: Idle clears the tid");

    // 负例：任何一处不认都必须归 Unknown（Native 只认 header，actor 认整体快照）。
    GuardRecord broken = record;
    broken.version = gp::kGuardRecordVersion + 1;
    check(!gp::guard_header_ok(broken), "guard: a wrong version fails the header");
    check(gp::guard_classify(broken) == gp::GuardSnapshot::Unknown,
          "guard: a wrong version classifies as Unknown");
    broken = record;
    broken.size = 0;
    check(!gp::guard_header_ok(broken), "guard: a zero size fails the header");
    broken = record;
    broken.checksum ^= 0x1u;
    check(!gp::guard_snapshot_ok(broken), "guard: a wrong checksum fails the snapshot");
    check(gp::guard_header_ok(broken), "guard: the header is still fine with a wrong checksum");
    broken = record;
    broken.reserved = 1;
    check(!gp::guard_snapshot_ok(broken), "guard: a non-zero reserved fails the snapshot");
    broken = record;
    broken.reserved2 = 1;
    check(!gp::guard_snapshot_ok(broken), "guard: a non-zero reserved2 fails the snapshot");
    broken = record;
    broken.state = gp::kGuardStatePendingTid + 1;
    check(!gp::guard_snapshot_ok(broken), "guard: an unknown state fails the snapshot");
    broken = record;
    gp::guard_make_pending_tid(broken, 0, 5);
    check(!gp::guard_snapshot_ok(broken), "guard: PendingTid without a tid fails the snapshot");
    broken = record;
    gp::guard_make_pending_tid(broken, gp::kGuardMaxTid + 1, 5);
    check(!gp::guard_snapshot_ok(broken), "guard: a tid beyond 32 bits fails the snapshot");
    broken = record;
    gp::guard_make_idle(broken, 5);
    broken.tid = 7;
    broken.checksum = gp::guard_snapshot_fnv(broken);
    check(!gp::guard_snapshot_ok(broken), "guard: Idle with a tid fails the snapshot");
}

// --- 进程内：恢复分支表（真线程 + 官方 Toolhelp，不 mock） ---------------------

bool test_tid_present(std::uint32_t process_id, std::uint32_t thread_id) {
    HANDLE const snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return true;  // 查不出来按"仍在"（fail closed）
    }
    bool found = false;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    if (::Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID == process_id && entry.th32ThreadID == thread_id) {
                found = true;
                break;
            }
        } while (::Thread32Next(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
    return found;
}

void test_guard_recovery_table() {
    if (!name_matches("guardrecovery")) {
        return;
    }
    // 真线程制造"TID 在场/离场"：线程等在事件上 ⇒ 在场；置位并等它退出 ⇒ 离场。
    HANDLE const gate = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE const thread =
        ::CreateThread(nullptr, 0, [](void* parameter) -> DWORD {
            ::WaitForSingleObject(static_cast<HANDLE>(parameter), 5000);
            return 0;
        }, gate, 0, nullptr);
    check(gate != nullptr && thread != nullptr, "guard: a real thread exists for the tid probe");
    if (gate != nullptr && thread != nullptr) {
        DWORD const tid = ::GetThreadId(thread);
        bool const present = test_tid_present(::GetCurrentProcessId(), tid);
        check(present, "guard: Toolhelp really sees the live thread in this process");

        GuardRecord pending{};
        gp::guard_init_new_record(pending, 1);
        gp::guard_make_pending_tid(pending, tid, 2);
        check(gp::guard_recovery(gp::guard_classify(pending), gp::GuardTidProbe::Present) ==
                  gp::GuardRecovery::KeepUncertain,
              "guard: PendingTid with a live tid stays uncertain");

        ::SetEvent(gate);
        ::WaitForSingleObject(thread, 5000);
        ::CloseHandle(thread);
        bool const absent = !test_tid_present(::GetCurrentProcessId(), tid);
        check(absent, "guard: Toolhelp really stops seeing the exited thread");
        check(gp::guard_recovery(gp::guard_classify(pending),
                                 absent ? gp::GuardTidProbe::Absent : gp::GuardTidProbe::Present) ==
                  gp::GuardRecovery::ClearAndContinue,
              "guard: PendingTid with an absent tid is provable");
    }
    if (gate != nullptr) {
        ::CloseHandle(gate);
    }

    GuardRecord idle{};
    gp::guard_init_new_record(idle, 1);
    check(gp::guard_recovery(gp::GuardSnapshot::Idle, gp::GuardTidProbe::NotQueried) ==
              gp::GuardRecovery::ClearAndContinue,
          "guard: Idle is provable without a tid lookup");

    GuardRecord pending_no_tid{};
    gp::guard_init_new_record(pending_no_tid, 1);
    gp::guard_make_pending_no_tid(pending_no_tid, 2);
    check(gp::guard_recovery(gp::GuardSnapshot::PendingNoTid, gp::GuardTidProbe::Absent) ==
              gp::GuardRecovery::KeepUncertain,
          "guard: PendingNoTid stays uncertain even with an absent probe");
    check(gp::guard_recovery(gp::GuardSnapshot::PendingTid, gp::GuardTidProbe::LookupFailed) ==
              gp::GuardRecovery::KeepUncertain,
          "guard: a failed tid lookup stays uncertain");
    check(gp::guard_recovery(gp::GuardSnapshot::Unknown, gp::GuardTidProbe::Absent) ==
              gp::GuardRecovery::KeepUncertain,
          "guard: an unknown snapshot stays uncertain");
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring audit_path;
    char const* guard_child = nullptr;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--filter") == 0 && index + 1 < argc) {
            g_filter = argv[++index];
        } else if (std::strcmp(argv[index], "--audit") == 0 && index + 1 < argc) {
            audit_path = widen(argv[++index]);
        } else if (std::strcmp(argv[index], "--guard-child") == 0 && index + 1 < argc) {
            guard_child = argv[++index];
        }
    }

    // 守卫场景必须在**新进程**里跑：对象名按 pid+creation 命名，`DllMain` 每个进程只 attach 一次。
    if (guard_child != nullptr) {
        return run_guard_child_scenario(guard_child);
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
        test_status_fail_closed();
        test_retire_glue();
        test_guard_record_contract();
        test_guard_recovery_table();
        test_guard_child_scenarios();
    }

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
